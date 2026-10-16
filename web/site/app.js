/* SPDX-License-Identifier: GPL-2.0-or-later */
import { createRenderer } from './gpu.js';
import { createHost } from './host.js';
import { createEditor } from './editor.js';

const loading = document.querySelector('#loading');
const status = document.querySelector('#status');
const canvas = document.querySelector('#canvas');
const buttons = [...document.querySelectorAll('button')];
let failed = false;
function fail(error) {
  failed = true;
  console.error(error);
  loading.hidden = false;
  loading.textContent = `Unable to start the preview.\n${error.message || error}`;
  status.textContent = 'Stopped';
  buttons.forEach(button => button.disabled = true);
  document.querySelectorAll('fieldset, input').forEach(control => control.disabled = true);
}

async function deadline(promise, milliseconds = 5000) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Service worker setup timed out')), milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

async function serviceWorker() {
  if (!('serviceWorker' in navigator) || !isSecureContext) return;
  const registration = await deadline(navigator.serviceWorker.register('./sw.js', { updateViaCache: 'none' }));
  await deadline(navigator.serviceWorker.ready);
  if (!navigator.serviceWorker.controller) {
    let onController;
    try {
      await deadline(new Promise(resolve => {
        onController = resolve;
        navigator.serviceWorker.addEventListener('controllerchange', onController, { once: true });
        if (navigator.serviceWorker.controller) resolve();
      }));
    } finally { navigator.serviceWorker.removeEventListener('controllerchange', onController); }
  }
  // Pages cannot configure COOP/COEP. Our same-origin worker supplies them.
  // A URL marker bounds the retry even if browser storage is unavailable.
  const url = new URL(location.href);
  if (!crossOriginIsolated && !url.searchParams.has('coi')) {
    url.searchParams.set('coi', '1');
    location.replace(url);
    return true;
  }
  registration.update().catch(console.warn);
  return false;
}

function loadScript(url) {
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = url;
    script.onload = resolve;
    script.onerror = () => reject(new Error(`Unable to download ${url}`));
    document.head.append(script);
  });
}
function sync(module, populate) {
  return new Promise((resolve, reject) => module.FS.syncfs(populate, error => error ? reject(error) : resolve()));
}

async function start() {
  try { if (await serviceWorker()) return; }
  catch (error) { console.warn('Asset caching unavailable:', error); }
  const manifestResponse = await fetch('./manifest.json');
  if (!manifestResponse.ok) throw new Error('Build manifest is missing. Run web/tools/build.py.');
  const manifest = await manifestResponse.json();
  const requested = new URL(location.href).searchParams.get('threads');
  const canThread = crossOriginIsolated && typeof SharedArrayBuffer !== 'undefined';
  const hardware = Math.max(1, navigator.hardwareConcurrency || 2);
  const count = requested === null ? Math.min(32, hardware) : Number(requested);
  if (!Number.isInteger(count) || count < 0 || count > 32) throw new Error('threads must be an integer from 0 to 32.');
  const workerCount = canThread && count !== 0 ? Math.max(1, Math.min(32, hardware, count)) : 0;
  const variant = workerCount ? 'threaded' : 'single';
  const base = `./assets/${manifest.version}/${variant}/`;
  const renderer = await createRenderer(canvas, fail);
  const mode = workerCount ? `${workerCount} CPU workers` : 'single CPU thread';
  const host = createHost(canvas, renderer, job => {
    status.textContent = `WebGPU · ${mode} · CPU check ${job.checksum}`;
    document.querySelector('#cpu').disabled = false;
  }, fail);
  loading.textContent = `Loading Wasm · ${mode}`;
  await loadScript(`${base}ghost_web.js`);
  let storageError;
  const module = await window.createGhostWeb({
    canvas, webHost: host, webWorkerCount: workerCount,
    locateFile: name => new URL(base + name, location.href).href,
    onAbort: reason => fail(new Error(String(reason))),
    preRun: [instance => {
      instance.FS.mkdir('/user');
      instance.FS.mount(instance.IDBFS, {}, '/user');
      instance.addRunDependency('persistent-files');
      instance.FS.syncfs(true, error => {
        storageError = error;
        instance.removeRunDependency('persistent-files');
      });
    }],
  });
  // Exposed for diagnostics and browser acceptance checks.
  window.blenderWeb = { module, host, manifest, variant, workerCount, storageAvailable: !storageError };
  if (failed) return;
  loading.hidden = true;
  status.textContent = `WebGPU · ${mode}${storageError ? ' · persistence unavailable' : ''}`;
  document.querySelector('#cpu').disabled = false;
  document.querySelector('#cpu').onclick = () => {
    host.job = null;
    document.querySelector('#cpu').disabled = true;
    module._web_run_parallel_test();
  };
  let editor, storageBusy = false;
  const message = text => { status.textContent = text; };
  const updateStorageButtons = () => {
    document.querySelector('#save').disabled = failed || !!storageError || storageBusy;
    document.querySelector('#restore').disabled = failed || !!storageError || storageBusy ||
      !['/user/preview.json', '/user/view.json'].some(path => module.FS.analyzePath(path).exists);
  };
  const storageAction = action => async () => {
    if (failed || storageError || storageBusy) return;
    storageBusy = true; updateStorageButtons();
    try { await action(); }
    catch (error) { message(`Preview storage failed: ${error.message || error}`); }
    finally { storageBusy = false; updateStorageButtons(); }
  };
  editor = createEditor(module, host, { message,
    save: storageAction(async () => {
      const state = editor.capture();
      module.FS.writeFile('/user/preview.json', JSON.stringify(state));
      // Confirm only after IndexedDB has flushed. Edits during the flush remain dirty.
      await sync(module, false);
      editor.markSaved(state); message('Preview saved on this device');
    }),
    restore: storageAction(async () => {
      if (module.FS.analyzePath('/user/preview.json').exists) {
        editor.restore(JSON.parse(module.FS.readFile('/user/preview.json', { encoding: 'utf8' })));
        editor.markSaved(editor.capture()); message('Preview restored');
      } else {
        editor.restoreLegacyView(JSON.parse(module.FS.readFile('/user/view.json', { encoding: 'utf8' })));
        message('Saved view restored');
      }
    }),
  });
  window.blenderWeb.editor = editor;
  updateStorageButtons();
  host.invalidate();
}
start().catch(fail);
