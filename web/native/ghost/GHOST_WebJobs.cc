/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_WebJobs.hh"
#include <emscripten.h>
#include <cstdio>
#include <deque>

namespace {
struct Job { void *(*run)(void *); void *data; };
std::deque<Job> jobs;
bool active = false;
}
bool GHOST_web_gpu_job_active() { return active; }
extern "C" EMSCRIPTEN_KEEPALIVE void blender_web_run_gpu_jobs()
{
  if (active || jobs.empty()) return;
  /* WebGPU handles belong to the creating JS worker. Keep GPU orchestration
   * there while Blender's oneTBB tasks continue using the other CPU cores.
   * JSPI permits GPU waits without blocking that worker's JS event loop. */
  active = true;
  emscripten_pause_main_loop();
  while (!jobs.empty()) {
    const Job job = jobs.front();
    jobs.pop_front();
    fprintf(stderr, "WEB_GPU_JOB begin\n");
    job.run(job.data);
    fprintf(stderr, "WEB_GPU_JOB complete\n");
  }
  active = false;
  emscripten_resume_main_loop();
}
bool GHOST_web_schedule_gpu_job(bool uses_gpu, void *(*run)(void *), void *job, bool *ready)
{
  if (!uses_gpu) return false;
  if (!emscripten_has_asyncify()) {
    *ready = true;
    EM_ASM({ alert('Rendering is not available in this browser yet. Use current desktop Chrome to render.'); });
    return true;
  }
  jobs.push_back({run, job});
  EM_ASM({
    setTimeout(() => Module['ccall']('blender_web_run_gpu_jobs', null, [], [], {async:true})
      .catch(error => console.error('Blender GPU job failed', error)), 0);
  });
  return true;
}
