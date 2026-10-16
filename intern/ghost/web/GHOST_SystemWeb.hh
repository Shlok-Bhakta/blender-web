/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef WITH_WEBGPU_BACKEND
#  include "native/ghost/GHOST_SystemWeb.hh"
#else

#include "intern/GHOST_SystemHeadless.hh"

class GHOST_WindowWeb;

/** Browser platform support. Unsupported desktop capabilities are not advertised. */
class GHOST_SystemWeb final : public GHOST_SystemHeadless {
 public:
  ~GHOST_SystemWeb() override;
  bool processEvents(bool wait_for_event) override;
  uint64_t getMilliSeconds() const override;
  GHOST_TCapabilityFlag getCapabilities() const override;
  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override;
  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override;
  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override;
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override;
  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override;
  GHOST_IWindow *getWindowUnderCursor(int32_t x, int32_t y) override;
  GHOST_IWindow *createWindow(const char *title,
                            int32_t left,
                            int32_t top,
                            uint32_t width,
                            uint32_t height,
                            GHOST_TWindowState state,
                            GHOST_GPUSettings gpu_settings,
                            bool exclusive,
                            bool is_dialog,
                            const GHOST_IWindow *parent_window) override;
  void pointer(int kind, int x, int y, int button);
  void key(const char *code, const char *text, bool down, bool repeat);
  void blur();
  void resize(uint32_t width, uint32_t height, float scale);
  void wheel(int axis, int ticks);

 private:
  GHOST_ModifierKeys modifiers_;
  GHOST_Buttons buttons_;
  int32_t cursor_x_ = 0, cursor_y_ = 0;
};

GHOST_ISystem *GHOST_createWebSystem();

#endif
