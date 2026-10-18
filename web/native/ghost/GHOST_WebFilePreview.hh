/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Automatic file previews enter synchronous GPU paths outside a promising
 * browser GPU job. Skip that optional capture while saving, then restore the
 * preference. The native blend writer and the scene data remain unchanged. */
struct GHOST_WebFilePreview {
  blender::eUserpref_File_Preview_Type &type;
  blender::eUserpref_File_Preview_Type saved;
  explicit GHOST_WebFilePreview(blender::eUserpref_File_Preview_Type &value)
      : type(value), saved(value)
  {
    type = blender::USER_FILE_PREVIEW_NONE;
  }
  ~GHOST_WebFilePreview() { type = saved; }
};
