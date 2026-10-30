/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>

using GHOST_WebOSLHash = uint64_t;

// OSL's add/multiply nodes use pointer alignment on both wasm32 and native.
constexpr size_t GHOST_web_osl_closure_pointer_alignment()
{
  return alignof(void *);
}
