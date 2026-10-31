/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_TouchGestures.hh"

#include <algorithm>
#include <cmath>

namespace tuning = GHOST_TouchInputTuning;

static const GHOST_TouchTablet no_tablet{};

GHOST_TouchGestures::GHOST_TouchGestures(GHOST_TouchPointerState &pointer,
                                         GHOST_TouchModifierLatch &latch,
                                         GHOST_TouchSink &sink)
    : pointer_(pointer), latch_(latch), sink_(sink)
{
}

GHOST_TouchGestures::Touch *GHOST_TouchGestures::find(const int id)
{
  for (Touch &touch : touches_) {
    if (touch.id == id) {
      return &touch;
    }
  }
  return nullptr;
}

void GHOST_TouchGestures::centroid(double &x, double &y) const
{
  x = y = 0.0;
  for (const Touch &touch : touches_) {
    x += touch.x;
    y += touch.y;
  }
  x /= double(touches_.size());
  y /= double(touches_.size());
}

double GHOST_TouchGestures::spread() const
{
  if (touches_.size() < 2) {
    return 0.0;
  }
  return std::hypot(touches_[1].x - touches_[0].x, touches_[1].y - touches_[0].y);
}

void GHOST_TouchGestures::flushModifiers()
{
  GHOST_TouchModifierChange changes[GHOST_TOUCH_MODIFIER_COUNT];
  const int count = latch_.takeChanges(changes);
  for (int i = 0; i < count; i++) {
    sink_.modifier(changes[i].modifier, changes[i].down);
  }
}

void GHOST_TouchGestures::press(const GHOST_TouchPointerButton button,
                                const GHOST_TouchTablet &tablet)
{
  latch_.pointerPressed();
  flushModifiers();
  if (pointer_.setButton(button, true)) {
    sink_.button(button, true, tablet);
  }
}

void GHOST_TouchGestures::release(const GHOST_TouchPointerButton button,
                                  const GHOST_TouchTablet &tablet)
{
  if (pointer_.setButton(button, false)) {
    sink_.button(button, false, tablet);
  }
  latch_.pointerReleased();
  flushModifiers();
}

void GHOST_TouchGestures::click(const int count)
{
  for (int i = 0; i < count; i++) {
    press(GHOST_TouchPointerButton::Left, no_tablet);
    release(GHOST_TouchPointerButton::Left, no_tablet);
  }
}

void GHOST_TouchGestures::finishPendingTap()
{
  if (!tap_pending_) {
    return;
  }
  tap_pending_ = false;
  /* As with UIKit's single-tap recognizer waiting for the triple tap to fail,
   * one or two quick taps are one click. */
  click(1);
  tap_count_ = 0;
}

void GHOST_TouchGestures::setShift(const bool down)
{
  if (shift_held_ == down) {
    return;
  }
  shift_held_ = down;
  /* A belt Shift already sent to Blender stays down on its own. */
  if (!latch_.sent(GHOST_TouchModifier::Shift)) {
    sink_.modifier(GHOST_TouchModifier::Shift, down);
  }
}

void GHOST_TouchGestures::beginMulti(const double time)
{
  if (phase_ == Phase::OneMove || phase_ == Phase::OneDown || phase_ == Phase::SecondPress) {
    pointer_.endRelative();
  }
  phase_ = Phase::Multi;
  multi_start_ = time;
  multi_navigating_ = false;
  centroid(last_x_, last_y_);
  last_spread_ = spread();
  last_fingers_ = int(touches_.size());
  for (Touch &touch : touches_) {
    touch.start_x = touch.x;
    touch.start_y = touch.y;
  }
}

void GHOST_TouchGestures::touchDown(const int id, const double x, const double y, const double time)
{
  if (find(id) != nullptr) {
    return;
  }
  touches_.push_back({id, x, y, x, y});

  if (touches_.size() == 1) {
    sequence_start_ = time;
    press_start_ = time;
    max_fingers_ = 1;
    moved_ = false;
    if (tap_pending_ && time - last_tap_ <= tuning::multi_tap_interval_seconds) {
      phase_ = Phase::SecondPress;
    }
    else {
      finishPendingTap();
      phase_ = Phase::OneDown;
    }
    pointer_.beginRelative(x, y, time);
    return;
  }

  max_fingers_ = std::max(max_fingers_, int(touches_.size()));
  switch (phase_) {
    case Phase::Drag:
      /* Extra fingers during a drag are resting fingers; keep the drag. */
      return;
    case Phase::Multi:
      /* Restart the reference so the centroid does not jump. */
      centroid(last_x_, last_y_);
      last_spread_ = spread();
      last_fingers_ = int(touches_.size());
      if (!multi_navigating_ && !right_held_) {
        multi_start_ = time;
      }
      touches_.back().start_x = x;
      touches_.back().start_y = y;
      return;
    case Phase::Ending:
      return;
    default:
      finishPendingTap();
      beginMulti(time);
      return;
  }
}

