/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
/* Scope's -1 sentinel is compared with size_t. On Wasm32 the signed 64-bit
 * index remains signed in that comparison, so normalize invalid indices to
 * the end of the scope array before the original validity checks run. */
constexpr int64_t GHOST_web_shader_scope_index(int64_t index, size_t count)
{
  return index < 0 || uint64_t(index) >= count ? int64_t(count) : index;
}
