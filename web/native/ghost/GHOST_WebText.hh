/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <cstring>

/* KeyboardEvent.key is either a Unicode scalar or a name such as "Dead".
 * Composition strings require a separate input path; never truncate them. */
inline bool GHOST_web_key_utf8(const char *key, char (&output)[6])
{
  output[0] = '\0';
  const size_t length = std::strlen(key);
  if (length == 0 || length > 4) return false;
  const unsigned char first = static_cast<unsigned char>(key[0]);
  uint32_t scalar;
  unsigned count;
  uint32_t minimum;
  if (first < 0x80) { scalar = first; count = 1; minimum = 0; }
  else if (first >= 0xc2 && first <= 0xdf) { scalar = first & 0x1f; count = 2; minimum = 0x80; }
  else if (first >= 0xe0 && first <= 0xef) { scalar = first & 0x0f; count = 3; minimum = 0x800; }
  else if (first >= 0xf0 && first <= 0xf4) { scalar = first & 0x07; count = 4; minimum = 0x10000; }
  else return false;
  if (length != count) return false;
  for (unsigned i = 1; i < count; i++) {
    const unsigned char next = static_cast<unsigned char>(key[i]);
    if ((next & 0xc0) != 0x80) return false;
    scalar = (scalar << 6) | (next & 0x3f);
  }
  if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ||
      scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f)) return false;
  std::memcpy(output, key, length + 1);
  return true;
}
