/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <emscripten.h>
namespace blender {
[[noreturn]] inline void GHOST_web_main_loop(bContext *context)
{
  emscripten_set_main_loop_arg([](void *data) {
    auto *C = static_cast<bContext *>(data);
    wm_window_events_process(C);
    wm_event_do_handlers(C);
    wm_event_do_notifiers(C);
    wm_draw_update(C);
    PRF_frame_mark;
  }, context, 0, true);
  __builtin_unreachable();
}
}
