/* SPDX-License-Identifier: GPL-2.0-or-later */
(async () => {
  const status = document.getElementById('status');
  const message = document.getElementById('message');
  const progress = document.getElementById('progress');
  const detail = document.getElementById('detail');
  await navigator.serviceWorker.register('sw.js');
  await navigator.serviceWorker.ready;
  if (!crossOriginIsolated) {
    if (!navigator.serviceWorker.controller) {
      await new Promise(resolve => navigator.serviceWorker.addEventListener('controllerchange', resolve, {once:true}));
    }
    if (!sessionStorage.getItem('blender-isolation-reload')) {
      sessionStorage.setItem('blender-isolation-reload', '1');
      location.reload();
      return;
    }
    throw new Error('Browser cross-origin isolation is unavailable');
  }
  sessionStorage.removeItem('blender-isolation-reload');
  if (!navigator.gpu) throw new Error('This browser does not provide WebGPU');
  const manifest = await (await fetch('manifest.json')).json();
  const assetBase = new URL(manifest.assetBase, location.href);
  const canSuspend = typeof WebAssembly.promising === 'function' && typeof WebAssembly.Suspending === 'function';
  const uiOnly = !canSuspend && manifest.uiFallback;
  if (!canSuspend && !uiOnly) throw new Error('This build requires current desktop Chrome');
  const executable = uiOnly ? 'blender-ui.js' : 'blender.js';
  const selected = new Map(manifest.files.filter(file => {
    const name = file.name.split('/').pop();
    return name === 'blender.data' || name === executable ||
      name === (uiOnly ? 'blender-ui.wasm' : 'blender.wasm') ||
      name === (uiOnly ? 'blender-ui.worker.js' : 'blender.worker.js');
  }).map(file => [new URL(file.name, location.href).href, {...file, loaded:0, cached:false}]));
  const total = [...selected.values()].reduce((sum, file) => sum + file.bytes, 0);
  const mib = bytes => (bytes / 1048576).toFixed(1);
  progress.value = 0;
  let cacheFailed = false;
  navigator.serviceWorker.addEventListener('message', event => {
    if (event.data?.type !== 'blender-download') return;
    const file = selected.get(event.data.url);
    if (!file || !status.isConnected) return;
    file.loaded = Math.max(file.loaded, Math.min(file.bytes, event.data.loaded));
    file.cached ||= event.data.cached;
    const loaded = [...selected.values()].reduce((sum, entry) => sum + entry.loaded, 0);
    const cached = [...selected.values()].reduce((sum, entry) => sum + (entry.cached ? entry.bytes : 0), 0);
    progress.value = loaded / total;
    message.textContent = loaded === total ? 'Starting Blender…' : 'Loading Blender…';
    detail.textContent = `${mib(loaded)} / ${mib(total)} MiB · ${mib(cached)} MiB from cache`;
    cacheFailed ||= event.data.cacheFailed;
    if (cacheFailed) detail.textContent += ' · Download could not be saved';
  });
  // Request eviction protection without blocking startup. The browser decides
  // whether to grant it; Cache Storage remains usable if it declines.
  navigator.storage?.persist?.().catch(() => {});
  document.documentElement.dataset.blenderVariant = uiOnly ? 'ui' : 'render';
  const canvas = document.getElementById('canvas');
  canvas.width = innerWidth;
  canvas.height = innerHeight;
  // GHOST owns the backing-buffer size. Notify its existing resize callback
  // when layout or display scale changes, including browser zoom and moving
  // between monitors. Coalesce layout notifications to one per animation frame.
  let resizeFrame = 0;
  const notifyResize = () => {
    if (resizeFrame) return;
    resizeFrame = requestAnimationFrame(() => {
      resizeFrame = 0;
      window.dispatchEvent(new Event('resize'));
    });
  };
  new ResizeObserver(notifyResize).observe(canvas);
  visualViewport?.addEventListener('resize', notifyResize);
  const watchScale = () => {
    matchMedia(`(resolution: ${devicePixelRatio}dppx)`).addEventListener('change', () => {
      notifyResize();
      watchScale();
    }, {once:true});
  };
  watchScale();
  canvas.addEventListener('contextmenu', event => event.preventDefault());
  window.Module = {
    canvas,
    mainScriptUrlOrBlob: new URL(executable, assetBase).href,
    arguments: ['--factory-startup', '--disable-autoexec', '--python-expr',
      'import bpy; bpy.context.preferences.view.show_splash = False'],
    print: text => console.log(text),
    printErr: text => {
      console.log(text);
      if (String(text).includes('WGPU_FIRST_CONTENT')) {
        status.remove();
        document.documentElement.dataset.blenderReady = 'true';
        canvas.focus();
      }
    },
    setStatus: text => { if (status.isConnected && progress.value === 1) message.textContent = text || 'Starting Blender…'; },
    onAbort: reason => { document.body.append(status); message.textContent = `Blender stopped: ${reason}`; progress.removeAttribute('value'); },
    locateFile: path => new URL(uiOnly && path === 'blender.wasm' ? 'blender-ui.wasm' : path, assetBase).href,
  };
  const script = document.createElement('script');
  script.src = new URL(executable, assetBase).href;
  script.onerror = () => { message.textContent = 'Blender download failed. Reload to retry.'; };
  document.body.append(script);
})().catch(error => {
  document.getElementById('status').textContent = error.message;
  console.error(error);
});
