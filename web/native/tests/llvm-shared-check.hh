// SPDX-License-Identifier: GPL-2.0-or-later
#include <emscripten.h>
#include <dlfcn.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

extern "C" EMSCRIPTEN_KEEPALIVE float host_scale(float value)
{
  return value * 0.5f;
}

struct JitFunctions {
  int (*constant)();
  uint32_t (*mix)(uint32_t);
  uint64_t (*wide)(uint64_t);
  void (*shade)(float *, const float *, unsigned);
  void (*vector4)(float *, const float *);
  uint32_t (*increment)(void *, uint32_t);
};

static void run_jit_functions(const JitFunctions &f, unsigned constant, unsigned seed,
                              std::atomic<uint32_t> &counter,
                              std::vector<std::atomic<unsigned>> &seen,
                              std::atomic<unsigned> &errors)
{
  constexpr unsigned count = 1024;
  alignas(16) float input[count], output[count];
  if (f.constant() != int(constant)) ++errors;
  for (unsigned i = 0; i < count; ++i) {
    uint32_t x = (i * UINT32_C(2246822519)) ^ seed;
    uint32_t mixed = x * UINT32_C(1664525) + UINT32_C(1013904223);
    mixed ^= mixed >> 13;
    if (f.mix(x) != mixed) ++errors;
    uint64_t wide = (uint64_t(x) << 32) | i;
    if (f.wide(wide) != wide * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407)) ++errors;
    input[i] = float(int32_t(x)) / 2097152.0f;
  }
  f.shade(output, input, count);
  for (unsigned i = 0; i < count; ++i)
    if (output[i] != (input[i] * input[i]) * 0.5f + 0.25f) ++errors;
  for (unsigned i = 0; i < count; i += 4) f.vector4(output + i, input + i);
  for (unsigned i = 0; i < count; ++i) {
    if (output[i] != input[i] * 0.5f + 0.25f) ++errors;
    auto old = f.increment(&counter, 1);
    if (old < seen.size()) seen[old].fetch_add(1);
    else ++errors;
  }
}

struct SharedJitState {
  std::mutex mutex;
  std::condition_variable condition;
  unsigned started = 0, phase = 0, completed = 0, constant = 0;
  bool stop = false;
  JitFunctions functions{};
  std::atomic<uint32_t> counter{0};
  std::atomic<unsigned> errors{0};
  std::vector<std::atomic<unsigned>> seen = std::vector<std::atomic<unsigned>>(4096);
  std::vector<std::thread> workers;
};

static void start_jit_workers(SharedJitState &state)
{
  for (unsigned i = 0; i < 4; ++i) state.workers.emplace_back([&, i] {
    unsigned phase = 0;
    std::unique_lock<std::mutex> lock(state.mutex);
    ++state.started;
    state.condition.notify_all();
    while (true) {
      state.condition.wait(lock, [&] { return state.stop || state.phase != phase; });
      if (state.stop) return;
      phase = state.phase;
      auto functions = state.functions;
      auto constant = state.constant;
      lock.unlock();
      run_jit_functions(functions, constant, i + phase * 19, state.counter, state.seen, state.errors);
      lock.lock();
      ++state.completed;
      state.condition.notify_all();
    }
  });
  std::unique_lock<std::mutex> lock(state.mutex);
  state.condition.wait(lock, [&] { return state.started == 4; });
}

static void publish_and_test_jit(SharedJitState &state, const char *file, unsigned constant)
{
  void *handle = dlopen(file, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char *error = dlerror();
    require(false, error ? error : "dlopen generated module");
  }
  JitFunctions functions{
      reinterpret_cast<int (*)()>(dlsym(handle, "folded")),
      reinterpret_cast<uint32_t (*)(uint32_t)>(dlsym(handle, "mix")),
      reinterpret_cast<uint64_t (*)(uint64_t)>(dlsym(handle, "wide")),
      reinterpret_cast<void (*)(float *, const float *, unsigned)>(dlsym(handle, "shade")),
      reinterpret_cast<void (*)(float *, const float *)>(dlsym(handle, "vector4")),
      reinterpret_cast<uint32_t (*)(void *, uint32_t)>(dlsym(handle, "increment"))};
  require(functions.constant && functions.mix && functions.wide && functions.shade &&
          functions.vector4 && functions.increment, "resolve generated entry points");
  require(functions.constant() == int(constant), "main worker calls generated function");
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto &value : state.seen) value.store(0);
    state.counter.store(0);
    state.completed = 0;
    state.functions = functions;
    state.constant = constant;
    ++state.phase;
    state.condition.notify_all();
  }
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait(lock, [&] { return state.completed == 4; });
  }
  require(state.counter.load() == 4096, "generated atomics share the caller memory");
  for (auto &value : state.seen) require(value.load() == 1, "atomic return values are unique");
  require(state.errors.load() == 0, "existing workers execute generated code correctly");
  std::atomic<uint32_t> fresh_counter{0};
  std::vector<std::atomic<unsigned>> fresh_seen(1024);
  std::thread fresh([&] {
    run_jit_functions(functions, constant, 99, fresh_counter, fresh_seen, state.errors);
  });
  fresh.join();
  require(fresh_counter.load() == 1024 && state.errors.load() == 0,
          "new worker executes generated code correctly");
  for (auto &value : fresh_seen) require(value.load() == 1, "new worker atomic results");
  require(dlclose(handle) == 0, "close generated module handle");
}

static void finish_jit_workers(SharedJitState &state)
{
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.stop = true;
    state.condition.notify_all();
  }
  for (auto &worker : state.workers) worker.join();
}
