/* SPDX-License-Identifier: GPL-2.0-or-later */
import { createRenderer } from './gpu.js';
import { createHost } from './host.js';

const loading = document.querySelector('#loading');
const status = document.querySelector('#status');
const canvas = document.querySelector('#canvas');
const buttons = [...document.querySelectorAll('button')];
let failed = false;
function fail(error) {
  failed = true;
  console.error(error);
  loading.hidden = false;
  loading.textContent = `Unable to start the platform test.\n${error.message || error}`;
  status.textContent = 'Stopped';
  buttons.forEach(button => button.disabled = true);
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
  if (!storageError) {
    const save = document.querySelector('#save');
    const restore = document.querySelector('#restore');
    save.disabled = restore.disabled = false;
    const storageAction = action => async () => {
      save.disabled = restore.disabled = true;
      try { await action(); }
      catch (error) { status.textContent = `View storage failed: ${error.message || error}`; }
      finally { if (!failed) save.disabled = restore.disabled = false; }
    };
    save.onclick = storageAction(async () => {
      const view = { yaw: module._web_yaw(), pitch: module._web_pitch(), distance: module._web_distance() };
      module.FS.writeFile('/user/view.json', JSON.stringify(view));
      // Explicitly flush before saying saved. No asynchronous unload saves.
      await sync(module, false);
      status.textContent = `WebGPU · ${mode} · View saved on this device`;
    });
    restore.onclick = storageAction(async () => {
      const view = JSON.parse(module.FS.readFile('/user/view.json', { encoding: 'utf8' }));
      if (![view.yaw, view.pitch, view.distance].every(Number.isFinite)) throw new Error('Invalid saved view');
      module._web_orbit(view.yaw, view.pitch, view.distance);
      status.textContent = `WebGPU · ${mode} · View restored`;
    });
  }
  host.invalidate();
}
start().catch(fail);
