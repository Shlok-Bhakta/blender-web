/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
/* Image runtime keys occupy the namespace above the 32-bit render-slot hashes.
 * Keep that namespace on Wasm32 rather than truncating it to a pointer-sized key. */
using GHOST_WebCacheIdentifier = uint64_t;
constexpr GHOST_WebCacheIdentifier GHOST_web_image_runtime_base()
{
  return uint64_t(1) << 32u;
}
