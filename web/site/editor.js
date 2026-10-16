/* SPDX-License-Identifier: GPL-2.0-or-later */
// Browser controls for the preview scene. Keep this adapter separate from native Blender UI.
const axes = ['X', 'Y', 'Z'];
const defaults = () => ({ name: 'Cube', transform: [0,0,0,0,0,0,1,1,1], visible: true, selected: true });
const finiteArray = (value, length, limit) => Array.isArray(value) && value.length === length && value.every(n => Number.isFinite(n) && Math.abs(n) <= limit);

export function createEditor(module, host, actions) {
  const $ = selector => document.querySelector(selector);
  const workspace = $('#workspace'), inspectorToggle = $('#inspector-toggle');
  let object = defaults();
  let display = { grid: true, edges: true, orthographic: false };
  let saved = null, undo = [], redo = [];
  let inspectorOpen = !matchMedia('(max-width: 700px)').matches;
  let inspectorWidth = 286, outlinerHeight = 170;
  const fields = $('#transform-fields');
  for (const [group, title] of ['Location', 'Rotation', 'Scale'].entries()) {
    const row = document.createElement('div');
    row.className = 'transform-group';
    row.innerHTML = `<span class="transform-title">${title}</span><div class="transform-values">${axes.map((axis, index) => `<label class="transform-value"><span>${axis}</span><input type="number" step="any" min="${group === 2 ? '-100' : '-10000'}" max="${group === 2 ? '100' : '10000'}" data-transform="${group * 3 + index}" aria-label="${title} ${axis}"></label>`).join('')}</div>`;
    fields.append(row);
  }
  const readObject = () => ({ ...object, transform: Array.from({ length: 9 }, (_, i) => module._web_transform(i)) });
  const camera = () => ({ yaw: module._web_yaw(), pitch: module._web_pitch(), distance: module._web_distance(), target: Array.from({ length: 3 }, (_, i) => module._web_target(i)) });
  const capture = () => ({ format: 'blender-web-preview', version: 1, object: readObject(), camera: camera(), display: { ...display }, layout: { inspectorOpen, inspectorWidth, outlinerHeight } });
  const refreshDirty = () => {
    $('#project-state').textContent = saved === JSON.stringify(capture()) ? 'Saved locally' : 'Unsaved preview';
    const pose = camera();
    let orientation = 'User';
    if (display.orthographic) {
      if (Math.abs(pose.pitch - Math.PI / 2) < 0.0001) orientation = 'Top';
      else if (Math.abs(pose.pitch) < 0.0001 && Math.abs(pose.yaw) < 0.0001) orientation = 'Front';
      else if (Math.abs(pose.pitch) < 0.0001 && Math.abs(pose.yaw - Math.PI / 2) < 0.0001) orientation = 'Right';
    }
    $('#view-label').textContent = `${orientation} ${display.orthographic ? 'orthographic' : 'perspective'}`;
  };
  const syncDisplay = () => module._web_display(+object.visible, +object.selected, +display.grid, +display.edges, +display.orthographic);
  const closeMenus = () => document.querySelectorAll('nav details[open]').forEach(menu => {
    if (menu.contains(document.activeElement)) menu.querySelector('summary').focus();
    menu.open = false;
  });
  const syncUI = () => {
    object = readObject();
    fields.disabled = !object.selected;
    for (const input of fields.querySelectorAll('input')) input.value = Number(object.transform[Number(input.dataset.transform)].toFixed(4));
    $('#object-name').value = object.name;
    $('#object-name').disabled = !object.selected;
    $('#reset-transform').disabled = !object.selected;
    $('#outliner-name').textContent = object.name;
    $('#object-visible').checked = object.visible;
    $('#select-object').setAttribute('aria-pressed', String(object.selected));
    $('#object-row').classList.toggle('unselected', !object.selected);
    $('#selection-hint').hidden = object.selected;
    $('#object-label').textContent = object.selected ? `(1) Collection | ${object.name}` : 'No object selected';
    $('#dimensions').textContent = object.transform.slice(6).map(n => (Math.abs(n) * 2).toFixed(3)).join(' × ');
    $('#grid-toggle').setAttribute('aria-pressed', String(display.grid));
    $('#edges-toggle').setAttribute('aria-pressed', String(display.edges));
    $('#projection-toggle').setAttribute('aria-pressed', String(display.orthographic));
    $('#view-label').textContent = display.orthographic ? 'Orthographic' : 'User perspective';
    $('#undo').disabled = undo.length === 0;
    $('#redo').disabled = redo.length === 0;
    refreshDirty();
  };
  const applyObject = value => {
    object = { ...value, transform: [...value.transform] };
    object.transform.forEach((value, index) => module._web_set_transform(index, value));
    syncDisplay(); syncUI();
  };
  const edit = action => {
    const previous = readObject();
    action();
    if (JSON.stringify(previous) !== JSON.stringify(readObject())) {
      undo.push(previous); undo = undo.slice(-100); redo = [];
    }
    syncDisplay(); syncUI();
  };
  const setInspector = open => {
    inspectorOpen = open;
    workspace.classList.toggle('inspector-hidden', !open);
    inspectorToggle.setAttribute('aria-expanded', String(open));
    refreshDirty();
  };
  const setSizes = () => {
    document.documentElement.style.setProperty('--inspector-width', `${inspectorWidth}px`);
    document.documentElement.style.setProperty('--outliner-height', `${outlinerHeight}px`);
    $('#inspector-split').setAttribute('aria-valuenow', String(inspectorWidth));
    $('#outliner-split').setAttribute('aria-valuenow', String(outlinerHeight));
  };
  const action = (selector, callback) => $(selector).addEventListener('click', () => { callback(); closeMenus(); });
  fields.addEventListener('change', event => {
    const index = Number(event.target.dataset.transform), value = event.target.valueAsNumber;
    if (!Number.isInteger(index) || !event.target.checkValidity() || !Number.isFinite(value)) {
      actions.message('Enter a finite transform value within the displayed limits.'); syncUI(); return;
    }
    edit(() => module._web_set_transform(index, value));
  });
  $('#object-name').addEventListener('change', event => edit(() => { object.name = event.target.value.trim() || 'Cube'; }));
  $('#object-visible').addEventListener('change', event => edit(() => { object.visible = event.target.checked; }));
  action('#select-object', () => edit(() => { object.selected = !object.selected; }));
  action('#reset-transform', () => edit(() => defaults().transform.forEach((value, index) => module._web_set_transform(index, value))));
  action('#undo', () => {
    if (!undo.length) return;
    redo.push(readObject()); applyObject(undo.pop());
  });
  action('#redo', () => {
    if (!redo.length) return;
    undo.push(readObject()); applyObject(redo.pop());
  });
  const defaultDistance = () => Math.max(6, 4 * $('#canvas').clientHeight / Math.max(1, $('#canvas').clientWidth));
  const resetView = () => {
    display.orthographic = false;
    module._web_set_target(0, 0, 0); module._web_orbit(0.65, 0.5, defaultDistance());
    syncDisplay(); syncUI();
  };
  action('#reset-view', resetView);
  for (const [selector, factor] of [['#zoom-in', 1 / 1.2], ['#zoom-out', 1.2]]) action(selector, () => {
    module._web_orbit(module._web_yaw(), module._web_pitch(), module._web_distance() * factor); refreshDirty();
  });
  action('#frame-selected', () => {
    const state = readObject();
    module._web_set_target(...state.transform.slice(0, 3));
    const canvas = $('#canvas');
    const distance = Math.max(3, 4 * Math.max(...state.transform.slice(6).map(Math.abs)) * Math.max(1, canvas.clientHeight / Math.max(1, canvas.clientWidth)));
    module._web_orbit(module._web_yaw(), module._web_pitch(), distance); refreshDirty();
  });
  for (const button of document.querySelectorAll('[data-view]')) button.addEventListener('click', () => {
    const [yaw, pitch] = { front: [0, 0], right: [Math.PI / 2, 0], top: [0, Math.PI / 2] }[button.dataset.view];
    module._web_orbit(yaw, pitch, module._web_distance());
    display.orthographic = true; syncDisplay(); syncUI();
    $('#view-label').textContent = `${button.textContent} orthographic`;
  });
  for (const key of ['grid', 'edges']) action(`#${key}-toggle`, () => { display[key] = !display[key]; syncDisplay(); syncUI(); });
  action('#projection-toggle', () => { display.orthographic = !display.orthographic; syncDisplay(); syncUI(); });
  action('#new-preview', () => {
    edit(() => { applyObject(defaults()); });
    display = { grid: true, edges: true, orthographic: false };
    resetView(); syncDisplay(); syncUI(); actions.message('New preview. Object edits can be undone.');
  });
  action('#save', actions.save);
  action('#restore', actions.restore);
  action('#download', () => {
    const blob = new Blob([JSON.stringify(capture(), null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob), link = document.createElement('a');
    link.href = url; link.download = `${object.name.replace(/[^a-z0-9_-]/gi, '_').slice(0, 40)}.blender-web.json`;
    link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    actions.message('Downloaded preview .json. Native .blend saving follows the full editor build.');
  });
  action('#open', () => { $('#project-file').value = ''; $('#project-file').click(); });
  $('#project-file').addEventListener('change', async event => {
    const file = event.target.files[0];
    if (!file) return;
    try {
      if (file.size > 1024 * 1024) throw new Error('Preview files must be smaller than 1 MiB.');
      restore(JSON.parse(await file.text())); actions.message('Preview opened. Save to keep it on this device.');
    } catch (error) { actions.message(`Cannot open preview: ${error.message}`); }
  });
  inspectorToggle.addEventListener('click', () => setInspector(!inspectorOpen));
  const resizePanel = (selector, horizontal) => {
    const handle = $(selector);
    let drag;
    const update = delta => {
      if (horizontal) outlinerHeight = Math.round(Math.max(90, Math.min(360, $('#inspector').clientHeight - 150, drag.size + delta)));
      else inspectorWidth = Math.round(Math.max(220, Math.min(520, workspace.clientWidth - 246, drag.size - delta)));
      setSizes(); refreshDirty();
    };
    handle.addEventListener('pointerdown', event => {
      drag = { coordinate: horizontal ? event.clientY : event.clientX, size: horizontal ? outlinerHeight : inspectorWidth };
      handle.setPointerCapture(event.pointerId); handle.classList.add('dragging'); event.preventDefault();
    });
    handle.addEventListener('pointermove', event => { if (drag) update((horizontal ? event.clientY : event.clientX) - drag.coordinate); });
    const stop = () => { drag = null; handle.classList.remove('dragging'); };
    for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) handle.addEventListener(name, stop);
    handle.addEventListener('keydown', event => {
      if (!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key)) return;
      drag = { size: horizontal ? outlinerHeight : inspectorWidth };
      update(['ArrowRight','ArrowDown'].includes(event.key) ? 10 : -10); drag = null; event.preventDefault();
    });
  };
  resizePanel('#inspector-split', false); resizePanel('#outliner-split', true);
  document.addEventListener('click', event => { if (!event.target.closest('nav')) closeMenus(); });
  for (const summary of document.querySelectorAll('nav summary')) summary.addEventListener('click', () => {
    for (const menu of document.querySelectorAll('nav details')) if (menu !== summary.parentElement) menu.open = false;
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape') closeMenus();
    if (!(event.ctrlKey || event.metaKey)) return;
    if (event.key.toLowerCase() === 's') { event.preventDefault(); document.activeElement?.blur(); if (!$('#save').disabled) actions.save(); }
    if (event.key.toLowerCase() === 'z' && !event.target.closest('input')) {
      event.preventDefault(); $(event.shiftKey ? '#redo' : '#undo').click();
    }
  });
  for (const event of ['pointerup', 'wheel', 'keyup']) $('#canvas').addEventListener(event, () => requestAnimationFrame(refreshDirty));
  function restore(value) {
    if (value?.format !== 'blender-web-preview' || value.version !== 1) throw new Error('Expected a Blender Web preview .json file.');
    const o = value.object, c = value.camera, d = value.display, l = value.layout;
    if (!o || typeof o.name !== 'string' || !o.name.trim() || o.name.length > 64 || !finiteArray(o.transform, 9, 10000) || o.transform.slice(6).some(n => Math.abs(n) > 100) ||
        typeof o.visible !== 'boolean' || typeof o.selected !== 'boolean' || !c || ![c.yaw, c.pitch, c.distance].every(Number.isFinite) || Math.abs(c.yaw) > 10000 || Math.abs(c.pitch) > Math.PI / 2 || c.distance < 0.2 || c.distance > 500 || !finiteArray(c.target, 3, 10000) ||
        !d || !['grid','edges','orthographic'].every(key => typeof d[key] === 'boolean') || !l || typeof l.inspectorOpen !== 'boolean' || !Number.isFinite(l.inspectorWidth) || l.inspectorWidth < 220 || l.inspectorWidth > 520 || !Number.isFinite(l.outlinerHeight) || l.outlinerHeight < 90 || l.outlinerHeight > 360) {
      throw new Error('The preview contains invalid object, camera or layout values.');
    }
    edit(() => applyObject({ name: o.name.trim(), transform: [...o.transform], visible: o.visible, selected: o.selected }));
    display = { grid: d.grid, edges: d.edges, orthographic: d.orthographic };
    module._web_set_target(...c.target); module._web_orbit(c.yaw, c.pitch, c.distance);
    inspectorWidth = l.inspectorWidth; outlinerHeight = l.outlinerHeight;
    setSizes(); setInspector(l.inspectorOpen); syncDisplay(); syncUI();
  }
  for (const button of document.querySelectorAll('.editor-action')) button.disabled = false;
  $('#object-visible').disabled = false;
  setSizes(); setInspector(inspectorOpen); syncDisplay(); syncUI();
  module._web_orbit(module._web_yaw(), module._web_pitch(), defaultDistance());
  return { capture, restore, markSaved(value) { saved = JSON.stringify(value); refreshDirty(); },
    restoreLegacyView(value) {
      if (![value?.yaw, value?.pitch, value?.distance].every(Number.isFinite)) throw new Error('Invalid saved view.');
      module._web_orbit(value.yaw, value.pitch, value.distance); refreshDirty();
    } };
}
