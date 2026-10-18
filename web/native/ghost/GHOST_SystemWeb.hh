/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Browser (Emscripten) GHOST system. Input arrives through emscripten/html5.h
 * callbacks registered on the calling (Blender main) thread; under
 * PROXY_TO_PTHREAD they are delivered whenever that thread yields to its event
 * loop — which it does between iterations of the emscripten-driven WM main
 * loop. Exactly one window exists, mapped to the page's #canvas.
 */

#pragma once

#include "GHOST_ContextNone.hh"
#include "GHOST_System.hh"
#include "GHOST_WindowWeb.hh"

class GHOST_SystemWeb : public GHOST_System {
 private:
  GHOST_WindowWeb *window_ = nullptr;
  int32_t cursor_x_ = 0, cursor_y_ = 0;
  GHOST_Buttons buttons_;
  GHOST_ModifierKeys modifiers_;
  uint32_t canvas_w_ = 1280, canvas_h_ = 720;
  float canvas_scale_ = 1.0f;

 public:
  GHOST_SystemWeb();
  ~GHOST_SystemWeb() override = default;

  bool processEvents(bool waitForEvent) override;

  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }
  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override
  {
    keys = modifiers_;
    return GHOST_kSuccess;
  }
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override
  {
    buttons = buttons_;
    return GHOST_kSuccess;
  }
  GHOST_TCapabilityFlag getCapabilities() const override
  {
    return GHOST_TCapabilityFlag(
        GHOST_CAPABILITY_FLAG_ALL &
        ~(GHOST_kCapabilityWindowPosition | GHOST_kCapabilityCursorWarp |
          GHOST_kCapabilityClipboardPrimary | GHOST_kCapabilityClipboardImage |
          GHOST_kCapabilityDesktopSample | GHOST_kCapabilityInputIME |
          GHOST_kCapabilityWindowDecorationStyles | GHOST_kCapabilityKeyboardHyperKey |
          GHOST_kCapabilityCursorRGBA | GHOST_kCapabilityCursorGenerator));
  }
  char *getClipboard(bool /*selection*/) const override
  {
    return nullptr;
  }
  void putClipboard(const char * /*buffer*/, bool /*selection*/) const override {}

  uint64_t getMilliSeconds() const override;

  uint8_t getNumDisplays() const override
  {
    return 1;
  }
  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override
  {
    x = cursor_x_;
    y = cursor_y_;
    return GHOST_kSuccess;
  }
  GHOST_TSuccess setCursorPosition(int32_t /*x*/, int32_t /*y*/) override
  {
    return GHOST_kFailure;
  }
  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override
  {
    width = canvas_w_;
    height = canvas_h_;
  }
  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override
  {
    getMainDisplayDimensions(width, height);
  }

  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpu_settings) override
  {
    /* The WebGPU backend acquires its device independently of GHOST, and there
     * is exactly one device: hand back a no-op context so secondary-context
     * creation (IMB_ensure_gpu_context via F12 render) has a non-null object
     * to activate/release instead of CRASHING on a null virtual call. The
     * resulting GPUContext shares the global WebGPU device. */
    GHOST_ContextParams params = {};
    params.is_stereo_visual = bool(gpu_settings.flags & GHOST_gpuStereoVisual);
    return new GHOST_ContextNone(params);
  }
  GHOST_TSuccess disposeContext(GHOST_IContext *context) override
  {
    delete context;
    return GHOST_kSuccess;
  }

  GHOST_IWindow *createWindow(const char *title,
                              int32_t /*left*/,
                              int32_t /*top*/,
                              uint32_t /*width*/,
                              uint32_t /*height*/,
                              GHOST_TWindowState state,
                              GHOST_GPUSettings gpu_settings,
                              const bool /*exclusive*/,
                              const bool /*is_dialog*/,
                              const GHOST_IWindow * /*parent_window*/) override;

  GHOST_IWindow *getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/) override
  {
    return window_;
  }

  GHOST_WindowWeb *window_get()
  {
    return window_;
  }

  /* html5 callback handlers (public: called from static trampolines). */
  bool handleMouse(int event_type, const struct EmscriptenMouseEvent *e);
  bool handleWheel(const struct EmscriptenWheelEvent *e);
  bool handleKey(int event_type, const struct EmscriptenKeyboardEvent *e);
  bool handleResize();

 protected:
  GHOST_TSuccess init() override;
};

GHOST_ISystem *GHOST_createWebSystem();
