/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>

/* ResourceScope stores one chunk-list pointer and one allocator reference.
 * Preserve its compact-layout assertion for both Wasm32 and Wasm64. */
constexpr std::size_t GHOST_web_resource_scope_size()
{
  return 2 * sizeof(void *);
}
