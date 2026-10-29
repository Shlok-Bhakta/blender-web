/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_WebFFI.hh"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <ffi.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

/* A Wasm function table belongs to one JS worker, while libffi's CIF and
 * closure memory are shared. Install the original libffi typed trampoline in
 * each participating worker. The runtime thread owns table-slot allocation.
 * Sleeping workers reconcile before returning from their existing futex wait;
 * active workers receive a coalesced proxy-queue notice. No polling is added.
 * Callers must retain callbacks until all foreign users have stopped calling,
 * as with native libffi. Freeing a callback from inside itself is unsupported.
 */
namespace {
using Callback = void (*)(ffi_cif *, void *, void **, void *);
struct Closure {
  ffi_closure *memory;
  void *code;
  ffi_cif *cif;
  Callback function;
  void *user;
  std::atomic<unsigned> active{0};
};
struct Worker {
  pthread_t id;
  unsigned epoch = 0;
  bool sleeping = false, alive = true, notice = false;
};
struct Registry {
  std::mutex mutex;
  std::vector<std::shared_ptr<Worker>> workers;
  std::vector<Closure *> closures;
  unsigned epoch = 0;
  std::atomic<unsigned> progress{0};
  em_proxying_queue *queue = em_proxying_queue_create();
};
// Process lifetime avoids static-constructor and thread-local destructor order
// dependencies. Construction also covers pthreads started before Python.
Registry &registry()
{
  static Registry *instance = new Registry;
  return *instance;
}
thread_local std::shared_ptr<Worker> self;
thread_local std::vector<uintptr_t> installed;
thread_local std::vector<Closure *> invocations;
thread_local unsigned local_epoch = UINT_MAX, guard = 0, queue_depth = 0;

void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "GHOST Web libffi: %s\n", message);
    std::abort();
  }
}
struct Guard {
  Guard() { ++guard; }
  ~Guard() { --guard; }
};
extern "C" void *__real_ffi_closure_alloc(size_t, void **);
extern "C" void __real_ffi_closure_free(void *);
extern "C" ffi_status __real_ffi_prep_closure_loc(
    ffi_closure *, ffi_cif *, Callback, void *, void *);
