/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Browser (Emscripten) window: wraps the page's #canvas element. There is
 * exactly one window; its client size mirrors the canvas. The drawing context
 * is GHOST_kDrawingContextTypeNone (the WebGPU GPU backend acquires the device
 * independently); swapBuffers presents the GPU backend's backbuffer into the
 * canvas' WebGPU surface via `blender_webgpu_present`.
 */

#pragma once

#include "GHOST_Window.hh"

#include <emscripten/emscripten.h>

/* Implemented by the WebGPU GPU backend (source/blender/gpu/webgpu). Resolved
 * at the final static link of the Blender executable. */
extern "C" void blender_webgpu_present(int width, int height);
extern "C" void blender_webgpu_backbuffer_size(int width, int height);

class GHOST_WindowWeb : public GHOST_Window {
 private:
  uint32_t width_, height_;

 public:
  GHOST_WindowWeb(const char * /*title*/,
                  uint32_t width,
                  uint32_t height,
                  GHOST_TWindowState state,
                  const GHOST_ContextParams &context_params)
      : GHOST_Window(width, height, state, context_params, false), width_(width), height_(height)
  {
  }
  ~GHOST_WindowWeb() override = default;

  void resize(uint32_t width, uint32_t height)
  {
    width_ = width;
    height_ = height;
  }

  GHOST_TSuccess swapBufferAcquire() override
  {
    return GHOST_kSuccess;
  }

  GHOST_TSuccess hasCursorShape(GHOST_TStandardCursor /*cursor_shape*/) override
  {
    return GHOST_kSuccess;
  }

 protected:
  GHOST_TSuccess installDrawingContext(GHOST_TDrawingContextType /*type*/)
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess removeDrawingContext()
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setWindowCursorGrab(GHOST_TGrabCursorMode /*mode*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor /*shape*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setWindowCustomCursorShape(const uint8_t * /*bitmap*/,
                                            const uint8_t * /*mask*/,
                                            const int /*size*/[2],
                                            const int /*hot_spot*/[2],
                                            bool /*can_invert_color*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setWindowCursorVisibility(bool /*visible*/) override
  {
    return GHOST_kSuccess;
  }

  bool getValid() const override
  {
    return true;
  }
  void setTitle(const char *title) override
  {
    /* Runs on the render pthread under PROXY_TO_PTHREAD, where `document` does
     * not exist — set the title on the browser main thread. Must be SYNCHRONOUS:
     * `title` points into a caller-owned std::string that is destroyed as soon
     * as this returns, so an async dispatch would UTF8ToString() a freed/reused
     * buffer (garbage prefix). Synchronous proxying reads it while still valid.
     * setTitle is only called on title refreshes (open/save/dirty), not per
     * frame, so blocking the render thread briefly here is harmless. */
    MAIN_THREAD_EM_ASM({ document.title = UTF8ToString($0); }, title);
  }
  std::string getTitle() const override
  {
    return "Blender";
  }
  void setPath(const char * /*filepath*/) override {}

  void getWindowBounds(GHOST_Rect &bounds) const override
  {
    getClientBounds(bounds);
  }
  void getClientBounds(GHOST_Rect &bounds) const override
  {
    bounds.l_ = 0;
    bounds.t_ = 0;
    bounds.r_ = int32_t(width_);
    bounds.b_ = int32_t(height_);
  }
  GHOST_TSuccess setClientWidth(uint32_t /*width*/) override
  {
    return GHOST_kFailure;
  }
  GHOST_TSuccess setClientHeight(uint32_t /*height*/) override
  {
    return GHOST_kFailure;
  }
  GHOST_TSuccess setClientSize(uint32_t /*width*/, uint32_t /*height*/) override
  {
    return GHOST_kFailure;
  }
  void screenToClient(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override
  {
    outX = inX;
    outY = inY;
  }
  void clientToScreen(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override
  {
    outX = inX;
    outY = inY;
  }
  GHOST_TSuccess swapBufferRelease() override
  {
    /* End of window frame: blit the backbuffer into the canvas surface. */
    blender_webgpu_present(int(width_), int(height_));
    return GHOST_kSuccess;
  }
  GHOST_Context *newDrawingContext(GHOST_TDrawingContextType /*type*/) override
  {
    return nullptr;
  }
  GHOST_TSuccess activateDrawingContext() override
  {
    /* Ensure the GPU backend's window backbuffer matches the canvas BEFORE the
     * window manager draws into it. */
    blender_webgpu_backbuffer_size(int(width_), int(height_));
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setState(GHOST_TWindowState /*state*/) override
  {
    return GHOST_kSuccess;
  }
  GHOST_TWindowState getState() const override
  {
    return GHOST_kWindowStateMaximized;
  }
  GHOST_TSuccess invalidate() override
  {
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setOrder(GHOST_TWindowOrder /*order*/) override
  {
    return GHOST_kSuccess;
  }
};
