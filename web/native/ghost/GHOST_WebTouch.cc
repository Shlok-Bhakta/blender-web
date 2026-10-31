/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_WebTouch.hh"

#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventKey.hh"
#include "GHOST_EventTrackpad.hh"
#include "GHOST_SystemWeb.hh"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

#include <emscripten/emscripten.h>

/* GHOST_WindowWeb keeps its overrides protected; use the public interface. */
static void client_bounds(const GHOST_IWindow *window, GHOST_Rect &bounds)
{
  window->getClientBounds(bounds);
}

/* Both threads read the same clock: the page's time origin plus `now()`. */
static double web_clock_ms()
{
  return EM_ASM_DOUBLE({ return performance.timeOrigin + performance.now(); });
}
static double touch_epoch_ms = 0.0;

GHOST_WebTouch::GHOST_WebTouch(GHOST_SystemWeb &system)
    : system_(system), gestures_(pointer_, latch_, *this)
{
}

GHOST_WebTouch::~GHOST_WebTouch()
{
  /* The page keeps references to these buffers for the life of the process. */
}

double GHOST_WebTouch::now() const
{
  return (web_clock_ms() - touch_epoch_ms) / 1000.0;
}

void GHOST_WebTouch::install()
{
  if (ring_ != nullptr) {
    return;
  }
  ring_ = static_cast<int32_t *>(calloc(4 + ring_capacity * record_ints, sizeof(int32_t)));
  status_ = static_cast<float *>(calloc(StatusCount, sizeof(float)));
  text_ = static_cast<char *>(calloc(text_bytes, 1));
  touch_epoch_ms = web_clock_ms();
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        if (Module.webTouchInstall) {
          Module.webTouchInstall($0, $1, $2, $3, $4, $5);
        }
      },
      ring_,
      ring_capacity,
      record_ints,
      status_,
      text_,
      touch_epoch_ms);
}

bool GHOST_WebTouch::process(GHOST_WindowWeb *window)
{
  if (ring_ == nullptr || window == nullptr) {
    return false;
  }
  window_ = window;
  pushed_ = false;
  GHOST_Rect bounds;
  client_bounds(window, bounds);
  pointer_.initialize(bounds.getWidth(), bounds.getHeight(), 1.0);
  pointer_.setBlenderVisibility(window->getCursorVisibility());

  const double time = now();
  const uint32_t written = __atomic_load_n(reinterpret_cast<uint32_t *>(&ring_[0]),
                                           __ATOMIC_ACQUIRE);
  uint32_t read = uint32_t(ring_[1]);
  if (written - read > uint32_t(ring_capacity)) {
    /* The page outran us; drop what was overwritten rather than replay stale input. */
    read = written - ring_capacity;
  }
  for (; read != written; read++) {
    apply(&ring_[4 + (read % ring_capacity) * record_ints], time);
  }
  __atomic_store_n(reinterpret_cast<uint32_t *>(&ring_[1]), read, __ATOMIC_RELEASE);

  gestures_.update(time);
  writeStatus();
  return pushed_;
}

void GHOST_WebTouch::apply(const int32_t *record, const double now_seconds)
{
  float values[5];
  std::memcpy(values, &record[2], sizeof(values));
  const int kind = record[0], id = record[1];
  const double x = values[0], y = values[1];
  const double time = record[7] / 1000.0;
  GHOST_TouchTablet tablet;
  tablet.active = true;
  tablet.pressure = values[2];
  tablet.tilt_x = values[3];
  tablet.tilt_y = values[4];
  (void)now_seconds;

  if (kind >= TouchDown && kind <= PenHover) {
    touch_active_ = true;
  }
  switch (kind) {
    case TouchDown:
      gestures_.touchDown(id, x, y, time);
      break;
    case TouchMove:
      gestures_.touchMove(id, x, y, time);
      break;
    case TouchUp:
      gestures_.touchUp(id, x, y, time);
      break;
    case TouchCancel:
      gestures_.touchCancel(id, time);
      break;
    case PenDown:
      gestures_.penDown(x, y, time, tablet, id != 0);
      break;
    case PenMove:
      gestures_.penMove(x, y, time, tablet);
      break;
    case PenUp:
      gestures_.penUp(x, y, time, tablet);
      break;
    case PenHover:
      gestures_.penHover(x, y, time);
      break;
    case ModifierDown:
      gestures_.modifierDown(GHOST_TouchModifier(id), time);
      break;
    case ModifierUp:
      gestures_.modifierUp(GHOST_TouchModifier(id), time);
      break;
    case ModifierCancel:
      gestures_.modifierCancel(GHOST_TouchModifier(id));
      break;
    case TextCommit:
      commitText(id);
      break;
    case TextCancel:
      key(GHOST_kKeyEsc, true);
      key(GHOST_kKeyEsc, false);
      break;
    case Blur:
      gestures_.reset();
      touch_active_ = false;
      break;
    default:
      break;
  }
}

