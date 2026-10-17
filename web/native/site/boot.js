/* SPDX-License-Identifier: GPL-2.0-or-later */
(async () => {
  const status = document.getElementById('status');
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
  document.documentElement.dataset.blenderVariant = uiOnly ? 'ui' : 'render';
  const canvas = document.getElementById('canvas');
  canvas.width = innerWidth;
  canvas.height = innerHeight;
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
    setStatus: text => { if (status.isConnected) status.textContent = text || 'Starting Blender…'; },
    onAbort: reason => { status.textContent = `Blender stopped: ${reason}`; },
    locateFile: path => new URL(uiOnly && path === 'blender.wasm' ? 'blender-ui.wasm' : path, assetBase).href,
  };
  const script = document.createElement('script');
  script.src = new URL(executable, assetBase).href;
  script.onerror = () => { status.textContent = 'Blender download failed. Reload to retry.'; };
  document.body.append(script);
})().catch(error => {
  document.getElementById('status').textContent = error.message;
  console.error(error);
});
