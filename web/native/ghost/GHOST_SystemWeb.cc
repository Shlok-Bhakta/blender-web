/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventKey.hh"
#include "GHOST_EventWheel.hh"
#include "GHOST_SystemWeb.hh"
#include "GHOST_TimerManager.hh"
#include "GHOST_WindowManager.hh"

#define WEB_CANVAS "#canvas"

/* --- Static trampolines ----------------------------------------------------- */

static bool web_mouse_cb(int type, const EmscriptenMouseEvent *e, void *user)
{
  return static_cast<GHOST_SystemWeb *>(user)->handleMouse(type, e);
}
static bool web_wheel_cb(int /*type*/, const EmscriptenWheelEvent *e, void *user)
{
  return static_cast<GHOST_SystemWeb *>(user)->handleWheel(e);
}
static bool web_key_cb(int type, const EmscriptenKeyboardEvent *e, void *user)
{
  return static_cast<GHOST_SystemWeb *>(user)->handleKey(type, e);
}
static bool web_resize_cb(int /*type*/, const EmscriptenUiEvent * /*e*/, void *user)
{
  return static_cast<GHOST_SystemWeb *>(user)->handleResize();
}

/* --- Key mapping ------------------------------------------------------------ */

static GHOST_TKey web_key_from_code(const char *code)
{
  /* `code` is the PHYSICAL key (KeyboardEvent.code). */
  if (strncmp(code, "Key", 3) == 0 && code[3] >= 'A' && code[3] <= 'Z' && code[4] == '\0') {
    return GHOST_TKey(int(GHOST_kKeyA) + (code[3] - 'A'));
  }
  if (strncmp(code, "Digit", 5) == 0 && code[5] >= '0' && code[5] <= '9' && code[6] == '\0') {
    return GHOST_TKey(int(GHOST_kKey0) + (code[5] - '0'));
  }
  if (strncmp(code, "Numpad", 6) == 0 && code[6] >= '0' && code[6] <= '9' && code[7] == '\0') {
    return GHOST_TKey(int(GHOST_kKeyNumpad0) + (code[6] - '0'));
  }
  if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
    int num = atoi(code + 1);
    if (num >= 1 && num <= 24) {
      return GHOST_TKey(int(GHOST_kKeyF1) + num - 1);
    }
  }
  struct {
    const char *code;
    GHOST_TKey key;
  } table[] = {
      {"Space", GHOST_kKeySpace},
      {"Enter", GHOST_kKeyEnter},
      {"NumpadEnter", GHOST_kKeyNumpadEnter},
      {"Escape", GHOST_kKeyEsc},
      {"Tab", GHOST_kKeyTab},
      {"Backspace", GHOST_kKeyBackSpace},
      {"Delete", GHOST_kKeyDelete},
      {"Insert", GHOST_kKeyInsert},
      {"Home", GHOST_kKeyHome},
      {"End", GHOST_kKeyEnd},
      {"PageUp", GHOST_kKeyUpPage},
      {"PageDown", GHOST_kKeyDownPage},
      {"ArrowLeft", GHOST_kKeyLeftArrow},
      {"ArrowRight", GHOST_kKeyRightArrow},
      {"ArrowUp", GHOST_kKeyUpArrow},
      {"ArrowDown", GHOST_kKeyDownArrow},
      {"ShiftLeft", GHOST_kKeyLeftShift},
      {"ShiftRight", GHOST_kKeyRightShift},
      {"ControlLeft", GHOST_kKeyLeftControl},
      {"ControlRight", GHOST_kKeyRightControl},
      {"AltLeft", GHOST_kKeyLeftAlt},
      {"AltRight", GHOST_kKeyRightAlt},
      {"MetaLeft", GHOST_kKeyLeftOS},
      {"MetaRight", GHOST_kKeyRightOS},
      {"Minus", GHOST_kKeyMinus},
      {"Equal", GHOST_kKeyEqual},
      {"BracketLeft", GHOST_kKeyLeftBracket},
      {"BracketRight", GHOST_kKeyRightBracket},
      {"Backslash", GHOST_kKeyBackslash},
      {"Semicolon", GHOST_kKeySemicolon},
      {"Quote", GHOST_kKeyQuote},
      {"Backquote", GHOST_kKeyAccentGrave},
      {"Comma", GHOST_kKeyComma},
      {"Period", GHOST_kKeyPeriod},
      {"Slash", GHOST_kKeySlash},
      {"CapsLock", GHOST_kKeyCapsLock},
      {"NumpadAdd", GHOST_kKeyNumpadPlus},
      {"NumpadSubtract", GHOST_kKeyNumpadMinus},
      {"NumpadMultiply", GHOST_kKeyNumpadAsterisk},
      {"NumpadDivide", GHOST_kKeyNumpadSlash},
      {"NumpadDecimal", GHOST_kKeyNumpadPeriod},
  };
  for (const auto &entry : table) {
    if (strcmp(code, entry.code) == 0) {
      return entry.key;
    }
  }
  return GHOST_kKeyUnknown;
}