void GHOST_WebTouch::pointerChanged(const GHOST_TouchTablet &tablet)
{
  GHOST_WindowWeb *window = window_;
  int32_t event_x, event_y;
  const GHOST_TGrabCursorMode grab = window->getCursorGrabMode();
  if (grab == GHOST_kGrabWrap || grab == GHOST_kGrabHide) {
    GHOST_Rect bounds;
    if (window->getCursorGrabBounds(bounds) != GHOST_kSuccess) {
      client_bounds(window, bounds);
    }
    /* Hidden grabs keep the virtual cursor far from the edges, as on iOS. */
    const double margin = grab == GHOST_kGrabHide ? double(bounds.getWidth()) / 10.0 : 2.0;
    const GHOST_TAxisFlag axis = window->getCursorGrabAxis();
    const GHOST_TouchPointerWrapOffset offset = pointer_.wrapToBounds(
        bounds.l_, bounds.t_, bounds.r_, bounds.b_, margin, axis & GHOST_kAxisX, axis & GHOST_kAxisY);
    pointer_.clampToBounds(bounds.l_, bounds.t_, bounds.r_, bounds.b_);
    int32_t accum_x, accum_y;
    window->getCursorGrabAccum(accum_x, accum_y);
    accum_x += int32_t(std::lround(offset.x));
    accum_y += int32_t(std::lround(offset.y));
    window->setCursorGrabAccum(accum_x, accum_y);
    event_x = int32_t(std::lround(pointer_.x())) + accum_x;
    event_y = int32_t(std::lround(pointer_.y())) + accum_y;
  }
  else {
    GHOST_Rect bounds;
    client_bounds(window, bounds);
    if (GHOST_TouchInputTuning::pointer_always_wrap &&
        pointer_.source() != GHOST_TouchPointerSource::Pencil)
    {
      pointer_.wrapToBounds(bounds.l_, bounds.t_, bounds.r_, bounds.b_, 2.0, true, true);
    }
    else {
      pointer_.clampToBounds(bounds.l_, bounds.t_, bounds.r_, bounds.b_);
    }
    event_x = int32_t(std::lround(pointer_.x()));
    event_y = int32_t(std::lround(pointer_.y()));
  }
  last_x_ = event_x;
  last_y_ = event_y;
  system_.cursor_x_ = int32_t(std::lround(pointer_.x()));
  system_.cursor_y_ = int32_t(std::lround(pointer_.y()));
  const GHOST_TabletData data = tablet.active ?
                                    GHOST_TabletData{GHOST_kTabletModeStylus,
                                                     tablet.pressure,
                                                     tablet.tilt_x,
                                                     tablet.tilt_y} :
                                    GHOST_TABLET_DATA_NONE;
  system_.pushEvent(std::make_unique<GHOST_EventCursor>(
      system_.getMilliSeconds(), GHOST_kEventCursorMove, window, event_x, event_y, data));
  pushed_ = true;
}

void GHOST_WebTouch::button(const GHOST_TouchPointerButton button,
                            const bool down,
                            const GHOST_TouchTablet &tablet)
{
  const GHOST_TButton mask = button == GHOST_TouchPointerButton::Left  ? GHOST_kButtonMaskLeft :
                             button == GHOST_TouchPointerButton::Right ? GHOST_kButtonMaskRight :
                                                                         GHOST_kButtonMaskMiddle;
  system_.buttons_.set(mask, down);
  const GHOST_TabletData data = tablet.active ?
                                    GHOST_TabletData{GHOST_kTabletModeStylus,
                                                     tablet.pressure,
                                                     tablet.tilt_x,
                                                     tablet.tilt_y} :
                                    GHOST_TABLET_DATA_NONE;
  system_.pushEvent(std::make_unique<GHOST_EventButton>(system_.getMilliSeconds(),
                                                        down ? GHOST_kEventButtonDown :
                                                               GHOST_kEventButtonUp,
                                                        window_,
                                                        mask,
                                                        data));
  pushed_ = true;
}

void GHOST_WebTouch::key(const int key_code, const bool down, const char *utf8)
{
  const GHOST_TKey key = GHOST_TKey(key_code);
  switch (key) {
    case GHOST_kKeyLeftShift:
      system_.modifiers_.set(GHOST_kModifierKeyLeftShift, down);
      break;
    case GHOST_kKeyLeftControl:
      system_.modifiers_.set(GHOST_kModifierKeyLeftControl, down);
      break;
    case GHOST_kKeyLeftAlt:
      system_.modifiers_.set(GHOST_kModifierKeyLeftAlt, down);
      break;
    default:
      break;
  }
  system_.pushEvent(std::make_unique<GHOST_EventKey>(system_.getMilliSeconds(),
                                                     down ? GHOST_kEventKeyDown :
                                                            GHOST_kEventKeyUp,
                                                     window_,
                                                     key,
                                                     false,
                                                     down ? utf8 : nullptr));
  pushed_ = true;
}