void GHOST_TouchGestures::touchMove(const int id, const double x, const double y, const double time)
{
  Touch *touch = find(id);
  if (touch == nullptr) {
    return;
  }
  touch->x = x;
  touch->y = y;
  const double travelled = std::hypot(x - touch->start_x, y - touch->start_y);

  switch (phase_) {
    case Phase::OneDown:
      if (travelled <= tuning::tap_slop_points) {
        return;
      }
      moved_ = true;
      phase_ = Phase::OneMove;
      [[fallthrough]];
    case Phase::OneMove:
      pointer_.moveRelativeTo(x, y, time);
      sink_.pointerChanged(no_tablet);
      return;
    case Phase::SecondPress:
      if (travelled <= tuning::tap_slop_points) {
        return;
      }
      /* Moving without the short hold: the earlier tap was a click and this
       * finger moves the cursor. */
      moved_ = true;
      finishPendingTap();
      phase_ = Phase::OneMove;
      pointer_.moveRelativeTo(x, y, time);
      sink_.pointerChanged(no_tablet);
      return;
    case Phase::Drag: {
      if (touches_.size() != 1 && touch != &touches_.front()) {
        return;
      }
      pointer_.moveRelativeTo(x, y, time);
      sink_.pointerChanged(no_tablet);
      return;
    }
    case Phase::Multi: {
      double cx, cy;
      centroid(cx, cy);
      const double dx = cx - last_x_, dy = cy - last_y_;
      const double current_spread = spread();
      const double pinch = current_spread - last_spread_;
      last_x_ = cx;
      last_y_ = cy;
      last_spread_ = current_spread;

      if (right_held_) {
        pointer_.moveRelativeTo(cx, cy, time);
        sink_.pointerChanged(no_tablet);
        return;
      }
      if (!multi_navigating_) {
        bool beyond = false;
        for (const Touch &other : touches_) {
          beyond |= std::hypot(other.x - other.start_x, other.y - other.start_y) >
                    tuning::two_finger_right_click_slop_points;
        }
        if (!beyond) {
          return;
        }
        multi_navigating_ = true;
        moved_ = true;
      }
      const int fingers = int(touches_.size());
      if (fingers == 2) {
        if (dx != 0.0 || dy != 0.0) {
          sink_.scroll(dx, dy, 2);
        }
        if (pinch != 0.0) {
          sink_.magnify(pinch);
        }
      }
      else if (fingers == 3) {
        setShift(true);
        if (dx != 0.0 || dy != 0.0) {
          sink_.scroll(dx, dy, 3);
        }
      }
      return;
    }
    case Phase::Idle:
    case Phase::Ending:
      return;
  }
}

void GHOST_TouchGestures::touchUp(const int id, const double x, const double y, const double time)
{
  Touch *touch = find(id);
  if (touch == nullptr) {
    return;
  }
  touch->x = x;
  touch->y = y;
  if (phase_ == Phase::OneMove || phase_ == Phase::Drag) {
    if (touches_.size() == 1) {
      pointer_.moveRelativeTo(x, y, time);
      sink_.pointerChanged(no_tablet);
    }
  }
  touches_.erase(touches_.begin() + (touch - touches_.data()));

  if (!touches_.empty()) {
    if (phase_ == Phase::Multi) {
      if (right_held_) {
        /* Keep the right button until every finger lifts. */
        centroid(last_x_, last_y_);
        pointer_.beginRelative(last_x_, last_y_, time);
        return;
      }
      setShift(false);
      if (multi_navigating_) {
        phase_ = Phase::Ending;
        return;
      }
      /* A finger lifted before navigation: may still be a multi-finger tap. */
      centroid(last_x_, last_y_);
      last_spread_ = spread();
      return;
    }
    return;
  }

  const Phase phase = phase_;
  phase_ = Phase::Idle;
  pointer_.endRelative();
  switch (phase) {
    case Phase::OneDown:
      if (time - press_start_ <= tuning::tap_max_seconds) {
        tap_count_ = 1;
        tap_pending_ = true;
        last_tap_ = time;
      }
      return;
    case Phase::SecondPress:
      tap_count_++;
      if (tap_count_ >= 3) {
        /* A triple tap is the desktop double click. */
        tap_pending_ = false;
        tap_count_ = 0;
        click(2);
        return;
      }
      tap_pending_ = true;
      last_tap_ = time;
      return;
    case Phase::Drag:
      release(GHOST_TouchPointerButton::Left, no_tablet);
      return;
    case Phase::Multi:
      setShift(false);
      if (right_held_) {
        right_held_ = false;
        release(GHOST_TouchPointerButton::Right, no_tablet);
        return;
      }
      if (!moved_ && time - sequence_start_ <= tuning::multi_finger_tap_max_seconds) {
        if (max_fingers_ == 2) {
          sink_.shortcut(GHOST_TouchShortcut::Undo);
        }
        else if (max_fingers_ == 3) {
          sink_.shortcut(GHOST_TouchShortcut::Redo);
        }
        else if (max_fingers_ == 4) {
          sink_.shortcut(GHOST_TouchShortcut::Search);
        }
      }
      return;
    case Phase::Ending:
      setShift(false);
      return;
    case Phase::OneMove:
    case Phase::Idle:
      return;
  }
}