/* --- System ------------------------------------------------------------------ */

GHOST_SystemWeb::GHOST_SystemWeb() : GHOST_System() {}

GHOST_TSuccess GHOST_SystemWeb::init()
{
  GHOST_TSuccess success = GHOST_System::init();
  if (!success) {
    return GHOST_kFailure;
  }

  /* GHOST bounds and input use CSS pixels. Blender multiplies by the native
   * pixel size for rendering, as on desktop high-DPI platforms. */
  handleResize();

  /* Input callbacks, delivered on this (the Blender main) thread whenever it
   * yields to its event loop. */
  emscripten_set_mousedown_callback(WEB_CANVAS, this, false, web_mouse_cb);
  emscripten_set_mouseup_callback(WEB_CANVAS, this, false, web_mouse_cb);
  emscripten_set_mousemove_callback(WEB_CANVAS, this, false, web_mouse_cb);
  emscripten_set_wheel_callback(WEB_CANVAS, this, false, web_wheel_cb);
  emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false, web_key_cb);
  emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false, web_key_cb);
  emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false, web_resize_cb);
  return GHOST_kSuccess;
}

uint64_t GHOST_SystemWeb::getMilliSeconds() const
{
  return uint64_t(emscripten_get_now());
}

bool GHOST_SystemWeb::processEvents(bool /*waitForEvent*/)
{
  /* html5 callbacks have already pushed into the event queue (they run when the
   * main-loop iteration yields). Fire timers too. */
  bool has_event = false;
  if (timer_manager_) {
    has_event |= timer_manager_->fireTimers(getMilliSeconds());
  }
  has_event |= (event_manager_->getNumEvents() > 0);
  return has_event;
}

GHOST_IWindow *GHOST_SystemWeb::createWindow(const char *title,
                                             int32_t /*left*/,
                                             int32_t /*top*/,
                                             uint32_t /*width*/,
                                             uint32_t /*height*/,
                                             GHOST_TWindowState state,
                                             GHOST_GPUSettings gpu_settings,
                                             const bool /*exclusive*/,
                                             const bool /*is_dialog*/,
                                             const GHOST_IWindow * /*parent_window*/)
{
  if (window_ != nullptr) {
    /* Single-window platform: additional windows (preferences, etc.) are not
     * supported; return null so WM falls back to opening in the main window. */
    return nullptr;
  }
  const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS(gpu_settings);
  window_ = new GHOST_WindowWeb(title, canvas_w_, canvas_h_, state, context_params);
  /* Register with the window manager so validWindow() passes — otherwise every
   * GHOST event is dropped as "invalid window" in wm_window's event handler. */
  window_manager_->addWindow(window_);
  window_->resize(canvas_w_, canvas_h_, canvas_scale_);
  window_manager_->setActiveWindow(window_);
  pushEvent(std::unique_ptr<const GHOST_IEvent>(
      new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowSize, window_)));
  return window_;
}

/* --- Event handlers ----------------------------------------------------------- */

bool GHOST_SystemWeb::handleMouse(int event_type, const EmscriptenMouseEvent *e)
{
  if (window_ == nullptr) {
    return false;
  }
  const uint64_t t = getMilliSeconds();
  cursor_x_ = int32_t(e->targetX);
  cursor_y_ = int32_t(e->targetY);

  modifiers_.set(GHOST_kModifierKeyLeftShift, e->shiftKey);
  modifiers_.set(GHOST_kModifierKeyLeftControl, e->ctrlKey);
  modifiers_.set(GHOST_kModifierKeyLeftAlt, e->altKey);

  switch (event_type) {
    case EMSCRIPTEN_EVENT_MOUSEMOVE:
      pushEvent(std::unique_ptr<const GHOST_IEvent>(new GHOST_EventCursor(
          t, GHOST_kEventCursorMove, window_, cursor_x_, cursor_y_, GHOST_TABLET_DATA_NONE)));
      return true;
    case EMSCRIPTEN_EVENT_MOUSEDOWN:
    case EMSCRIPTEN_EVENT_MOUSEUP: {
      GHOST_TButton button = (e->button == 0) ? GHOST_kButtonMaskLeft :
                             (e->button == 1) ? GHOST_kButtonMaskMiddle :
                                                GHOST_kButtonMaskRight;
      const bool down = (event_type == EMSCRIPTEN_EVENT_MOUSEDOWN);
      buttons_.set(button, down);
      pushEvent(std::unique_ptr<const GHOST_IEvent>(new GHOST_EventButton(t,
                                      down ? GHOST_kEventButtonDown : GHOST_kEventButtonUp,
                                      window_,
                                      button,
                                      GHOST_TABLET_DATA_NONE)));
      return true;
    }
    default:
      return false;
  }
}

