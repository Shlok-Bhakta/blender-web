/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Platform-neutral touch and pen gesture recognition.
 *
 * Reproduces the touch controls of the 3D Modeling Thingy iOS port, which uses
 * UIKit recognizers, from raw pointer events:
 * - One finger moves a virtual cursor like a trackpad; a tap clicks at the cursor.
 * - Tap, then hold and drag, is a left-button drag. A triple tap is a double click.
 * - A still two-finger hold is a right click; keep holding and move to drag.
 * - Two fingers orbit/scroll and pinch to zoom; three fingers pan.
 * - Two, three and four finger taps are Undo, Redo and Search.
 * - A pen points directly, with pressure and tilt.
 * Modifier keys from the on-screen belt go through #GHOST_TouchModifierLatch.
 */

#pragma once

#include "GHOST_TouchModifierLatch.hh"
#include "GHOST_TouchPencilContact.hh"
#include "GHOST_TouchPointerState.hh"

#include <vector>

struct GHOST_TouchTablet {
  bool active = false;
  float pressure = 1.0f;
  /** -1..1, positive to the right and towards the user. */
  float tilt_x = 0.0f;
  float tilt_y = 0.0f;
};

enum class GHOST_TouchShortcut : uint8_t {
  Undo,
  Redo,
  Search,
};

/** Receives what the gestures mean. Implemented by the platform glue. */
class GHOST_TouchSink {
 public:
  virtual ~GHOST_TouchSink() = default;
  /** The shared pointer state moved; report it (after applying any grab). */
  virtual void pointerChanged(const GHOST_TouchTablet &tablet) = 0;
  virtual void button(GHOST_TouchPointerButton button,
                      bool down,
                      const GHOST_TouchTablet &tablet) = 0;
  virtual void modifier(GHOST_TouchModifier modifier, bool down) = 0;
  /** Continuous trackpad-style scroll in client points. Three fingers hold Shift. */
  virtual void scroll(double delta_x, double delta_y, int fingers) = 0;
  /** Pinch distance change in client points. */
  virtual void magnify(double delta) = 0;
  virtual void shortcut(GHOST_TouchShortcut shortcut) = 0;
};

class GHOST_TouchGestures {
 public:
  GHOST_TouchGestures(GHOST_TouchPointerState &pointer,
                      GHOST_TouchModifierLatch &latch,
                      GHOST_TouchSink &sink);

  void touchDown(int id, double x, double y, double time);
  void touchMove(int id, double x, double y, double time);
  void touchUp(int id, double x, double y, double time);
  /** The browser took the touch away. Ends the gesture without tapping. */
  void touchCancel(int id, double time);

  /** \a secondary is the pen's barrel button, which right clicks. */
  void penDown(double x, double y, double time, const GHOST_TouchTablet &tablet, bool secondary);
  void penMove(double x, double y, double time, const GHOST_TouchTablet &tablet);
  void penUp(double x, double y, double time, const GHOST_TouchTablet &tablet);
  void penHover(double x, double y, double time);

  void modifierDown(GHOST_TouchModifier modifier, double time);
  void modifierUp(GHOST_TouchModifier modifier, double time);
  void modifierCancel(GHOST_TouchModifier modifier);

  /** Resolve timed gestures; call on every event-loop pass. */
  void update(double time);
  /** Release everything, for example when the page loses focus. */
  void reset();

  bool active() const
  {
    return !touches_.empty() || phase_ != Phase::Idle || tap_pending_;
  }

 private:
  enum class Phase : uint8_t {
    Idle,
    /** One finger is down and has not moved yet. */
    OneDown,
    /** One finger moves the cursor. */
    OneMove,
    /** A finger came down again soon after a tap: a repeated tap or a drag. */
    SecondPress,
    /** Tap, then hold and drag: the left button is down. */
    Drag,
    /** Two or more fingers. */
    Multi,
    /** Fingers lifted mid-gesture; wait for the rest without acting. */
    Ending,
  };
  struct Touch {
    int id;
    double x, y, start_x, start_y;
  };

  Touch *find(int id);
  void centroid(double &x, double &y) const;
  double spread() const;
  void beginMulti(double time);
  void flushModifiers();
  void press(GHOST_TouchPointerButton button, const GHOST_TouchTablet &tablet);
  void release(GHOST_TouchPointerButton button, const GHOST_TouchTablet &tablet);
  void click(int count);
  void finishPendingTap();
  void setShift(bool down);

  GHOST_TouchPointerState &pointer_;
  GHOST_TouchModifierLatch &latch_;
  GHOST_TouchPencilContact pencil_;
  GHOST_TouchSink &sink_;

  std::vector<Touch> touches_;
  Phase phase_ = Phase::Idle;
  int max_fingers_ = 0;
  bool moved_ = false;
  double sequence_start_ = 0.0;
  double press_start_ = 0.0;

  int tap_count_ = 0;
  bool tap_pending_ = false;
  double last_tap_ = 0.0;

  double multi_start_ = 0.0;
  bool multi_navigating_ = false;
  bool right_held_ = false;
  bool shift_held_ = false;
  double last_x_ = 0.0, last_y_ = 0.0, last_spread_ = 0.0;
  int last_fingers_ = 0;

  bool pen_down_ = false;
  GHOST_TouchPointerButton pen_button_ = GHOST_TouchPointerButton::Left;
};
