/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "BLI_string_ref.hh"
/* StringRef returns a signed 64-bit sentinel, unlike Wasm32 std::string::npos. */
inline void GHOST_web_validate_shader_source([[maybe_unused]] blender::StringRef source)
{
  BLI_assert_msg(source.find("//") == blender::StringRef::not_found &&
                     source.find("/*") == blender::StringRef::not_found,
                 "Input source should have no comments.");
}

inline int64_t GHOST_web_find_printf_argument(std::string_view format, int64_t start)
{
  const size_t index = format.find('%', size_t(start));
  return index == std::string_view::npos ? int64_t(-1) : int64_t(index);
}
