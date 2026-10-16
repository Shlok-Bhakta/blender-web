/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_SystemWeb.hh"
#include "GHOST_WindowWeb.hh"

#include "intern/GHOST_Event.hh"
#include "intern/GHOST_EventButton.hh"
#include "intern/GHOST_EventCursor.hh"
#include "intern/GHOST_EventKey.hh"
#include "intern/GHOST_EventManager.hh"
#include "intern/GHOST_EventWheel.hh"
#include "intern/GHOST_WindowManager.hh"

#include <emscripten.h>
#include <cstring>
#include <cstdlib>

/* All browser event callbacks run on the browser thread, which owns GHOST's queue.
 * Compute workers must proxy events here rather than mutate it concurrently. */
static GHOST_SystemWeb *web_system = nullptr;

EM_JS(void, web_attach, (), { Module.webHost.attach(Module); });
EM_JS(int, web_width, (), { return Module.canvas.width; });
EM_JS(int, web_height, (), { return Module.canvas.height; });

GHOST_ISystem *GHOST_createWebSystem()
{
  return new GHOST_SystemWeb();
}
GHOST_SystemWeb::~GHOST_SystemWeb()
{
  /* The base window manager does not own windows. Dispose while JS is still live. */
  while (window_manager_ && !window_manager_->getWindows().empty()) {
    disposeWindow(window_manager_->getWindows().back());
  }
  web_system = nullptr;
}
bool GHOST_SystemWeb::processEvents(bool /*wait_for_event*/)
{
  /* Never block the browser event loop. JS schedules frames only when needed. */
  return event_manager_ && event_manager_->getNumEvents() != 0;
}
uint64_t GHOST_SystemWeb::getMilliSeconds() const { return uint64_t(emscripten_get_now()); }
GHOST_TCapabilityFlag GHOST_SystemWeb::getCapabilities() const
{
  /* No warping, OS window placement, native clipboard or IME implementation yet. */
  return GHOST_TCapabilityFlag(0);
}
void GHOST_SystemWeb::getMainDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  width = uint32_t(web_width());
  height = uint32_t(web_height());
}
void GHOST_SystemWeb::getAllDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  getMainDisplayDimensions(width, height);
}
GHOST_TSuccess GHOST_SystemWeb::getModifierKeys(GHOST_ModifierKeys &keys) const
{
  keys = modifiers_;
  return GHOST_kSuccess;
}
GHOST_TSuccess GHOST_SystemWeb::getButtons(GHOST_Buttons &buttons) const
{
  buttons = buttons_;
  return GHOST_kSuccess;
}
GHOST_TSuccess GHOST_SystemWeb::getCursorPosition(int32_t &x, int32_t &y) const
{
  x = cursor_x_;
  y = cursor_y_;
  return GHOST_kSuccess;
}
GHOST_IWindow *GHOST_SystemWeb::getWindowUnderCursor(int32_t x, int32_t y)
{
  return GHOST_System::getWindowUnderCursor(x, y);
}
GHOST_IWindow *GHOST_SystemWeb::createWindow(const char *title, int32_t, int32_t,
                                           uint32_t width, uint32_t height,
                                           GHOST_TWindowState state,
                                           GHOST_GPUSettings gpu_settings,
                                           bool, bool, const GHOST_IWindow *)
{
  /* A real GPUBackend must introduce its context type before editor integration.
   * The platform test owns WebGPU directly and requests a None context. */
  if (!window_manager_->getWindows().empty() || state != GHOST_kWindowStateNormal ||
      gpu_settings.context_type != GHOST_kDrawingContextTypeNone) {
    return nullptr;
  }
  auto *window = new GHOST_WindowWeb(title, width, height);
  window_manager_->addWindow(window);
  window_manager_->setActiveWindow(window);
  web_system = this;
  web_attach();
  return window;
}

