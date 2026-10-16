/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "intern/GHOST_WindowNULL.hh"

/** A canvas-backed window. Additional native windows currently fail explicitly. */
class GHOST_WindowWeb final : public GHOST_WindowNULL {
 public:
  GHOST_WindowWeb(const char *title, uint32_t width, uint32_t height);
  ~GHOST_WindowWeb() override;
  void setTitle(const char *title) override;
  std::string getTitle() const override;
  void getClientBounds(GHOST_Rect &bounds) const override;
  void getWindowBounds(GHOST_Rect &bounds) const override;
  GHOST_TSuccess setClientSize(uint32_t width, uint32_t height) override;
  GHOST_TSuccess setClientWidth(uint32_t width) override;
  GHOST_TSuccess setClientHeight(uint32_t height) override;
  GHOST_TSuccess invalidate() override;
  GHOST_TSuccess activateDrawingContext() override;
  GHOST_TSuccess swapBufferRelease() override;
  GHOST_TSuccess setState(GHOST_TWindowState state) override;
  GHOST_TSuccess setOrder(GHOST_TWindowOrder order) override;
  void resizeFromBrowser(uint32_t width, uint32_t height, float scale);

 protected:
  GHOST_TSuccess hasCursorShape(GHOST_TStandardCursor shape) override;
  GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor shape) override;
  GHOST_TSuccess setWindowCursorVisibility(bool visible) override;
  GHOST_TSuccess setWindowCursorGrab(GHOST_TGrabCursorMode mode) override;
  GHOST_TSuccess setWindowCustomCursorShape(const uint8_t *, const uint8_t *,
                                          const int[2], const int[2], bool) override;

 private:
  std::string title_;
  uint32_t width_, height_;
};
