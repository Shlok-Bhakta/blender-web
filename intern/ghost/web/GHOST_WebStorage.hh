/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <emscripten.h>
/* navigator.storage.estimate() is asynchronous. Bootstrap refreshes this
 * estimate before Blender starts; -1 means the browser did not report quota. */
EM_JS(double, GHOST_web_storage_free_space, (), {
  return globalThis['blenderStorageAvailable'] ?? -1;
});
