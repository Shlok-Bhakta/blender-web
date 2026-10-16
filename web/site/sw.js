/* SPDX-License-Identifier: GPL-2.0-or-later */
const VERSION = '__BUILD_VERSION__';
const PREFIX = `blender-web:${self.registration.scope}:`;
const CACHE = PREFIX + VERSION;
const SHELL = ['index.html', 'style.css', 'app.js', 'gpu.js', 'host.js', 'editor.js', 'manifest.json'];

self.addEventListener('install', event => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await cache.addAll(SHELL.map(path => new URL(path, self.registration.scope).href));
    // Existing tabs keep their matching shell and asset cache until they close.
    // First installation activates automatically without skipWaiting().
  })());
});
self.addEventListener('activate', event => {
  event.waitUntil((async () => {
    // Cache names include the project scope, so other Pages apps stay untouched.
    await Promise.all((await caches.keys()).filter(name => name.startsWith(PREFIX) && name !== CACHE).map(name => caches.delete(name)));
    await self.clients.claim();
  })());
});
function isolated(response) {
  const headers = new Headers(response.headers);
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  headers.set('Cross-Origin-Resource-Policy', 'same-origin');
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
}
self.addEventListener('fetch', event => {
  const request = event.request;
  const url = new URL(request.url);
  if (request.method !== 'GET' || url.origin !== self.location.origin || !url.href.startsWith(self.registration.scope)) return;
  const relative = url.pathname.slice(new URL(self.registration.scope).pathname.length);
  const navigation = request.mode === 'navigate';
  // Do not cache arbitrary user imports or application data.
  const cacheable = navigation || SHELL.includes(relative) || relative.startsWith(`assets/${VERSION}/`);
  event.respondWith((async () => {
    const cache = await caches.open(CACHE);
    const key = navigation ? new URL('index.html', self.registration.scope).href : request;
    let response = cacheable ? await cache.match(key) : null;
    if (!response) {
      response = await fetch(request);
      if (cacheable && response.ok) {
        // Quota exhaustion must not prevent the application from starting.
        try { await cache.put(key, response.clone()); } catch (error) { console.warn('Cache write failed', error); }
      }
    }
    return response.type === 'opaque' || response.status === 0 ? response : isolated(response);
  })());
});
