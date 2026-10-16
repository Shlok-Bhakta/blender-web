/* SPDX-License-Identifier: GPL-2.0-or-later */
globalThis['blenderLoadShaderCache'] = async () => {
  try {
    const db = await new Promise((resolve, reject) => {
      const request = indexedDB.open('blender-web-wgsl', 1);
      request.onupgradeneeded = () => request.result.createObjectStore('shaders');
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
    globalThis.__WGSL_CACHE__ = await new Promise((resolve, reject) => {
      const values = new Map();
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
