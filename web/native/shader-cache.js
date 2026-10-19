/* SPDX-License-Identifier: GPL-2.0-or-later */
globalThis['blenderLoadShaderCache'] = async () => {
  // These are original shaders translated by this exact compiler recipe.
  // They avoid repeating translation for common UI/default-render variants on
  // a first visit. Unseen variants still use the runtime shader compiler.
  globalThis.__WGSL_CACHE__ = new Map();
  try {
    const seed = JSON.parse(FS.readFile('/5.2/datafiles/shader-seed.json', {encoding:'utf8'}));
    for (const [key, value] of seed.entries) globalThis.__WGSL_CACHE__.set(key, value);
    console.log('WEB_WGSL_SEED entries=' + seed.entries.length);
  } catch (error) { console.warn('Bundled shader cache unavailable', error); }
  try {
    const db = await new Promise((resolve, reject) => {
      const request = indexedDB.open('blender-web-wgsl', 1);
      request.onupgradeneeded = () => request.result.createObjectStore('shaders');
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
    globalThis.__WGSL_CACHE__ = await new Promise((resolve, reject) => {
      const values = globalThis.__WGSL_CACHE__;
      const tx = db.transaction('shaders');
      tx.objectStore('shaders').openCursor().onsuccess = event => {
        const cursor = event.target.result;
        if (cursor) { values.set(cursor.key, cursor.value); cursor.continue(); }
      };
      tx.oncomplete = () => resolve(values);
      tx.onerror = () => reject(tx.error);
    });
    globalThis.__WGSL_CACHE_PUT__ = (key, value) => {
      try { db.transaction('shaders','readwrite').objectStore('shaders').put(value,key); }
      catch (error) { console.warn('Shader cache write failed', error); }
    };
  } catch (error) { console.warn('Shader cache unavailable', error); }
};
