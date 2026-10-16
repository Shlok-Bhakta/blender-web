/* SPDX-License-Identifier: GPL-2.0-or-later */
const VERSION = '__VERSION__';
const PREFIX = `blender-native:${self.registration.scope}:`;
const CACHE = PREFIX + VERSION;
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
    const cache = await caches.open(PREFIX + (bundle ? bundle[1] : VERSION));
    const cacheable = !!bundle || SHELL.has(relative) || navigation;
    const key = navigation ? new URL('index.html', self.registration.scope).href : event.request;
    let response = cacheable && await cache.match(key);
    if (!response) {
      response = await fetch(event.request);
      if (cacheable && response.ok) {
        try { await cache.put(key, response.clone()); } catch (error) { console.warn(error); }
      }
    }
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, {status:response.status,statusText:response.statusText,headers});
  })());
});
