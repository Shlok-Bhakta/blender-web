/* SPDX-License-Identifier: GPL-2.0-or-later */
/** GHOST/WebGPU integration test. This does not start Blender's editor or Workbench. */
#include "GHOST_ISystem.hh"
#include "GHOST_IEventConsumer.hh"
#include "GHOST_IWindow.hh"
#include "CLG_log.h"
#include "BLI_math_base.hh"

#include <emscripten.h>
#include <atomic>
#include <algorithm>
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
static float transform_[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};
static float target_[3] = {0, 0, 0};
static bool visible_ = true, selected_ = true, grid_ = true, edges_ = true, orthographic_ = false;
static bool dragging = false;
static int last_x, last_y;
static uint32_t event_count = 0, resize_count = 0, frames = 0;
static std::atomic<int> completed{0};
static std::atomic<uint64_t> checksum{0};
static int workers = 0;
static bool job_pending = false;

EM_JS(void, web_draw, (const float *matrix, const float *view, const float *normal,
                      int visible, int selected, int grid, int edges), {
  Module.webHost.draw(HEAPF32.subarray(matrix >> 2, (matrix >> 2) + 16),
                     HEAPF32.subarray(view >> 2, (view >> 2) + 16),
                     HEAPF32.subarray(normal >> 2, (normal >> 2) + 16),
                     {visible: !!visible, selected: !!selected, grid: !!grid, edges: !!edges});
});
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
                                               window_->getNativePixelSize(), -1.5707963f, 1.5707963f);
        }
        last_x = data->x;
        last_y = data->y;
        break;
      }
      case GHOST_kEventWheel: {
        const auto *data = static_cast<const GHOST_TEventWheelData *>(event->getData());
        distance_ = blender::math::clamp(distance_ * std::pow(0.9f, float(data->value)), 0.2f, 500.0f);
        break;
      }
      case GHOST_kEventKeyDown: {
        const auto *data = static_cast<const GHOST_TEventKeyData *>(event->getData());
        if (data->key == GHOST_kKeyHome) {
          GHOST_Rect bounds;
          window_->getClientBounds(bounds);
          yaw = 0.65f; pitch = 0.5f;
          distance_ = std::max(6.0f, 4.0f * bounds.getHeight() / std::max(1, bounds.getWidth()));
          target_[0] = target_[1] = target_[2] = 0;
        }
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
  const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
  /* Blender coordinates are Z-up. The analytical basis also handles the top view. */
  float view[16] = {cy,-sy*sp,sy*cp,0, sy,cy*sp,-cy*cp,0, 0,cp,sp,0, 0,0,-distance_,1};
  for (int row = 0; row < 3; ++row) {
    view[12 + row] -= view[row] * target_[0] + view[4 + row] * target_[1] + view[8 + row] * target_[2];
  }
  float mvp[16], view_projection[16];
  /* WebGPU uses a 0..1 depth range. */
  constexpr float near_ = 0.1f, far_ = 5000.0f, f = 2.414213562f;
  float projection[16] = {f/aspect,0,0,0, 0,f,0,0,
      0,0,far_/(near_-far_),-1, 0,0,far_*near_/(near_-far_),0};
  if (orthographic_) {
    const float height = distance_ / f;
    const float ortho[16] = {1/(height*aspect),0,0,0, 0,1/height,0,0,
        0,0,1/(near_-far_),0, 0,0,near_/(near_-far_),1};
    std::copy(ortho, ortho + 16, projection);
  }
  multiply(projection, view, view_projection);
  constexpr float radians = 0.01745329252f;
  const float rx = transform_[3] * radians, ry = transform_[4] * radians, rz = transform_[5] * radians;
  const float x[16] = {1,0,0,0, 0,std::cos(rx),std::sin(rx),0, 0,-std::sin(rx),std::cos(rx),0, 0,0,0,1};
  const float y[16] = {std::cos(ry),0,-std::sin(ry),0, 0,1,0,0, std::sin(ry),0,std::cos(ry),0, 0,0,0,1};
  const float z[16] = {std::cos(rz),std::sin(rz),0,0, -std::sin(rz),std::cos(rz),0,0, 0,0,1,0, 0,0,0,1};
  float rotation[16], temp[16], model[16], normal[16];
  multiply(y, x, temp);
  multiply(z, temp, rotation);
  std::copy(rotation, rotation + 16, model);
  std::copy(rotation, rotation + 16, normal);
  for (int axis = 0; axis < 3; ++axis) {
    for (int row = 0; row < 3; ++row) {
      model[axis * 4 + row] *= transform_[6 + axis];
      normal[axis * 4 + row] *= std::abs(transform_[6 + axis]) > 1e-6f ? 1/transform_[6 + axis] : 0;
    }
    model[12 + axis] = transform_[axis];
  }
  multiply(view_projection, model, mvp);
  web_draw(mvp, view_projection, normal, visible_, selected_, grid_, edges_);
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
  if (!std::isfinite(new_yaw) || !std::isfinite(new_pitch) || !std::isfinite(new_distance)) { return; }
  yaw = new_yaw;
  pitch = blender::math::clamp(new_pitch, -1.5707963f, 1.5707963f);
  distance_ = blender::math::clamp(new_distance, 0.2f, 500.0f);
  web_request_frame();
}
EMSCRIPTEN_KEEPALIVE float web_yaw() { return yaw; }
EMSCRIPTEN_KEEPALIVE float web_pitch() { return pitch; }
EMSCRIPTEN_KEEPALIVE float web_distance() { return distance_; }
EMSCRIPTEN_KEEPALIVE float web_transform(int index) { return index >= 0 && index < 9 ? transform_[index] : 0; }
EMSCRIPTEN_KEEPALIVE int web_set_transform(int index, float value)
{
  if (index < 0 || index >= 9 || !std::isfinite(value) || std::abs(value) > 10000) { return 0; }
  transform_[index] = value;
  web_request_frame();
  return 1;
}
EMSCRIPTEN_KEEPALIVE float web_target(int index) { return index >= 0 && index < 3 ? target_[index] : 0; }
EMSCRIPTEN_KEEPALIVE void web_set_target(float x, float y, float z)
{
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) { return; }
  target_[0] = x; target_[1] = y; target_[2] = z;
  web_request_frame();
}
EMSCRIPTEN_KEEPALIVE void web_display(int visible, int selected, int grid, int edges, int orthographic)
{
  visible_ = visible; selected_ = selected; grid_ = grid; edges_ = edges; orthographic_ = orthographic;
  web_request_frame();
}
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
  window_ = system_->createWindow("Blender Web | Preview", 0, 0, 960, 640,
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
