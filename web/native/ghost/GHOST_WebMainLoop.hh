/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <emscripten.h>
#include "GHOST_WebJobs.hh"
namespace blender {
namespace {
bContext *web_frame_context;
}

extern "C" EMSCRIPTEN_KEEPALIVE void blender_web_run_frame()
{
  if (GHOST_web_gpu_job_active()) return;
  auto *C = web_frame_context;
  wm_window_events_process(C);
  wm_event_do_handlers(C);
  wm_event_do_notifiers(C);
  wm_draw_update(C);
  PRF_frame_mark;
}

[[noreturn]] inline void GHOST_web_main_loop(bContext *context)
{
  web_frame_context = context;
  emscripten_set_main_loop_arg([](void *) {
    if (!emscripten_has_asyncify()) {
      blender_web_run_frame();
      return;
    }
    // Selection readbacks can wait for WebGPU from ordinary input handlers.
    // Enter each frame through JSPI, and serialize frames while it suspends.
    // Defer until this C callback has unwound before entering Wasm again.
    EM_ASM({
      if (Module['blenderWebFramePending']) return;
      Module['blenderWebFramePending'] = true;
      queueMicrotask(() => {
        Module['blenderWebFramePromise'] = Module['ccall']('blender_web_run_frame', null, [], [], {async:true})
          .catch(error => { console.error('Blender UI frame failed', error); throw error; })
          .finally(() => { Module['blenderWebFramePending'] = false; });
      });
    });
  }, context, 0, true);
  __builtin_unreachable();
}
}
