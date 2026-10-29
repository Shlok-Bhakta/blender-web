/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>

/* OSL's public string values contain an interned pointer. Its generated code
 * stores a full 64-bit hash instead. Those widths differ on wasm32. This
 * conversion applies only to storage used by generated code. */
constexpr size_t GHOST_web_osl_hash_storage_size(size_t bytes, bool is_string)
{
  return is_string ? bytes / sizeof(void *) * sizeof(uint64_t) : bytes;
}

#if defined(WITH_GHOST_WEB_OSL_PROBE)
/* This installs the finite SDK-loader experiment, not the production adapter. */
void GHOST_web_osl_install_probe_engine();
#endif
