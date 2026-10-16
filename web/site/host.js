/* SPDX-License-Identifier: GPL-2.0-or-later */
export function createHost(canvas, renderer, onJob, fail) {
  let module, observer, abort, frame = 0, stopped = false, ratioQuery;
  const invalidate = () => {
    if (stopped || frame || document.hidden || !module?._web_frame) return;
    frame = requestAnimationFrame(() => {
      frame = 0;
      try { module._web_frame(); } catch (error) { stopped = true; fail(error); }
    });
  };
  const resize = () => {
    if (!module || stopped) return;
    const rect = canvas.getBoundingClientRect();
    // Bound pixel cost on Retina displays while respecting the adapter's limits.
    const scale = Math.min(devicePixelRatio || 1, 2, renderer.maxDimension / Math.max(1, rect.width, rect.height));
    const width = Math.max(1, Math.floor(rect.width * scale));
    const height = Math.max(1, Math.floor(rect.height * scale));
    if (canvas.width === width && canvas.height === height && host.pixelRatio === scale) return;
    canvas.width = width;
    canvas.height = height;
    host.pixelRatio = scale;
    module._web_resize(width, height, scale);
    invalidate();
  };
  const watchRatio = () => {
    ratioQuery?.removeEventListener('change', ratioChange);
    ratioQuery = matchMedia(`(resolution: ${devicePixelRatio}dppx)`);
    ratioQuery.addEventListener('change', ratioChange, { once: true });
  };
  const ratioChange = () => { resize(); watchRatio(); };
  const host = {
    diagnostics: {}, job: null, pixelRatio: 1,
    setTitle(title) { document.title = title; },
    draw(matrix) { if (!stopped) renderer.draw(matrix); },
    invalidate,
    jobComplete(checksum, workers) {
      host.job = { checksum, workers, done: true };
      onJob(host.job);
    },
    attach(instance) {
      module = instance;
      abort = new AbortController();
      const listen = (target, type, callback, options = {}) => target.addEventListener(type, callback, { ...options, signal: abort.signal });
      const point = event => {
        const rect = canvas.getBoundingClientRect();
        return [Math.round((event.clientX - rect.left) * host.pixelRatio), Math.round((event.clientY - rect.top) * host.pixelRatio)];
      };
      const pointers = new Set();
      const sendPointer = (event, kind) => {
        const [x, y] = point(event);
        module._web_pointer(0, x, y, 0);
        if (kind) module._web_pointer(kind, x, y, event.button < 0 ? 0 : event.button);
        invalidate();
      };
      listen(canvas, 'pointerdown', event => {
        if (!event.isPrimary) return;
        canvas.focus({ preventScroll: true });
        canvas.setPointerCapture(event.pointerId);
        pointers.add(event.pointerId);
        sendPointer(event, 1);
        event.preventDefault();
      });
      listen(canvas, 'pointermove', event => { if (event.isPrimary) sendPointer(event, 0); });
      listen(canvas, 'pointerup', event => {
        if (!event.isPrimary) return;
        sendPointer(event, 2);
        pointers.delete(event.pointerId);
        if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
      });
      const cancel = event => {
        if (pointers.delete(event.pointerId)) { module._web_blur(); invalidate(); }
      };
      listen(canvas, 'pointercancel', cancel);
      listen(canvas, 'lostpointercapture', cancel);
      listen(canvas, 'contextmenu', event => event.preventDefault());
      let wheelX = 0, wheelY = 0;
      listen(canvas, 'wheel', event => {
        event.preventDefault();
        const factor = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? canvas.clientHeight : 1;
        wheelX += event.deltaX * factor;
        wheelY += event.deltaY * factor;
        for (const axis of [0, 1]) {
          const value = axis ? wheelX : wheelY;
          const ticks = Math.trunc(value / 40);
          if (ticks) {
            module._web_wheel(axis, -ticks);
            if (axis) wheelX -= ticks * 40; else wheelY -= ticks * 40;
          }
        }
        invalidate();
      }, { passive: false });
      const key = (event, down) => {
        // Preserve browser shortcuts and tab navigation.
        if (event.ctrlKey || event.metaKey || event.code === 'Tab') return;
        module.ccall('web_key', null, ['string','string','number','number'],
          [event.code, event.key.length === 1 ? event.key : '', +down, +event.repeat]);
        event.preventDefault();
        invalidate();
      };
      listen(canvas, 'keydown', event => key(event, true));
      listen(canvas, 'keyup', event => key(event, false));
      listen(canvas, 'blur', () => { pointers.clear(); module._web_blur(); invalidate(); });
      listen(window, 'blur', () => { pointers.clear(); module._web_blur(); invalidate(); });
      listen(document, 'visibilitychange', () => {
        if (document.hidden) {
          pointers.clear(); module._web_blur();
          cancelAnimationFrame(frame); frame = 0;
        } else { resize(); invalidate(); }
      });
      listen(window, 'resize', resize);
      observer = new ResizeObserver(resize);
      observer.observe(canvas);
      watchRatio();
      resize();
    },
    detach() {
      stopped = true;
      abort?.abort(); observer?.disconnect();
      ratioQuery?.removeEventListener('change', ratioChange);
      cancelAnimationFrame(frame); frame = 0;
    },
  };
  return host;
}
