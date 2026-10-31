/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Touch, pen and on-screen modifier input for the browser.
 *
 * `touch.js` listens to pointer events on the browser's main thread and writes
 * them into a ring in shared Wasm memory; the Blender thread drains it on each
 * event-loop pass. The other way, a small status block tells the page where
 * to draw the virtual cursor and how to show the modifier keys.
 */

#pragma once

#include "GHOST_TouchGestures.hh"

#include <cstdint>

class GHOST_SystemWeb;
class GHOST_WindowWeb;

class GHOST_WebTouch : public GHOST_TouchSink {
 public:
  explicit GHOST_WebTouch(GHOST_SystemWeb &system);
  ~GHOST_WebTouch() override;

  /** Allocate the shared buffers and start listening on the page. */
  void install();
  /** Drain queued input and resolve timed gestures. Returns true if events were pushed. */
  bool process(GHOST_WindowWeb *window);

  void pointerChanged(const GHOST_TouchTablet &tablet) override;
  void button(GHOST_TouchPointerButton button,
              bool down,
              const GHOST_TouchTablet &tablet) override;
  void modifier(GHOST_TouchModifier modifier, bool down) override;
  void scroll(double delta_x, double delta_y, int fingers) override;
  void magnify(double delta) override;
  void shortcut(GHOST_TouchShortcut shortcut) override;

  /* Shared layout, mirrored in touch.js. */
  static constexpr int ring_capacity = 1024;
  static constexpr int record_ints = 8;
  static constexpr int text_bytes = 16384;
  enum Kind : int32_t {
    TouchDown = 1,
    TouchMove,
    TouchUp,
    TouchCancel,
    PenDown,
    PenMove,
    PenUp,
    PenHover,
    ModifierDown,
    ModifierUp,
    ModifierCancel,
    TextCommit,
    TextCancel,
    Blur,
  };
  enum Status : int32_t {
    StatusX = 0,
    StatusY,
    StatusFlags,
    StatusModifiers,
    StatusSerial,
    StatusCount,
  };
  enum Flags : int32_t {
    FlagCursorVisible = 1 << 0,
    FlagTouchActive = 1 << 1,
  };

 private:
  void apply(const int32_t *record, double now);
  void key(int key, bool down, const char *utf8 = nullptr);
  void commitText(int length);
  void writeStatus();
  double now() const;

  GHOST_SystemWeb &system_;
  GHOST_WindowWeb *window_ = nullptr;
  GHOST_TouchPointerState pointer_;
  GHOST_TouchModifierLatch latch_;
  GHOST_TouchGestures gestures_;
  int32_t *ring_ = nullptr;
  float *status_ = nullptr;
  char *text_ = nullptr;
  bool pushed_ = false;
  bool touch_active_ = false;
  double scroll_rest_x_ = 0.0, scroll_rest_y_ = 0.0, zoom_rest_ = 0.0;
  int32_t last_x_ = 0, last_y_ = 0;
};
