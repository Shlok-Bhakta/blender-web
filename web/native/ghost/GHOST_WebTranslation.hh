/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <type_traits>

/* StringRef uses signed 64-bit offsets and -1 for a missing delimiter. Keep
 * that type through `auto`: wasm32 size_t turns -1 into positive UINT32_MAX. */
template<typename Text, typename Needle>
constexpr int64_t GHOST_web_translation_find(const Text &text, const Needle &needle)
{
  static_assert(std::is_signed_v<decltype(text.find(needle))>);
  return text.find(needle);
}

template<typename Text, typename Separators>
constexpr int64_t GHOST_web_translation_separator(const Text &text, const Separators &separators)
{
  static_assert(std::is_signed_v<decltype(text.find_first_of(separators))>);
  return text.find_first_of(separators);
}
