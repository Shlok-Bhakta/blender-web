/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>
#include <cstdint>
#include <OpenImageIO/strutil.h>

using GHOST_WebOSLHash = uint64_t;

constexpr uint64_t GHOST_web_osl_string_hash(OIIO::string_view value)
{
  return OIIO::Strutil::strhash64(value);
}

// OSL's add/multiply nodes use pointer alignment on both wasm32 and native.
constexpr size_t GHOST_web_osl_closure_pointer_alignment()
{
  return alignof(void *);
}

// Cycles already keeps this record and its ImageHandle alive in its texture map.
// A real pointer preserves every type/ID bit on wasm32 without another owner.
template<typename Record> uintptr_t GHOST_web_osl_texture_handle(const Record *record)
{
  return reinterpret_cast<uintptr_t>(record);
}

template<typename Record> auto GHOST_web_osl_texture_handle_type(const void *handle)
{
  using Type = decltype(Record::type);
  return handle ? static_cast<const Record *>(handle)->type : Type{};
}

template<typename Record> int GHOST_web_osl_texture_handle_id(const void *handle)
{
  return handle ? static_cast<const Record *>(handle)->id : 0;
}