extern "C" ffi_status ffi_web_install_closure(ffi_closure *, ffi_cif *, void *, void *, void *);
extern "C" int __real_emscripten_futex_wait(volatile void *, uint32_t, double);
extern "C" int __real_pthread_create(
    pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
EM_JS_DEPS(ghost_ffi, "$setWasmTableEntry");
EM_JS(void, ensure_table, (uintptr_t index), {
  if (wasmTable.length <= index) wasmTable.grow(index + 1 - wasmTable.length);
});
EM_JS(void, clear_table, (uintptr_t index), { setWasmTableEntry(index, null); });

void changed()
{
  auto &state = registry();
  state.progress.fetch_add(1);
  emscripten_futex_wake(&state.progress, INT_MAX);
}
void dispatch(ffi_cif *cif, void *result, void **arguments, void *data)
{
  auto *closure = static_cast<Closure *>(data);
  closure->active.fetch_add(1);
  invocations.push_back(closure);
  closure->function(cif, result, arguments, closure->user);
  require(!invocations.empty() && invocations.back() == closure, "Unbalanced callback stack");
  invocations.pop_back();
  closure->active.fetch_sub(1);
  changed();
}
// The registry mutex keeps CIFs and closure descriptors alive during installation.
void apply_locked()
{
  auto &state = registry();
  if (local_epoch == state.epoch) return;
  for (auto index : installed) clear_table(index);
  installed.clear();
  for (auto *closure : state.closures) {
    ensure_table(reinterpret_cast<uintptr_t>(closure->code));
    require(ffi_web_install_closure(closure->memory, closure->cif,
                reinterpret_cast<void *>(dispatch), closure, closure->code) == FFI_OK,
            "Could not install a worker callback");
    installed.push_back(reinterpret_cast<uintptr_t>(closure->code));
  }
  local_epoch = state.epoch;
  if (self) self->epoch = state.epoch;
  changed();
}
void reconcile(void *)
{
  Guard protect;
  std::lock_guard lock(registry().mutex);
  apply_locked();
}
void notice(void *)
{
  Guard protect;
  std::lock_guard lock(registry().mutex);
  if (self) self->notice = false;
  apply_locked();
}
void drain()
{
  if (guard || queue_depth) return;
  ++queue_depth;
  emscripten_proxy_execute_queue(registry().queue);
  --queue_depth;
}
void sync(pthread_t thread, void (*function)(void *), void *data)
{
  if (pthread_equal(thread, pthread_self())) function(data);
  else {
    ++queue_depth;
    bool ok = emscripten_proxy_sync(registry().queue, thread, function, data);
    --queue_depth;
    require(ok, "Runtime proxy target exited");
    drain();
  }
}
void register_worker()
{
  if (self) return;
  Guard protect;
  auto &state = registry();
  require(state.queue, "Could not create the callback proxy queue");
  std::lock_guard lock(state.mutex);
  self = std::make_shared<Worker>();
  self->id = pthread_self();
  state.workers.push_back(self);
  apply_locked();
}
void unregister_worker(void *)
{
  Guard protect;
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  // pthread_exit can abandon a callback before dispatch returns. Release every
  // active count so a different thread can finish retiring that closure.
  for (auto *closure : invocations) closure->active.fetch_sub(1);
  invocations.clear();
  for (auto index : installed) clear_table(index);
  installed.clear();
  local_epoch = UINT_MAX;
  self->alive = false;
  auto entry = std::find(state.workers.begin(), state.workers.end(), self);
  require(entry != state.workers.end(), "Unregistered worker exited");
  state.workers.erase(entry);
  self.reset();
  changed();
}
struct Start {
  void *(*function)(void *);
  void *data;
};
void *start_worker(void *argument)
{
  const auto start = *static_cast<Start *>(argument);
  delete static_cast<Start *>(argument);
  register_worker();
  void *result;
  pthread_cleanup_push(unregister_worker, nullptr);
  result = start.function(start.data);
  pthread_cleanup_pop(1);
  return result;
}
void publish(unsigned requested, const std::vector<std::shared_ptr<Worker>> &targets);
}  // namespace

