/* SPDX-License-Identifier: GPL-2.0-or-later */
const VERSION = '__VERSION__';
const PREFIX = `blender-native:${self.registration.scope}:`;
const CACHE = PREFIX + VERSION;
const ASSETS = __ASSETS__;
const CONTENT_CACHE = PREFIX + 'content-v1';
const SHELL = new Set(['index.html', 'boot.js', 'manifest.json']);
self.addEventListener('install', event => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await cache.addAll([...SHELL].map(path => new URL(path, self.registration.scope).href));
    await self.skipWaiting();
  })());
});
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (url.origin !== location.origin || event.request.method !== 'GET' || !url.href.startsWith(self.registration.scope)) return;
  const relative = url.pathname.slice(new URL(self.registration.scope).pathname.length);
  const bundle = /^assets\/([a-f0-9]{20})\//.exec(relative);
  const navigation = event.request.mode === 'navigate';
  event.respondWith((async () => {
    // Versioned bundles keep already-open applications and their workers on
    // matching JS/Wasm/data, even when a new deployment takes control.
    if (bundle && bundle[1] !== VERSION && !ASSETS[relative]) {
      // A newly activated worker can still receive requests from an older tab.
      const previous = await caches.open(PREFIX + bundle[1]);
      const manifest = await previous.match(new URL('manifest.json', self.registration.scope).href);
      if (manifest) for (const file of (await manifest.json()).files) ASSETS[file.name] = file;
    }
    const asset = ASSETS[relative];
    let cache;
    try { cache = await caches.open(asset ? CONTENT_CACHE : PREFIX + (bundle ? bundle[1] : VERSION)); }
    catch (error) { console.warn('Blender cache unavailable', error); }
    // Stable per-file hashes reuse unchanged data and Wasm across deployments,
    // while versioned request URLs still pin each running tab to its build.
    const contentKey = asset && new URL(`__content__/${asset.sha256}`, self.registration.scope).href;
    let lastProgress = 0;
    const report = async (loaded, cached = false, cacheFailed = false) => {
      if (!asset) return;
      const now = performance.now();
      if (!cacheFailed && loaded !== asset.bytes && now - lastProgress < 80) return;
      lastProgress = now;
      const clients = await self.clients.matchAll({type:'window'});
      for (const client of clients) {
        if (client.url.startsWith(self.registration.scope)) client.postMessage({
          type:'blender-download', url:url.href, loaded, cached, cacheFailed,
        });
      }
    };
    const cacheable = !!bundle || SHELL.has(relative) || navigation;
    const key = contentKey || (navigation ? new URL('index.html', self.registration.scope).href : event.request);
    let response = cacheable && cache && await cache.match(key).catch(() => undefined);
    if (response) await report(asset?.bytes, true);
    if (!response) {
      response = await fetch(event.request);
      if (asset && response.ok && response.body) {
        let loaded = 0;
        const body = response.body.pipeThrough(new TransformStream({
          transform(chunk, controller) {
            loaded += chunk.byteLength;
            controller.enqueue(chunk);
            void report(loaded);
          },
        }));
        response = new Response(body, {status:response.status,statusText:response.statusText,headers:response.headers});
      }
      if (cacheable && response.ok && cache) {
        // Cache one branch while the browser consumes the other. In particular,
        // Wasm compilation can begin before its entire download has finished.
        event.waitUntil(cache.put(key, response.clone()).then(
          () => report(asset?.bytes),
          async error => {
            console.warn('Blender download could not be cached', error);
            await report(0, false, true);
          }));
      }
    }
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, {status:response.status,statusText:response.statusText,headers});
  })());
});
