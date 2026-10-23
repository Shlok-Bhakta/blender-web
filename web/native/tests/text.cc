/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>
#include <harfbuzz/hb-ot.h>
#include <fribidi/fribidi.h>
#include "../ghost/GHOST_WebText.hh"
#include "../ghost/GHOST_WebTranslation.hh"
#include "BLI_string_ref.hh"

static_assert(GHOST_web_translation_find(blender::StringRef("File", 4), char(4)) ==
              blender::StringRef::not_found);
static_assert(GHOST_web_translation_find(blender::StringRef("Operator\4Open", 13), char(4)) == 8);

unsigned shape(FT_Library library, const char *file, const char *text,
               unsigned expected, bool rtl)
{
  FT_Face ft_face;
  assert(FT_New_Face(library, file, 0, &ft_face) == 0);
  hb_face_t *face = hb_ft_face_create_referenced(ft_face);
  hb_font_t *font = hb_font_create(face);
  hb_ot_font_set_funcs(font);
  hb_font_set_scale(font, 1000, 1000);
  hb_buffer_t *buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, text, -1, 0, -1);
  hb_buffer_guess_segment_properties(buffer);
  assert(HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(buffer)) == rtl);
  hb_shape(font, buffer, nullptr, 0);
  unsigned count;
  const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
  const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
  assert(count == expected);
  int advance = 0;
  for (unsigned i = 0; i < count; i++) {
    assert(info[i].codepoint != 0);
    advance += positions[i].x_advance;
  }
  assert(advance > 0);
  hb_buffer_destroy(buffer);
  hb_font_destroy(font);
  hb_face_destroy(face);
  FT_Done_Face(ft_face);
  return count;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  char path[2048];
  FT_Library library;
  assert(FT_Init_FreeType(&library) == 0);
  std::snprintf(path, sizeof(path), "%s/NotoSansArabic-VariableFont_wdth,wght.woff2", argv[1]);
  const unsigned arabic = shape(library, path, "سلام", 3, true);
  std::snprintf(path, sizeof(path), "%s/NotoSansDevanagari-Regular.woff2", argv[1]);
  const unsigned devanagari = shape(library, path, "कि", 2, false);
  FT_Done_FreeType(library);

  const FriBidiChar logical[] = {'a', 'b', 'c', ' ', 0x5d0, 0x5d1, 0x5d2};
  const FriBidiChar expected[] = {'a', 'b', 'c', ' ', 0x5d2, 0x5d1, 0x5d0};
  FriBidiChar visual[7];
  FriBidiStrIndex l2v[7], v2l[7];
  FriBidiLevel levels[7];
  FriBidiParType direction = FRIBIDI_PAR_LTR;
  assert(fribidi_log2vis(logical, 7, &direction, visual, l2v, v2l, levels) > 0);
  for (int i = 0; i < 7; i++) {
    assert(visual[i] == expected[i]);
    assert(l2v[i] >= 0 && l2v[i] < 7 && v2l[l2v[i]] == i);
  }
  for (const char *key : {"a", "é", "日", "😀", " "}) {
    char output[6]{};
    assert(GHOST_web_key_utf8(key, output) && std::strcmp(key, output) == 0);
  }
  for (const char *key : {"", "Dead", "Enter", "ab", "é", "\x1f", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
    char output[6]{};
    assert(!GHOST_web_key_utf8(key, output) && output[0] == '\0');
  }
  std::printf("{\"harfbuzz\":\"%s\",\"arabicGlyphs\":%u,\"devanagariGlyphs\":%u,"
              "\"bidiMixedDirection\":true,\"unicodeKeys\":true,\"signedCatalogIndices\":true}\n",
              hb_version_string(), arabic, devanagari);
}
