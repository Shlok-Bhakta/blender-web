/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Touch, pen and on-screen modifier input for Blender Web, after the 3D Modeling
 * Thingy iOS port. Gestures are recognized in GHOST (GHOST_TouchGestures.cc);
 * this page side forwards raw pointers through a ring in shared Wasm memory and
 * draws the virtual cursor, the Shift/Ctrl/Alt belt and the text entry sheet.
 * Layout constants mirror GHOST_WebTouch.hh. */
if (typeof window !== 'undefined') {
  (() => {
    const Kind = {TouchDown:1, TouchMove:2, TouchUp:3, TouchCancel:4, PenDown:5, PenMove:6, PenUp:7,
      PenHover:8, ModifierDown:9, ModifierUp:10, ModifierCancel:11, TextCommit:12, TextCancel:13, Blur:14};
    const Status = {X:0, Y:1, Flags:2, Modifiers:3};
    const FlagCursorVisible = 1, FlagTouchActive = 2;
    const ModeOff = 0, ModeHeld = 1, ModeLatched = 2, ModeLocked = 3;
    let ring = 0, capacity = 0, recordInts = 0, status = 0, text = 0, textBytes = 16384, epoch = 0;
    let i32, f32, u8;
    let canvas, cursor, belt, handle, sheet, sheetInput;
    let touchSeen = false, hardwareKeyboard = false, lastInput = 'mouse';
    let editing = false;
    const keys = [];

    const views = () => {
      const buffer = wasmMemory.buffer;
      if (i32?.buffer !== buffer) {
        i32 = new Int32Array(buffer);
        f32 = new Float32Array(buffer);
        u8 = new Uint8Array(buffer);
      }
    };
    const clock = () => performance.timeOrigin + performance.now() - epoch;

    const write = (kind, id = 0, x = 0, y = 0, pressure = 1, tiltX = 0, tiltY = 0) => {
      if (!ring) return;
      views();
      const header = ring >> 2;
      const index = Atomics.load(i32, header) >>> 0;
      const base = header + 4 + (index % capacity) * recordInts;
      i32[base] = kind;
      i32[base + 1] = id;
      f32[base + 2] = x;
      f32[base + 3] = y;
      f32[base + 4] = pressure;
      f32[base + 5] = tiltX;
      f32[base + 6] = tiltY;
      i32[base + 7] = Math.round(clock());
      Atomics.store(i32, header, (index + 1) | 0);
    };

    const local = event => {
      const rect = canvas.getBoundingClientRect();
      return [event.clientX - rect.left, event.clientY - rect.top];
    };

    const onPointer = event => {
      if (event.pointerType === 'mouse') {
        if (event.type === 'pointermove' && (event.movementX || event.movementY)) lastInput = 'mouse';
        else if (event.type === 'pointerdown') lastInput = 'mouse';
        return;
      }
      /* Keep the browser from scrolling, zooming or synthesizing mouse events. */
      event.preventDefault();
      event.stopImmediatePropagation();
      const [x, y] = local(event);
      if (event.pointerType === 'pen') {
        lastInput = 'pen';
        const pressure = event.pressure || (event.buttons ? 0.5 : 0);
        const tiltX = (event.tiltX || 0) / 90, tiltY = (event.tiltY || 0) / 90;
        switch (event.type) {
          case 'pointerdown':
            canvas.setPointerCapture?.(event.pointerId);
            write(Kind.PenDown, event.button === 2 || (event.buttons & 2) ? 1 : 0, x, y, pressure, tiltX, tiltY);
            break;
          case 'pointermove':
            write(event.buttons ? Kind.PenMove : Kind.PenHover, 0, x, y, pressure, tiltX, tiltY);
            break;
          case 'pointerup':
          case 'pointercancel':
            write(Kind.PenUp, 0, x, y, pressure, tiltX, tiltY);
            break;
        }
      }
      else {
        lastInput = 'touch';
        if (!touchSeen) {
          touchSeen = true;
          updateBeltVisibility();
        }
        switch (event.type) {
          case 'pointerdown':
            canvas.setPointerCapture?.(event.pointerId);
            write(Kind.TouchDown, event.pointerId, x, y);
            break;
          case 'pointermove':
            write(Kind.TouchMove, event.pointerId, x, y);
            break;
          case 'pointerup':
            write(Kind.TouchUp, event.pointerId, x, y);
            break;
          case 'pointercancel':
            write(Kind.TouchCancel, event.pointerId, x, y);
            break;
        }
      }
    };

    const style = `
#blender-touch-cursor{position:fixed;left:0;top:0;width:22px;height:22px;pointer-events:none;z-index:20;display:none;
  will-change:transform;filter:drop-shadow(0 1px 1.5px rgba(0,0,0,.6))}
#blender-touch-belt{position:fixed;z-index:21;display:none;align-items:center;gap:4px;padding:4px;height:44px;
  border-radius:26px;background:rgba(30,30,30,.82);-webkit-backdrop-filter:blur(12px);backdrop-filter:blur(12px);
  touch-action:none;user-select:none;-webkit-user-select:none;-webkit-touch-callout:none;transition:opacity .15s}
#blender-touch-belt.left{flex-direction:row}#blender-touch-belt.right{flex-direction:row-reverse}
#blender-touch-belt button{all:unset;box-sizing:border-box;touch-action:none;display:grid;place-items:center;
  color:#eee;font:600 14px system-ui;-webkit-tap-highlight-color:transparent}
#blender-touch-belt .handle{width:44px;height:44px;border-radius:22px;background:rgba(255,255,255,.12)}
#blender-touch-belt .handle.dragging{background:rgba(255,255,255,.25)}
#blender-touch-belt .key{position:relative;width:58px;height:44px;border-radius:12px;background:rgba(255,255,255,.10)}
#blender-touch-belt:not(.expanded) .key{display:none}
#blender-touch-belt .key.held,#blender-touch-belt .key.latched,#blender-touch-belt .key.locked{background:#e99546;color:#111}
#blender-touch-belt .key.locked::after{content:'';position:absolute;bottom:3px;left:50%;width:14px;height:2px;
  margin-left:-7px;border-radius:1px;background:#111}
#blender-text-sheet{position:fixed;inset:0;z-index:30;display:none;background:rgba(0,0,0,.35)}
#blender-text-sheet form{position:absolute;left:50%;top:max(16px,env(safe-area-inset-top));width:min(560px,calc(100% - 24px));
  transform:translateX(-50%);display:grid;grid-template-columns:auto 1fr auto;gap:8px;align-items:center;padding:10px;
  border-radius:14px;background:#2b2b2b;box-shadow:0 8px 30px rgba(0,0,0,.5)}
#blender-text-sheet input{-webkit-user-select:text;user-select:text;min-width:0;font:16px system-ui;color:#eee;background:#1d1d1d;border:1px solid #555;border-radius:8px;padding:9px 10px}
#blender-text-sheet button{font:600 15px system-ui;border:0;border-radius:8px;padding:10px 14px;color:#eee;background:#444}
#blender-text-sheet button[type=submit]{background:#e99546;color:#111}`;

    const cornerKey = 'blenderWebTouchBeltCorner';
    let corner = Number(localStorage.getItem(cornerKey) ?? 3);
    const placeBelt = () => {
      const left = corner === 0 || corner === 2, top = corner < 2;
      belt.classList.toggle('left', left);
      belt.classList.toggle('right', !left);
      Object.assign(belt.style, {
        left: left ? 'calc(12px + env(safe-area-inset-left))' : '',
        right: left ? '' : 'calc(12px + env(safe-area-inset-right))',
        top: top ? 'calc(12px + env(safe-area-inset-top))' : '',
        /* Keep Blender's status bar clear. */
        bottom: top ? '' : 'calc(40px + env(safe-area-inset-bottom))',
        transform: '',
      });
    };
    const updateBeltVisibility = () => {
      if (belt) belt.style.display = touchSeen && !hardwareKeyboard ? 'flex' : 'none';
    };

    const buildBelt = () => {
      belt = document.createElement('div');
      belt.id = 'blender-touch-belt';
      belt.setAttribute('role', 'toolbar');
      belt.setAttribute('aria-label', 'Modifier keys');
      handle = document.createElement('button');
      handle.className = 'handle';
      handle.setAttribute('aria-label', 'Show Shift, Ctrl and Alt. Drag to move to another corner.');
      handle.innerHTML = '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
        'stroke-width="1.8" stroke-linecap="round"><rect x="2.5" y="6" width="19" height="12" rx="2"/>' +
        '<path d="M6 10h.01M10 10h.01M14 10h.01M18 10h.01M8 14h8"/></svg>';
      belt.append(handle);
      ['Shift', 'Ctrl', 'Alt'].forEach((name, id) => {
        const key = document.createElement('button');
        key.className = 'key';
        key.textContent = name;
        key.setAttribute('aria-label', name);
        key.addEventListener('pointerdown', event => {
          event.preventDefault();
          key.setPointerCapture(event.pointerId);
          write(Kind.ModifierDown, id);
        });
        key.addEventListener('pointerup', event => {
          event.preventDefault();
          write(Kind.ModifierUp, id);
        });
        key.addEventListener('pointercancel', () => write(Kind.ModifierCancel, id));
        keys.push(key);
        belt.append(key);
      });
      let drag = null;
      handle.addEventListener('pointerdown', event => {
        event.preventDefault();
        handle.setPointerCapture(event.pointerId);
        drag = {x:event.clientX, y:event.clientY, moved:false};
      });
      handle.addEventListener('pointermove', event => {
        if (!drag) return;
        const dx = event.clientX - drag.x, dy = event.clientY - drag.y;
        if (!drag.moved && Math.hypot(dx, dy) < 8) return;
        drag.moved = true;
        handle.classList.add('dragging');
        belt.style.transform = `translate(${dx}px,${dy}px)`;
      });
      const finish = event => {
        if (!drag) return;
        handle.classList.remove('dragging');
        if (drag.moved) {
          /* Snap to the corner of the quarter the belt was dropped in. */
          const rect = belt.getBoundingClientRect();
          const cx = rect.left + rect.width / 2, cy = rect.top + rect.height / 2;
          corner = (cy < innerHeight / 2 ? 0 : 2) + (cx < innerWidth / 2 ? 0 : 1);
          localStorage.setItem(cornerKey, String(corner));
          placeBelt();
        }
        else if (event.type === 'pointerup') {
          belt.classList.toggle('expanded');
        }
        drag = null;
      };
      handle.addEventListener('pointerup', finish);
      handle.addEventListener('pointercancel', finish);
      document.body.append(belt);
      placeBelt();
      updateBeltVisibility();
    };

    const buildCursor = () => {
      cursor = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      cursor.id = 'blender-touch-cursor';
      cursor.setAttribute('viewBox', '0 0 22 22');
      cursor.innerHTML = '<path d="M2 1.5v16.2l4.4-4.1 2.8 6.6 3-1.3-2.8-6.4h6.1z" fill="#fff" stroke="#111" ' +
        'stroke-width="1.3" stroke-linejoin="round"/>';
      document.body.append(cursor);
    };

    const closeSheet = () => {
      sheet.style.display = 'none';
      sheetInput.blur();
      editing = false;
      canvas.focus({preventScroll:true});
    };
    const buildSheet = () => {
      sheet = document.createElement('div');
      sheet.id = 'blender-text-sheet';
      sheet.innerHTML = '<form autocomplete="off"><button type="button" class="cancel">Cancel</button>' +
        '<input aria-label="Text" enterkeyhint="done" autocapitalize="off" autocorrect="off" spellcheck="false">' +
        '<button type="submit">Done</button></form>';
      sheetInput = sheet.querySelector('input');
      const form = sheet.querySelector('form');
      /* Typing here must not also reach Blender's window-level key listeners. */
      for (const type of ['keydown', 'keyup', 'keypress']) {
        sheet.addEventListener(type, event => event.stopPropagation());
      }
      for (const type of ['pointerdown', 'pointermove', 'pointerup']) {
        sheet.addEventListener(type, event => event.stopPropagation());
      }
      form.addEventListener('submit', event => {
        event.preventDefault();
        if (!editing) return;
        views();
        const bytes = new TextEncoder().encode(sheetInput.value);
        const length = Math.min(bytes.length, textBytes - 1);
        u8.set(bytes.subarray(0, length), text);
        write(Kind.TextCommit, length);
        closeSheet();
      });
      sheet.querySelector('.cancel').addEventListener('click', () => {
        if (editing) write(Kind.TextCancel);
        closeSheet();
      });
      document.body.append(sheet);
    };

    Module.webTextEditBegin = (value, numeric) => {
      /* Desktop typing goes straight to Blender; only touch edits use the sheet. */
      if (!sheet || (lastInput !== 'touch' && lastInput !== 'pen')) return;
      editing = true;
      sheetInput.value = value;
      sheetInput.setAttribute('inputmode', 'text');
      sheetInput.dataset.numeric = numeric ? 'true' : 'false';
      sheet.style.display = 'block';
      sheetInput.focus({preventScroll:true});
      sheetInput.select();
    };
    Module.webTextEditEnd = () => {
      if (editing) closeSheet();
    };

    const frame = () => {
      requestAnimationFrame(frame);
      if (!status) return;
      views();
      const base = status >> 2;
      const flags = f32[base + Status.Flags] | 0;
      const x = f32[base + Status.X], y = f32[base + Status.Y];
      const show = (flags & FlagTouchActive) && (flags & FlagCursorVisible) && lastInput === 'touch';
      cursor.style.display = show ? 'block' : 'none';
      if (show) cursor.style.transform = `translate(${x - 2}px,${y - 1.5}px)`;
      const modes = f32[base + Status.Modifiers] | 0;
      keys.forEach((key, i) => {
        const mode = (modes >> (i * 2)) & 3;
        key.classList.toggle('held', mode === ModeHeld);
        key.classList.toggle('latched', mode === ModeLatched);
        key.classList.toggle('locked', mode === ModeLocked);
      });
      if (belt.style.display !== 'none') {
        /* Fade the keys while the cursor is over them, to see what is underneath. */
        const rect = belt.getBoundingClientRect();
        const over = show && x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
        belt.style.opacity = over ? '0.3' : '1';
      }
    };

    /* State for tests and diagnostics. */
    Module.webTouchDebug = () => {
      if (!status) return null;
      views();
      const base = status >> 2;
      return {x:f32[base + Status.X], y:f32[base + Status.Y], flags:f32[base + Status.Flags] | 0,
        modifiers:f32[base + Status.Modifiers] | 0, lastInput, touchSeen, hardwareKeyboard,
        cursorShown:cursor.style.display === 'block', beltShown:belt.style.display !== 'none',
        beltExpanded:belt.classList.contains('expanded'), sheetOpen:editing, sheetValue:sheetInput.value};
    };

    Module.webTouchInstall = (ringPointer, ringCapacity, ringRecordInts, statusPointer, textPointer, epochMs) => {
      ring = ringPointer >>> 0;
      capacity = ringCapacity;
      recordInts = ringRecordInts;
      status = statusPointer >>> 0;
      text = textPointer >>> 0;
      epoch = epochMs;
      canvas = document.getElementById('canvas');
      const head = document.createElement('style');
      head.textContent = style;
      document.head.append(head);
      buildCursor();
      buildBelt();
      buildSheet();
      for (const type of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel']) {
        canvas.addEventListener(type, onPointer, {capture:true, passive:false});
      }
      /* Safari's page pinch zoom and long-press callouts fight the canvas gestures. */
      for (const type of ['gesturestart', 'gesturechange', 'touchstart']) {
        canvas.addEventListener(type, event => event.preventDefault(), {passive:false});
      }
      canvas.style.webkitTouchCallout = 'none';
      canvas.style.webkitUserSelect = 'none';
      window.addEventListener('keydown', event => {
        if (sheet.contains(event.target)) return;
        lastInput = 'keyboard';
        if (!hardwareKeyboard) {
          /* The belt hides while a hardware keyboard is in use, as on iPad. */
          hardwareKeyboard = true;
          updateBeltVisibility();
        }
      }, {capture:true});
      window.addEventListener('blur', () => write(Kind.Blur));
      document.addEventListener('visibilitychange', () => {
        if (document.hidden) write(Kind.Blur);
      });
      requestAnimationFrame(frame);
    };
  })();
}
