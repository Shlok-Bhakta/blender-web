/* SPDX-License-Identifier: GPL-2.0-or-later */
/** GHOST/WebGPU integration test. This does not start Blender's editor or Workbench. */
#include "GHOST_ISystem.hh"
#include "GHOST_IEventConsumer.hh"
#include "GHOST_IWindow.hh"
#include "CLG_log.h"
#include "BLI_math_base.hh"

#include <emscripten.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#ifdef __EMSCRIPTEN_PTHREADS__
#  include <condition_variable>
#  include <mutex>
#  include <thread>
#endif

static GHOST_ISystem *system_;
static GHOST_IWindow *window_;
static float yaw = 0.65f, pitch = 0.5f, distance_ = 6.0f;
static bool dragging = false;
static int last_x, last_y;
static uint32_t event_count = 0, resize_count = 0, frames = 0;
static std::atomic<int> completed{0};
static std::atomic<uint64_t> checksum{0};
static int workers = 0;
static bool job_pending = false;

EM_JS(void, web_draw, (const float *matrix), { Module.webHost.draw(HEAPF32.subarray(matrix >> 2, (matrix >> 2) + 16)); });
EM_JS(void, web_request_frame, (), { Module.webHost.invalidate(); });
EM_JS(void, web_diagnostics, (int events, int resizes, int frame_count), {
  Module.webHost.diagnostics = {events, resizes, frames: frame_count, backend: 'WEB'};
});
EM_JS(int, web_worker_count, (), { return Module.webWorkerCount || 0; });
EM_JS(void, web_job_complete, (double result, int count), {
  Module.webHost.jobComplete(result, count);
});

static constexpr uint32_t job_size = 1 << 20;
static uint64_t run_chunk(int index, int count)
{
  uint64_t result = 0;
  const uint32_t start = uint64_t(job_size) * index / count;
  const uint32_t end = uint64_t(job_size) * (index + 1) / count;
  for (uint32_t i = start; i < end; ++i) { result += (i * uint64_t(i)) % 1009; }
  return result;
}

#ifdef __EMSCRIPTEN_PTHREADS__
/* Persistent workers sleep while idle. No spin loops and no browser-thread joins. */
static std::mutex job_mutex;
static std::condition_variable job_cv;
static int generation = 0;
static std::vector<std::thread> threads;
static void worker_main(int index)
{
  int seen = 0;
  while (true) {
    std::unique_lock lock(job_mutex);
    job_cv.wait(lock, [&] { return generation != seen; });
    seen = generation;
    lock.unlock();
    checksum.fetch_add(run_chunk(index, workers), std::memory_order_relaxed);
    completed.fetch_add(1, std::memory_order_release);
  }
}
#endif

class TestEvents final : public GHOST_IEventConsumer {
  bool processEvent(const GHOST_IEvent *event) override
  {
    ++event_count;
    switch (event->getType()) {
      case GHOST_kEventWindowSize: ++resize_count; break;
      case GHOST_kEventButtonDown: dragging = true; break;
      case GHOST_kEventButtonUp:
      case GHOST_kEventWindowDeactivate: dragging = false; break;
      case GHOST_kEventCursorMove: {
        const auto *data = static_cast<const GHOST_TEventCursorData *>(event->getData());
        if (dragging) {
          yaw += (data->x - last_x) * 0.006f / window_->getNativePixelSize();
          pitch = blender::math::clamp(pitch + (data->y - last_y) * 0.006f /
                                               window_->getNativePixelSize(), -1.5f, 1.5f);
        }
        last_x = data->x;
        last_y = data->y;
        break;
      }
      case GHOST_kEventWheel: {
        const auto *data = static_cast<const GHOST_TEventWheelData *>(event->getData());
        distance_ = blender::math::clamp(distance_ - data->value * 0.25f, 3.0f, 15.0f);
        break;
      }
      case GHOST_kEventKeyDown: {
        const auto *data = static_cast<const GHOST_TEventKeyData *>(event->getData());
        if (data->key == GHOST_kKeyHome) { yaw = 0.65f; pitch = 0.5f; distance_ = 6.0f; }
        break;
      }
      default: break;
    }
    return true;
  }
};