bool GHOST_SystemWeb::handleWheel(const EmscriptenWheelEvent *e)
{
  if (window_ == nullptr || e->deltaY == 0.0) {
    return false;
  }
  const int32_t value = (e->deltaY > 0) ? -1 : 1;
  pushEvent(std::unique_ptr<const GHOST_IEvent>(new GHOST_EventWheel(getMilliSeconds(), window_, GHOST_kEventWheelAxisVertical, value)));
  return true;
}

bool GHOST_SystemWeb::handleKey(int event_type, const EmscriptenKeyboardEvent *e)
{
  if (window_ == nullptr) {
    return false;
  }
  const GHOST_TKey key = web_key_from_code(e->code);
  if (key == GHOST_kKeyUnknown) {
    return false;
  }
  const bool down = (event_type == EMSCRIPTEN_EVENT_KEYDOWN);

  switch (key) {
    case GHOST_kKeyLeftShift:
    case GHOST_kKeyRightShift:
      modifiers_.set(GHOST_kModifierKeyLeftShift, down);
      break;
    case GHOST_kKeyLeftControl:
    case GHOST_kKeyRightControl:
      modifiers_.set(GHOST_kModifierKeyLeftControl, down);
      break;
    case GHOST_kKeyLeftAlt:
    case GHOST_kKeyRightAlt:
      modifiers_.set(GHOST_kModifierKeyLeftAlt, down);
      break;
    default:
      break;
  }

  char utf8[6] = {0};
  if (down && e->key[0] != '\0' && e->key[1] == '\0') {
    /* Single-character `key` value = printable glyph. */
    utf8[0] = e->key[0];
  }
  if (getenv("GHOST_LOG_KEYS")) {
    fprintf(stderr,
            "GHOST_KEY %s code='%s' key='%s' ghost=%d utf8='%s'\n",
            down ? "dn" : "up",
            e->code,
            e->key,
            int(key),
            utf8);
    fflush(stderr);
  }
  pushEvent(std::unique_ptr<const GHOST_IEvent>(new GHOST_EventKey(getMilliSeconds(),
                               down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp,
                               window_,
                               key,
                               e->repeat,
                               utf8)));
  /* Let the browser keep F5/F11/devtools shortcuts etc. only when unhandled. */
  return true;
}

bool GHOST_SystemWeb::handleResize()
{
  double w = 0, h = 0;
  if (emscripten_get_element_css_size(WEB_CANVAS, &w, &h) != EMSCRIPTEN_RESULT_SUCCESS ||
      !std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1)
  {
    /* Hidden/minimized canvases retain their last usable drawing buffers. */
    return false;
  }
  const uint32_t width = uint32_t(w), height = uint32_t(h);
  const double ratio = emscripten_get_device_pixel_ratio();
  const int limit = EM_ASM_INT({
    return Module.preinitializedWebGPUDevice?.limits.maxTextureDimension2D || 8192;
  });
  const float scale = float(std::min({std::isfinite(ratio) && ratio > 0 ? ratio : 1.0,
                                     double(limit) / width,
                                     double(limit) / height}));
  const bool dpi_changed = canvas_scale_ != scale;
  const bool size_changed = canvas_w_ != width || canvas_h_ != height;
  canvas_w_ = width;
  canvas_h_ = height;
  canvas_scale_ = scale;
  const int pixel_w = std::max(1, int(width * scale));
  const int pixel_h = std::max(1, int(height * scale));
  int old_w = 0, old_h = 0;
  emscripten_get_canvas_element_size(WEB_CANVAS, &old_w, &old_h);
  if (old_w != pixel_w || old_h != pixel_h) {
    emscripten_set_canvas_element_size(WEB_CANVAS, pixel_w, pixel_h);
  }
  if (!size_changed && !dpi_changed) {
    return false;
  }
  if (window_) {
    window_->resize(canvas_w_, canvas_h_, canvas_scale_);
    if (dpi_changed) {
      pushEvent(std::unique_ptr<const GHOST_IEvent>(
          new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowDPIHintChanged, window_)));
    }
    pushEvent(std::unique_ptr<const GHOST_IEvent>(new GHOST_Event(getMilliSeconds(), GHOST_kEventWindowSize, window_)));
  }
  return true;
}

GHOST_ISystem *GHOST_createWebSystem() { return new GHOST_SystemWeb(); }

/* Presentation notifications cross back to the browser UI thread. Keep the
 * loader contract in GHOST, separate from Blender's editors. */
extern "C" void GHOST_web_first_frame()
{
  MAIN_THREAD_ASYNC_EM_ASM({
    document.documentElement.dataset.blenderReady = 'true';
    document.getElementById('status')?.remove();
    document.getElementById('canvas')?.focus();
  });
}
