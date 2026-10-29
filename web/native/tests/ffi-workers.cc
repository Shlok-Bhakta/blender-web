/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <emscripten.h>
#include <emscripten/threading.h>
#include <ffi.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace {
#ifndef NDEBUG
#  error "Compile this check with NDEBUG to verify release callback behavior"
#endif
struct Pair { double number; int count; };
std::atomic<unsigned> calls{0};
void check(bool condition, unsigned line)
{
  if (!condition) {
    std::fprintf(stderr, "libffi worker check failed at line %u\n", line);
    std::abort();
  }
}
#define require(condition) check((condition), __LINE__)
EM_JS(int, table_size, (), { return wasmTable.length; });
void scalar(ffi_cif *, void *result, void **args, void *)
{
  *static_cast<int *>(result) = *static_cast<int *>(args[0]) * 3 + *static_cast<int *>(args[1]) + 5;
  ++calls;
}
void wide(ffi_cif *, void *result, void **args, void *)
{
  *static_cast<int64_t *>(result) = *static_cast<int64_t *>(args[0]) +
      int64_t(*static_cast<double *>(args[1])) + *static_cast<int *>(args[2]);
  ++calls;
}
void aggregate(ffi_cif *, void *result, void **args, void *)
{
  auto value = *static_cast<Pair *>(args[0]);
  *static_cast<Pair *>(result) = {value.number + .5, value.count + int(*static_cast<int64_t *>(args[1]))};
  ++calls;
}
void variable(ffi_cif *, void *result, void **args, void *)
{
  require(*static_cast<int *>(args[0]) == 2);
  *static_cast<double *>(result) = *static_cast<double *>(args[1]) + *static_cast<double *>(args[2]);
  ++calls;
}
void no_extra_arguments(ffi_cif *, void *result, void **args, void *)
{
  *static_cast<int *>(result) = *static_cast<int *>(args[0]) + 1;
  ++calls;
}
void invoke(void *code, unsigned kind)
{
  switch (kind) {
    case 0: require(reinterpret_cast<int (*)(int, int)>(code)(7, 9) == 35); break;
    case 1: require(reinterpret_cast<int64_t (*)(int64_t, double, int)>(code)(
                         (1LL << 54) + 13, 4., -2) == (1LL << 54) + 15); break;
    case 2: {
      auto result = reinterpret_cast<Pair (*)(Pair, int64_t)>(code)({1.25, 9}, 7);
      require(result.number == 1.75 && result.count == 16);
      break;
    }
    case 3: require(reinterpret_cast<double (*)(int, ...)>(code)(2, 1.5, 4.5) == 6.); break;
  }
}
struct ScalarClosure {
  ffi_cif cif;
  ffi_type *args[2] = {&ffi_type_sint32, &ffi_type_sint32};
  void *code = nullptr;
  ffi_closure *memory;
  ScalarClosure()
  {
    require(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint32, args) == FFI_OK);
    memory = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
    require(memory && ffi_prep_closure_loc(memory, &cif, scalar, nullptr, code) == FFI_OK);
  }
  ~ScalarClosure() { ffi_closure_free(memory); }
};
void wait(std::atomic<unsigned> &word)
{
  while (!word.load()) emscripten_futex_wait(&word, 0, INFINITY);
}
void signal(std::atomic<unsigned> &word)
{
  word.store(1);
  emscripten_futex_wake(&word, INT_MAX);
}
struct Blocking {
  std::atomic<unsigned> entered{0}, release{0};
  bool exits;
};
void blocking(ffi_cif *, void *result, void **args, void *data)
{
  auto &state = *static_cast<Blocking *>(data);
  signal(state.entered);
  wait(state.release);
  if (state.exits) pthread_exit(reinterpret_cast<void *>(uintptr_t(0x55aa)));
  // A retiring closure must not hold the registry mutex while waiting for us.
  { ScalarClosure nested; invoke(nested.code, 0); }
  *static_cast<int *>(result) = *static_cast<int *>(args[0]) + 1;
  ++calls;
}
void retirement(bool exits)
{
  ffi_cif cif;
  ffi_type *args[] = {&ffi_type_sint32};
  require(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint32, args) == FFI_OK);
  void *code = nullptr;
  auto *memory = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
  Blocking state{{0}, {0}, exits};
  require(memory && ffi_prep_closure_loc(memory, &cif, blocking, &state, code) == FFI_OK);
  pthread_t caller;
  require(pthread_create(&caller, nullptr, [](void *target) -> void * {
    require(reinterpret_cast<int (*)(int)>(target)(41) == 42);
    return nullptr;
  }, code) == 0);
  wait(state.entered);
  std::atomic<unsigned> freeing{0}, freed{0};
  std::thread freer([&] { signal(freeing); ffi_closure_free(memory); signal(freed); });
  wait(freeing);
  emscripten_thread_sleep(20);
  require(!freed.load());
  signal(state.release);
  void *result = nullptr;
  require(pthread_join(caller, &result) == 0);
  require(result == (exits ? reinterpret_cast<void *>(uintptr_t(0x55aa)) : nullptr));
  freer.join();
  require(freed.load());
}
}  // namespace