void GHOST_TouchGestures::touchCancel(const int id, const double /*time*/)
{
  Touch *touch = find(id);
  if (touch == nullptr) {
    return;
  }
  touches_.erase(touches_.begin() + (touch - touches_.data()));
  if (!touches_.empty()) {
    if (phase_ == Phase::Multi && !right_held_) {
      setShift(false);
      phase_ = Phase::Ending;
    }
    return;
  }
  const Phase phase = phase_;
  phase_ = Phase::Idle;
  pointer_.endRelative();
  setShift(false);
  if (phase == Phase::Drag) {
    release(GHOST_TouchPointerButton::Left, no_tablet);
  }
  else if (right_held_) {
    right_held_ = false;
    release(GHOST_TouchPointerButton::Right, no_tablet);
  }
}

void GHOST_TouchGestures::update(const double time)
{
  if (tap_pending_ && touches_.empty() &&
      time - last_tap_ > tuning::multi_tap_interval_seconds)
  {
    finishPendingTap();
  }
  if (phase_ == Phase::SecondPress && touches_.size() == 1 &&
      time - press_start_ >= tuning::tap_drag_hold_seconds)
  {
    /* Tap, then hold: the left button is down until this finger lifts. */
    tap_pending_ = false;
    tap_count_ = 0;
    phase_ = Phase::Drag;
    press(GHOST_TouchPointerButton::Left, no_tablet);
  }
  if (phase_ == Phase::Multi && touches_.size() == 2 && !multi_navigating_ && !right_held_ &&
      time - multi_start_ >= tuning::two_finger_right_click_hold_seconds)
  {
    right_held_ = true;
    moved_ = true;
    double cx, cy;
    centroid(cx, cy);
    pointer_.beginRelative(cx, cy, time);
    press(GHOST_TouchPointerButton::Right, no_tablet);
  }
}

void GHOST_TouchGestures::penDown(const double x,
                                  const double y,
                                  const double time,
                                  const GHOST_TouchTablet &tablet,
                                  const bool secondary)
{
  (void)time;
  finishPendingTap();
  pencil_.touchBegan(x, y);
  pencil_.strokeBegan();
  pointer_.moveAbsolute(x, y, GHOST_TouchPointerSource::Pencil);
  sink_.pointerChanged(tablet);
  pen_down_ = true;
  pen_button_ = secondary ? GHOST_TouchPointerButton::Right : GHOST_TouchPointerButton::Left;
  press(pen_button_, tablet);
}

void GHOST_TouchGestures::penMove(const double x,
                                  const double y,
                                  const double /*time*/,
                                  const GHOST_TouchTablet &tablet)
{
  if (!pen_down_) {
    return;
  }
  pencil_.touchMoved(x, y);
  pencil_.strokeMoved(x, y);
  pointer_.moveAbsolute(x, y, GHOST_TouchPointerSource::Pencil);
  sink_.pointerChanged(tablet);
}

void GHOST_TouchGestures::penUp(const double x,
                                const double y,
                                const double /*time*/,
                                const GHOST_TouchTablet &tablet)
{
  if (!pen_down_) {
    return;
  }
  pencil_.touchEnded(x, y);
  pointer_.moveAbsolute(pencil_.x(), pencil_.y(), GHOST_TouchPointerSource::Pencil);
  sink_.pointerChanged(tablet);
  pen_down_ = false;
  release(pen_button_, tablet);
  pencil_.strokeEnded();
}

void GHOST_TouchGestures::penHover(const double x, const double y, const double /*time*/)
{
  if (!pencil_.acceptsHover()) {
    return;
  }
  pointer_.moveAbsolute(x, y, GHOST_TouchPointerSource::Pencil);
  sink_.pointerChanged(no_tablet);
}

void GHOST_TouchGestures::modifierDown(const GHOST_TouchModifier modifier, const double time)
{
  latch_.keyDown(modifier, time);
  flushModifiers();
}

void GHOST_TouchGestures::modifierUp(const GHOST_TouchModifier modifier, const double time)
{
  latch_.keyUp(modifier, time);
  flushModifiers();
}

void GHOST_TouchGestures::modifierCancel(const GHOST_TouchModifier modifier)
{
  latch_.keyCancelled(modifier);
  flushModifiers();
}

void GHOST_TouchGestures::reset()
{
  touches_.clear();
  if (pointer_.buttonDown(GHOST_TouchPointerButton::Left)) {
    release(GHOST_TouchPointerButton::Left, no_tablet);
  }
  if (pointer_.buttonDown(GHOST_TouchPointerButton::Right)) {
    release(GHOST_TouchPointerButton::Right, no_tablet);
  }
  pointer_.clearButtons();
  pointer_.endRelative();
  setShift(false);
  latch_.clear();
  flushModifiers();
  pencil_.reset();
  phase_ = Phase::Idle;
  tap_pending_ = false;
  tap_count_ = 0;
  right_held_ = false;
  multi_navigating_ = false;
  pen_down_ = false;
}