void GHOST_WebTouch::modifier(const GHOST_TouchModifier modifier, const bool down)
{
  key(modifier == GHOST_TouchModifier::Shift   ? GHOST_kKeyLeftShift :
      modifier == GHOST_TouchModifier::Control ? GHOST_kKeyLeftControl :
                                                 GHOST_kKeyLeftAlt,
      down);
}

void GHOST_WebTouch::scroll(const double delta_x, const double delta_y, const int /*fingers*/)
{
  /* GHOST trackpad deltas are integers; carry the remainder so slow motion still counts. */
  scroll_rest_x_ += delta_x;
  scroll_rest_y_ += delta_y;
  const int32_t dx = int32_t(std::trunc(scroll_rest_x_));
  const int32_t dy = int32_t(std::trunc(scroll_rest_y_));
  if (dx == 0 && dy == 0) {
    return;
  }
  scroll_rest_x_ -= dx;
  scroll_rest_y_ -= dy;
  system_.pushEvent(std::make_unique<GHOST_EventTrackpad>(system_.getMilliSeconds(),
                                                          window_,
                                                          GHOST_kTrackpadEventScroll,
                                                          last_x_,
                                                          last_y_,
                                                          dx,
                                                          dy,
                                                          true));
  pushed_ = true;
}

void GHOST_WebTouch::magnify(const double delta)
{
  zoom_rest_ += delta;
  const int32_t amount = int32_t(std::trunc(zoom_rest_));
  if (amount == 0) {
    return;
  }
  zoom_rest_ -= amount;
  system_.pushEvent(std::make_unique<GHOST_EventTrackpad>(system_.getMilliSeconds(),
                                                          window_,
                                                          GHOST_kTrackpadEventMagnify,
                                                          last_x_,
                                                          last_y_,
                                                          amount,
                                                          0,
                                                          false));
  pushed_ = true;
}

void GHOST_WebTouch::shortcut(const GHOST_TouchShortcut shortcut)
{
  switch (shortcut) {
    case GHOST_TouchShortcut::Undo:
    case GHOST_TouchShortcut::Redo: {
      const bool redo = shortcut == GHOST_TouchShortcut::Redo;
      key(GHOST_kKeyLeftControl, true);
      if (redo) {
        key(GHOST_kKeyLeftShift, true);
      }
      key(GHOST_kKeyZ, true, redo ? "Z" : "z");
      key(GHOST_kKeyZ, false);
      if (redo) {
        key(GHOST_kKeyLeftShift, false);
      }
      key(GHOST_kKeyLeftControl, false);
      break;
    }
    case GHOST_TouchShortcut::Search:
      key(GHOST_kKeyF3, true);
      key(GHOST_kKeyF3, false);
      break;
  }
}

void GHOST_WebTouch::commitText(const int length)
{
  if (length < 0 || length >= text_bytes) {
    return;
  }
  /* Replace the field's text: select everything, type the edit, confirm. */
  key(GHOST_kKeyLeftControl, true);
  key(GHOST_kKeyA, true, "a");
  key(GHOST_kKeyA, false);
  key(GHOST_kKeyLeftControl, false);
  const char *text = text_;
  int offset = 0;
  while (offset < length) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    const int size = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    if (offset + size > length) {
      break;
    }
    char utf8[6] = {};
    std::memcpy(utf8, text + offset, size);
    key(GHOST_kKeyUnknown, true, utf8);
    key(GHOST_kKeyUnknown, false);
    offset += size;
  }
  key(GHOST_kKeyEnter, true);
  key(GHOST_kKeyEnter, false);
}

void GHOST_WebTouch::writeStatus()
{
  if (!touch_active_) {
    status_[StatusFlags] = 0.0f;
    return;
  }
  int32_t flags = FlagTouchActive;
  if (pointer_.visible() && window_->getCursorGrabMode() != GHOST_kGrabHide) {
    flags |= FlagCursorVisible;
  }
  int32_t modes = 0;
  for (int i = 0; i < GHOST_TOUCH_MODIFIER_COUNT; i++) {
    modes |= int32_t(latch_.mode(GHOST_TouchModifier(i))) << (i * 2);
  }
  status_[StatusX] = float(pointer_.x());
  status_[StatusY] = float(pointer_.y());
  status_[StatusFlags] = float(flags);
  status_[StatusModifiers] = float(modes);
  status_[StatusSerial] += 1.0f;
}

/* Text editing hooks, called from Blender's button text editing. */
extern "C" void GHOST_web_text_edit_begin(const char *text, const bool numeric)
{
  /* Synchronous: `text` belongs to the caller. Begin happens once per edit. */
  MAIN_THREAD_EM_ASM(
      {
        if (Module.webTextEditBegin) {
          Module.webTextEditBegin(UTF8ToString($0), !!$1);
        }
      },
      text ? text : "",
      numeric);
}

extern "C" void GHOST_web_text_edit_end()
{
  MAIN_THREAD_ASYNC_EM_ASM({
    if (Module.webTextEditEnd) {
      Module.webTextEditEnd();
    }
  });
}