static void multiply(const float a[16], const float b[16], float out[16])
{
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      out[column * 4 + row] = 0;
      for (int k = 0; k < 4; ++k) { out[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k]; }
    }
  }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void web_frame()
{
  system_->processEvents(false);
  system_->dispatchEvents();
  GHOST_Rect bounds;
  window_->getClientBounds(bounds);
  const float aspect = float(bounds.getWidth()) / float(bounds.getHeight());
  const float cy = std::cos(yaw), sy = std::sin(yaw), cx = std::cos(pitch), sx = std::sin(pitch);
  const float rotate_y[16] = {cy,0,-sy,0, 0,1,0,0, sy,0,cy,0, 0,0,0,1};
  const float rotate_x[16] = {1,0,0,0, 0,cx,sx,0, 0,-sx,cx,0, 0,0,0,1};
  float view[16], mvp[16];
  multiply(rotate_x, rotate_y, view);
  view[14] = -distance_;
  /* WebGPU uses a 0..1 depth range. */
  constexpr float near_ = 0.1f, far_ = 100.0f, f = 2.414213562f;
  const float projection[16] = {f/aspect,0,0,0, 0,f,0,0,
      0,0,far_/(near_-far_),-1, 0,0,far_*near_/(near_-far_),0};
  multiply(projection, view, mvp);
  web_draw(mvp);
  web_diagnostics(event_count, resize_count, ++frames);
  if (job_pending) {
    if (completed.load(std::memory_order_acquire) == (workers ? workers : 1)) {
      job_pending = false;
      web_job_complete(double(checksum.load()), workers);
    }
    else { web_request_frame(); }
  }
}
EMSCRIPTEN_KEEPALIVE void web_orbit(float new_yaw, float new_pitch, float new_distance)
{
  yaw = new_yaw;
  pitch = blender::math::clamp(new_pitch, -1.5f, 1.5f);
  distance_ = blender::math::clamp(new_distance, 3.0f, 15.0f);
  web_request_frame();
}
EMSCRIPTEN_KEEPALIVE float web_yaw() { return yaw; }
EMSCRIPTEN_KEEPALIVE float web_pitch() { return pitch; }
EMSCRIPTEN_KEEPALIVE float web_distance() { return distance_; }
EMSCRIPTEN_KEEPALIVE void web_run_parallel_test()
{
  if (job_pending) { return; }
  checksum.store(0);
  completed.store(0);
  job_pending = true;
#ifdef __EMSCRIPTEN_PTHREADS__
  { std::lock_guard lock(job_mutex); ++generation; }
  job_cv.notify_all();
#else
  checksum.store(run_chunk(0, 1));
  completed.store(1, std::memory_order_release);
#endif
  web_request_frame();
}
}

int main()
{
  CLG_init();
  if (GHOST_ISystem::createSystem(true) != GHOST_kSuccess) { return 1; }
  system_ = GHOST_ISystem::getSystem();
  GHOST_GPUSettings settings{};
  settings.context_type = GHOST_kDrawingContextTypeNone;
  window_ = system_->createWindow("Blender Web | GHOST platform test", 0, 0, 960, 640,
                                  GHOST_kWindowStateNormal, settings, false, false, nullptr);
  if (!window_) { return 2; }
  system_->addEventConsumer(new TestEvents());
#ifdef __EMSCRIPTEN_PTHREADS__
  workers = web_worker_count();
  for (int i = 0; i < workers; ++i) { threads.emplace_back(worker_main, i); }
#endif
  web_request_frame();
  /* Keep callbacks and persistent sleeping workers alive. */
  emscripten_exit_with_live_runtime();
}