int main()
{
  {
    ffi_cif cif;
    ffi_type *args[] = {&ffi_type_sint32};
    require(ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 1, &ffi_type_sint32, args) == FFI_OK);
    void *code = nullptr;
    auto *memory = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
    require(memory && ffi_prep_closure_loc(memory, &cif, no_extra_arguments, nullptr, code) == FFI_OK);
    require(reinterpret_cast<int (*)(int, ...)>(code)(7) == 8);
    ffi_closure_free(memory);
  }
  std::fprintf(stderr, "Testing callbacks on existing sleeping workers\n");
  // All workers start before the first closure, without an initialization call.
  {
    std::atomic<unsigned> release{0}, ready{0};
    void *code = nullptr;
    std::vector<std::thread> sleepers;
    for (int i = 0; i < 8; ++i) {
      try { sleepers.emplace_back([&] {
      ready.fetch_add(1);
      emscripten_futex_wake(&ready, INT_MAX);
      wait(release);
      invoke(code, 0);
      }); }
      catch (const std::exception &error) {
        std::fprintf(stderr, "Creating sleeper %d failed: %s\n", i, error.what());
        std::abort();
      }
    }
    while (ready.load() != 8) {
      unsigned observed = ready.load();
      if (observed != 8) emscripten_futex_wait(&ready, observed, INFINITY);
    }
    emscripten_thread_sleep(20);
    for (int i = 0; i < 200; ++i) { ScalarClosure temporary; }
    ScalarClosure final;
    code = final.code;
    signal(release);
    for (auto &thread : sleepers) thread.join();
  }
  std::fprintf(stderr, "Testing 1000 signature changes and worker reuse\n");
  std::mutex mutex;
  std::condition_variable condition;
  bool stop = false;
  unsigned generation = 0, complete = 0, ready = 0, kind = 0;
  void *code = nullptr;
  std::vector<std::thread> workers;
  try {
  for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
    std::unique_lock lock(mutex);
    ++ready;
    condition.notify_all();
    unsigned seen = 0;
    for (;;) {
      condition.wait(lock, [&] { return stop || generation > seen; });
      if (stop) return;
      seen = generation;
      auto target = code;
      auto signature = kind;
      lock.unlock();
      invoke(target, signature);
      lock.lock();
      ++complete;
      condition.notify_all();
    }
  });
  { std::unique_lock lock(mutex); condition.wait(lock, [&] { return ready == 8; }); }
  int bounded = 0;
  for (unsigned iteration = 0; iteration < 1000; ++iteration) {
    if (iteration % 250 == 0) std::fprintf(stderr, "Signature reuse %u\n", iteration);
    ffi_cif cif;
    ffi_type *fields[] = {&ffi_type_double, &ffi_type_sint32, nullptr};
    ffi_type pair{};
    pair.type = FFI_TYPE_STRUCT;
    pair.elements = fields;
    ffi_type *scalar_args[] = {&ffi_type_sint32, &ffi_type_sint32};
    ffi_type *wide_args[] = {&ffi_type_sint64, &ffi_type_double, &ffi_type_sint32};
    ffi_type *pair_args[] = {&pair, &ffi_type_sint64};
    ffi_type *var_args[] = {&ffi_type_sint32, &ffi_type_double, &ffi_type_double};
    kind = iteration % 4;
    using Function = void (*)(ffi_cif *, void *, void **, void *);
    Function functions[] = {scalar, wide, aggregate, variable};
    ffi_status status = kind == 3 ?
        ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 3, &ffi_type_double, var_args) :
        ffi_prep_cif(&cif, FFI_DEFAULT_ABI, kind == 1 ? 3 : 2,
                     kind == 0 ? &ffi_type_sint32 : kind == 1 ? &ffi_type_sint64 : &pair,
                     kind == 0 ? scalar_args : kind == 1 ? wide_args : pair_args);
    require(status == FFI_OK);
    auto *memory = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
    require(memory && ffi_prep_closure_loc(memory, &cif, functions[kind], nullptr, code) == FFI_OK);
    invoke(code, kind);
    {
      std::unique_lock lock(mutex);
      complete = 0;
      ++generation;
      condition.notify_all();
      condition.wait(lock, [&] { return complete == 8; });
    }
    if (iteration % 4 == 0) {
      std::thread fresh([&] { invoke(code, kind); });
      try { fresh.join(); }
      catch (const std::exception &error) {
        std::fprintf(stderr, "Joining fresh worker failed: %s\n", error.what());
        std::abort();
      }
    }
    if (iteration == 0) bounded = table_size();
    require(table_size() == bounded);
    ffi_closure_free(memory);
  }
  std::vector<std::thread> publishers;
  std::fprintf(stderr, "Testing concurrent publishers\n");
  for (int i = 0; i < 4; ++i) publishers.emplace_back([] {
    for (int j = 0; j < 20; ++j) { ScalarClosure closure; invoke(closure.code, 0); }
  });
  for (auto &thread : publishers) thread.join();
  require(table_size() <= bounded + 4);
  { std::lock_guard lock(mutex); stop = true; }
  condition.notify_all();
  for (auto &thread : workers) thread.join();
  retirement(false);
  retirement(true);
  require(calls.load() == 9341);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "Worker check exception: %s\n", error.what());
    std::abort();
  } catch (...) {
    std::fprintf(stderr, "Worker check raised a non-C++ exception\n");
    std::abort();
  }
  std::printf("{\"calls\":%u,\"signatureReuseIterations\":1000,\"signatureKinds\":4,"
              "\"existingWorkers\":8,\"sleepingPublications\":200,\"concurrentClosures\":80,"
              "\"nestedRetirement\":true,\"threadExitDuringCallback\":true,"
              "\"zeroExtraVariadicArguments\":true,"
              "\"boundedSerialTable\":true,\"releaseAssertionsDisabled\":true}\n", calls.load());
}