extern "C" void GHOST_web_initialize_ffi()
{
  register_worker();
}
extern "C" int __wrap_pthread_create(
    pthread_t *thread, const pthread_attr_t *attributes, void *(*function)(void *), void *data)
{
  // Always register new workers, including those created before Python starts.
  auto *start = new (std::nothrow) Start{function, data};
  if (!start) return EAGAIN;
  int result = __real_pthread_create(thread, attributes, start_worker, start);
  if (result) delete start;
  return result;
}
extern "C" int __wrap_emscripten_futex_wait(volatile void *address, uint32_t value, double timeout)
{
  // This path must not initialize the registry: malloc and C++ static guards
  // themselves may wait on a futex during registry construction.
  if (!self || guard || queue_depth) return __real_emscripten_futex_wait(address, value, timeout);
  const double deadline = (timeout < 0 || !std::isfinite(timeout)) ?
                              INFINITY : emscripten_get_now() + timeout;
  drain();
  {
    Guard protect;
    std::lock_guard lock(registry().mutex);
    apply_locked();
    self->sleeping = true;
    changed();
  }
  const double remaining = std::isfinite(deadline) ?
                               std::max(0., deadline - emscripten_get_now()) : INFINITY;
  int result = __real_emscripten_futex_wait(address, value, remaining);
  {
    Guard protect;
    std::lock_guard lock(registry().mutex);
    apply_locked();
    self->sleeping = false;
    changed();
  }
  drain();
  return result;
}
namespace {
void publish(unsigned requested, const std::vector<std::shared_ptr<Worker>> &targets)
{
  auto &state = registry();
  reconcile(nullptr);
  sync(emscripten_main_runtime_thread_id(), reconcile, nullptr);
  for (const auto &worker : targets) {
    if (pthread_equal(worker->id, pthread_self())) continue;
    bool send = false;
    {
      Guard protect;
      std::lock_guard lock(state.mutex);
      if (worker->alive && !worker->sleeping && worker->epoch < requested && !worker->notice) {
        worker->notice = true;
        send = true;
      }
    }
    if (send) {
      ++queue_depth;
      int queued = emscripten_proxy_async(state.queue, worker->id, notice, nullptr);
      --queue_depth;
      if (!queued) {
        Guard protect;
        std::lock_guard lock(state.mutex);
        require(!worker->alive, "Could not notify an active callback worker");
      }
    }
  }
  for (;;) {
    bool done = true;
    unsigned observed;
    {
      Guard protect;
      std::lock_guard lock(state.mutex);
      for (const auto &worker : targets) {
        if (worker->alive && !worker->sleeping && worker->epoch < requested) done = false;
      }
      observed = state.progress.load();
    }
    if (done) break;
    __wrap_emscripten_futex_wait(&state.progress, observed, INFINITY);
  }
}
struct Allocation {
  size_t size;
  void **code;
  void *memory;
};
void allocate(void *data)
{
  auto *request = static_cast<Allocation *>(data);
  request->memory = __real_ffi_closure_alloc(request->size, request->code);
}
void release(void *memory)
{
  __real_ffi_closure_free(memory);
}
}  // namespace
extern "C" void *__wrap_ffi_closure_alloc(size_t size, void **code)
{
  Allocation request{size, code, nullptr};
  sync(emscripten_main_runtime_thread_id(), allocate, &request);
  return request.memory;
}
extern "C" ffi_status __wrap_ffi_prep_closure_loc(
    ffi_closure *memory, ffi_cif *cif, Callback function, void *user, void *code)
{
  auto &state = registry();
  auto *closure = new Closure{memory, code, cif, function, user};
  unsigned requested = 0;
  std::vector<std::shared_ptr<Worker>> targets;
  ffi_status status;
  {
    Guard protect;
    std::lock_guard lock(state.mutex);
    require(std::none_of(state.closures.begin(), state.closures.end(),
                        [&](auto *entry) { return entry->memory == memory; }),
            "Closure was prepared twice");
    ensure_table(reinterpret_cast<uintptr_t>(code));
    status = __real_ffi_prep_closure_loc(memory, cif, dispatch, closure, code);
    if (status == FFI_OK) {
      state.closures.push_back(closure);
      requested = ++state.epoch;
      targets = state.workers;
    }
  }
  if (status != FFI_OK) {
    delete closure;
    return status;
  }
  publish(requested, targets);
  return status;
}
extern "C" void __wrap_ffi_closure_free(void *memory)
{
  auto &state = registry();
  Closure *closure = nullptr;
  unsigned requested = 0;
  std::vector<std::shared_ptr<Worker>> targets;
  {
    Guard protect;
    std::lock_guard lock(state.mutex);
    auto entry = std::find_if(state.closures.begin(), state.closures.end(),
                             [&](auto *item) { return item->memory == memory; });
    if (entry != state.closures.end()) {
      closure = *entry;
      require(std::find(invocations.begin(), invocations.end(), closure) == invocations.end(),
              "A callback cannot free itself");
      state.closures.erase(entry);
      requested = ++state.epoch;
      targets = state.workers;
    }
  }
  if (closure) {
    publish(requested, targets);
    while (closure->active.load()) {
      unsigned observed = state.progress.load();
      if (closure->active.load()) __wrap_emscripten_futex_wait(&state.progress, observed, INFINITY);
    }
    delete closure;
  }
  sync(emscripten_main_runtime_thread_id(), release, memory);
}
