/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_WindowWeb.hh"

#include <emscripten.h>

EM_JS(void, web_window_title, (const char *title), {
  Module.webHost.setTitle(UTF8ToString(title));
});
EM_JS(void, web_window_destroy, (), { Module.webHost.detach(); });
EM_JS(void, web_window_invalidate, (), { Module.webHost.invalidate(); });
EM_JS(void, web_window_cursor, (int visible), {
  Module.canvas.style.cursor = visible ? 'default' : 'none';
});

GHOST_WindowWeb::GHOST_WindowWeb(const char *title, uint32_t width, uint32_t height)
    : GHOST_WindowNULL(title, 0, 0, width, height, GHOST_kWindowStateNormal, nullptr,
                       GHOST_kDrawingContextTypeNone, GHOST_CONTEXT_PARAMS_NONE),
      title_(title), width_(width), height_(height)
{
  setTitle(title);
}

GHOST_WindowWeb::~GHOST_WindowWeb() { web_window_destroy(); }
void GHOST_WindowWeb::setTitle(const char *title)
{
  title_ = title;
  web_window_title(title);
}
std::string GHOST_WindowWeb::getTitle() const { return title_; }
void GHOST_WindowWeb::getClientBounds(GHOST_Rect &bounds) const
{
  bounds.set(0, 0, int32_t(width_), int32_t(height_));
}
void GHOST_WindowWeb::getWindowBounds(GHOST_Rect &bounds) const { getClientBounds(bounds); }
GHOST_TSuccess GHOST_WindowWeb::setClientSize(uint32_t, uint32_t)
{
  /* CSS owns the canvas size. Never claim an OS resize succeeded. */
  return GHOST_kFailure;
}
GHOST_TSuccess GHOST_WindowWeb::setClientWidth(uint32_t) { return GHOST_kFailure; }
GHOST_TSuccess GHOST_WindowWeb::setClientHeight(uint32_t) { return GHOST_kFailure; }
GHOST_TSuccess GHOST_WindowWeb::invalidate()
{
  web_window_invalidate();
  return GHOST_kSuccess;
}
GHOST_TSuccess GHOST_WindowWeb::activateDrawingContext() { return GHOST_kSuccess; }
GHOST_TSuccess GHOST_WindowWeb::swapBufferRelease() { return GHOST_kSuccess; }
GHOST_TSuccess GHOST_WindowWeb::setState(GHOST_TWindowState state)
{
  return state == GHOST_kWindowStateNormal ? GHOST_kSuccess : GHOST_kFailure;
}
GHOST_TSuccess GHOST_WindowWeb::setOrder(GHOST_TWindowOrder) { return GHOST_kFailure; }
GHOST_TSuccess GHOST_WindowWeb::hasCursorShape(GHOST_TStandardCursor shape)
{
  return shape == GHOST_kStandardCursorDefault ? GHOST_kSuccess : GHOST_kFailure;
}
GHOST_TSuccess GHOST_WindowWeb::setWindowCursorShape(GHOST_TStandardCursor shape)
{
  return hasCursorShape(shape);
}
GHOST_TSuccess GHOST_WindowWeb::setWindowCursorVisibility(bool visible)
{
  web_window_cursor(visible);
  return GHOST_kSuccess;
}
GHOST_TSuccess GHOST_WindowWeb::setWindowCursorGrab(GHOST_TGrabCursorMode mode)
{
  return mode == GHOST_kGrabDisable ? GHOST_kSuccess : GHOST_kFailure;
}
GHOST_TSuccess GHOST_WindowWeb::setWindowCustomCursorShape(
    const uint8_t *, const uint8_t *, const int[2], const int[2], bool)
{
  return GHOST_kFailure;
}
void GHOST_WindowWeb::resizeFromBrowser(uint32_t width, uint32_t height, float scale)
{
  width_ = width;
  height_ = height;
  native_pixel_size_ = scale;
}
