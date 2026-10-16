/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "BKE_idtype.hh"
#include "BLI_ghash.h"
#include "GHOST_WebImageCache.hh"
#include <cstdio>
using namespace blender;
int main()
{
  IDCacheKey slot = {42, 1234};
  IDCacheKey runtime = {42, GHOST_web_image_runtime_base() + 1234};
  int slot_value = 1, runtime_value = 2;
  if (runtime.identifier == slot.identifier || sizeof(runtime.identifier) != 8) return 1;
  auto *cache = BLI_ghash_new(BKE_idtype_cache_key_hash, BKE_idtype_cache_key_cmp, "Wasm image cache keys");
  BLI_ghash_insert(cache, &slot, &slot_value);
  BLI_ghash_insert(cache, &runtime, &runtime_value);
  const bool distinct = BLI_ghash_len(cache) == 2 &&
      BLI_ghash_lookup(cache, &slot) == &slot_value &&
      BLI_ghash_lookup(cache, &runtime) == &runtime_value;
  BLI_ghash_free(cache, nullptr, nullptr);
  if (!distinct) return 2;
  std::puts("{\"imageCacheNamespaces\":true}");
}
