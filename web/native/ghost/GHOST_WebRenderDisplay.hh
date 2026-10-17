/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
/* Render into Blender's full-screen Image Editor on a one-canvas platform.
 * Restore the user's display preference on return; scene settings stay intact. */
struct GHOST_WebRenderDisplay {
  blender::eUserpref_RenderDisplayType &type;
  blender::eUserpref_RenderDisplayType saved;
  explicit GHOST_WebRenderDisplay(blender::eUserpref_RenderDisplayType &value) : type(value), saved(value)
  {
    if (type == blender::USER_RENDER_DISPLAY_WINDOW) type = blender::USER_RENDER_DISPLAY_SCREEN;
  }
  ~GHOST_WebRenderDisplay() { type = saved; }
};