static GHOST_TKey key_from_code(const char *code)
{
  if (std::strncmp(code, "Key", 3) == 0 && std::strlen(code) == 4) {
    return GHOST_TKey(code[3]);
  }
  if (std::strncmp(code, "Digit", 5) == 0 && std::strlen(code) == 6) {
    return GHOST_TKey(code[5]);
  }
  struct Key { const char *name; GHOST_TKey key; };
  static constexpr Key keys[] = {
      {"Escape", GHOST_kKeyEsc}, {"Space", GHOST_kKeySpace}, {"Enter", GHOST_kKeyEnter},
      {"Backspace", GHOST_kKeyBackSpace}, {"Tab", GHOST_kKeyTab},
      {"ShiftLeft", GHOST_kKeyLeftShift}, {"ShiftRight", GHOST_kKeyRightShift},
      {"ControlLeft", GHOST_kKeyLeftControl}, {"ControlRight", GHOST_kKeyRightControl},
      {"AltLeft", GHOST_kKeyLeftAlt}, {"AltRight", GHOST_kKeyRightAlt},
      {"MetaLeft", GHOST_kKeyLeftOS}, {"MetaRight", GHOST_kKeyRightOS},
      {"ArrowLeft", GHOST_kKeyLeftArrow}, {"ArrowRight", GHOST_kKeyRightArrow},
      {"ArrowUp", GHOST_kKeyUpArrow}, {"ArrowDown", GHOST_kKeyDownArrow},
      {"Delete", GHOST_kKeyDelete}, {"Home", GHOST_kKeyHome}, {"End", GHOST_kKeyEnd},
      {"PageUp", GHOST_kKeyUpPage}, {"PageDown", GHOST_kKeyDownPage},
  };
  for (const auto &item : keys) {
    if (std::strcmp(code, item.name) == 0) { return item.key; }
  }
  if (code[0] == 'F') {
    const int number = std::atoi(code + 1);
    if (number >= 1 && number <= 24) { return GHOST_TKey(GHOST_kKeyF1 + number - 1); }
  }
  return GHOST_kKeyUnknown;
}

void GHOST_SystemWeb::pointer(int kind, int x, int y, int button)
{
  GHOST_IWindow *window = window_manager_->getActiveWindow();
  if (!window) { return; }
  cursor_x_ = x;
  cursor_y_ = y;
  GHOST_TabletData tablet{};
  if (kind == 0) {
    pushEvent(std::make_unique<GHOST_EventCursor>(getMilliSeconds(), GHOST_kEventCursorMove,
                                                 window, x, y, tablet));
  }
  else if (button >= 0 && button <= 2) {
    const GHOST_TButton buttons[] = {GHOST_kButtonMaskLeft, GHOST_kButtonMaskMiddle,
                                    GHOST_kButtonMaskRight};
    buttons_.set(buttons[button], kind == 1);
    pushEvent(std::make_unique<GHOST_EventButton>(getMilliSeconds(),
        kind == 1 ? GHOST_kEventButtonDown : GHOST_kEventButtonUp, window, buttons[button], tablet));
  }
}
void GHOST_SystemWeb::key(const char *code, const char *text, bool down, bool repeat)
{
  GHOST_IWindow *window = window_manager_->getActiveWindow();
  if (!window) { return; }
  const GHOST_TKey key = key_from_code(code);
  const GHOST_TKey modifier_keys[] = {GHOST_kKeyLeftShift, GHOST_kKeyRightShift,
      GHOST_kKeyLeftAlt, GHOST_kKeyRightAlt, GHOST_kKeyLeftControl, GHOST_kKeyRightControl,
      GHOST_kKeyLeftOS, GHOST_kKeyRightOS};
  for (int i = 0; i < 8; ++i) {
    if (key == modifier_keys[i]) { modifiers_.set(GHOST_TModifierKey(i), down); }
  }
  char utf8[6] = {};
  if (down && text && std::strlen(text) < sizeof(utf8)) { std::strcpy(utf8, text); }
  pushEvent(std::make_unique<GHOST_EventKey>(getMilliSeconds(),
      down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp, window, key, repeat, utf8));
}
void GHOST_SystemWeb::blur()
{
  modifiers_.clear();
  buttons_.clear();
  if (auto *window = window_manager_->getActiveWindow()) {
    pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowDeactivate, window));
  }
}
void GHOST_SystemWeb::resize(uint32_t width, uint32_t height, float scale)
{
  if (auto *window = static_cast<GHOST_WindowWeb *>(window_manager_->getActiveWindow())) {
    window->resizeFromBrowser(width, height, scale);
    pushEvent(std::make_unique<GHOST_Event>(getMilliSeconds(), GHOST_kEventWindowSize, window));
  }
}
void GHOST_SystemWeb::wheel(int axis, int ticks)
{
  if (auto *window = window_manager_->getActiveWindow()) {
    pushEvent(std::make_unique<GHOST_EventWheel>(getMilliSeconds(), window,
        axis == 0 ? GHOST_kEventWheelAxisVertical : GHOST_kEventWheelAxisHorizontal, ticks));
  }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void web_pointer(int kind, int x, int y, int button)
{
  if (web_system) { web_system->pointer(kind, x, y, button); }
}
EMSCRIPTEN_KEEPALIVE void web_key(const char *code, const char *text, int down, int repeat)
{
  if (web_system) { web_system->key(code, text, down, repeat); }
}
EMSCRIPTEN_KEEPALIVE void web_blur() { if (web_system) { web_system->blur(); } }
EMSCRIPTEN_KEEPALIVE void web_resize(int width, int height, float scale)
{
  if (web_system && width > 0 && height > 0) { web_system->resize(width, height, scale); }
}
EMSCRIPTEN_KEEPALIVE void web_wheel(int axis, int ticks)
{
  if (web_system) { web_system->wheel(axis, ticks); }
}
}
