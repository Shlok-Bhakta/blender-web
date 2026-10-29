// SPDX-License-Identifier: GPL-2.0-or-later
#include "GHOST_WebJITProbe.hh"
#include "GHOST_WebOSL.hh"
#include <OSL/oslexec.h>
#include <OSL/rendererservices.h>
#include <OSL/shaderglobals.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <emscripten.h>
#include <ffi.h>
#include <mutex>
#include <thread>
#include <vector>

static void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "OSL execution: %s\n", message);
    std::exit(1);
  }
}

struct GainCallback {
  ffi_cif cif{};
  ffi_type *arguments[1] = {&ffi_type_float};
  ffi_closure *closure = nullptr;
  void *entry = nullptr;
  std::atomic<unsigned> calls{0};
  void create()
  {
    require(ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_float, arguments) == FFI_OK,
            "prepare callback CIF");
    closure = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &entry));
    require(closure, "allocate callback");
    require(ffi_prep_closure_loc(
                closure,
                &cif,
                [](ffi_cif *, void *result, void **args, void *data) {
                  auto &self = *static_cast<GainCallback *>(data);
                  ++self.calls;
                  *static_cast<float *>(result) = *static_cast<float *>(args[0]) + 1.f;
                },
                this,
                entry) == FFI_OK,
            "publish callback");
  }
  void reset()
  {
    if (closure)
      ffi_closure_free(closure);
    closure = nullptr;
    entry = nullptr;
  }
  ~GainCallback()
  {
    reset();
  }
};
struct Renderer : OSL::RendererServices {
  GainCallback *gain = nullptr;
  std::atomic<unsigned> calls{0};
  bool get_userdata(bool derivatives,
                    OSL::ustringhash name,
                    OSL::TypeDesc type,
                    OSL::ShaderGlobals *globals,
                    void *value) override
  {
    if (name == OSL::ustringhash("gain") && type == OSL::TypeDesc::FLOAT) {
      auto *out = static_cast<float *>(value);
      out[0] = reinterpret_cast<float (*)(float)>(gain->entry)(globals->u);
      if (derivatives) {
        out[1] = out[2] = 0;
      }
      ++calls;
      return true;
    }
    if (name == OSL::ustringhash("tag") && type == OSL::TypeDesc::STRING) {
      auto hash = OSL::ustring(globals->u < .5f ? "browser" : "bravo").hash();
      std::memcpy(value, &hash, sizeof(hash));
      ++calls;
      return true;
    }
    return false;
  }
};

int run_system(int argc, char **argv)
{
#if defined(__EMSCRIPTEN__)
  GHOST_web_osl_install_probe_engine();
#endif
  GainCallback callback;
  callback.create();
  Renderer renderer;
  renderer.gain = &callback;
  OSL::ShadingSystem system(&renderer);
  system.attribute("searchpath:shader", argc > 1 ? argv[1] : "/fixtures");
  system.attribute("llvm_jit_target", "none");
  const OSL::ustring outputs[] = {OSL::ustring("result"),
                                  OSL::ustring("energy"),
                                  OSL::ustring("matches"),
                                  OSL::ustring("echoed")};
  system.attribute("renderer_outputs", OSL::TypeDesc(OSL::TypeDesc::STRING, 4), outputs);
  auto group = system.ShaderGroupBegin("browser");
  require(system.Parameter(*group, "gain", 1.25f, false), "varying gain parameter");
  require(system.Parameter(*group, "tag", OSL::ustring("browser"), false),
          "varying string parameter");
  require(system.Shader(*group, "surface", "web_execute", "layer"), "load shader");
  require(system.ShaderGroupEnd(*group), "finish group");
  auto shade = [&] {
    auto *thread = system.create_thread_info();
    auto *context = system.get_context(thread);
    OSL::ShaderGlobals globals{};
    for (unsigned i = 0; i < 128; ++i) {
      globals.u = float(i) / 127;
      globals.v = float(127 - i) / 127;
      globals.P = OSL::Vec3(globals.u, globals.v, .5f);
      require(system.execute(context, *group, globals), "execute original OSL shader");
      OSL::TypeDesc type;
      const float *color = static_cast<const float *>(
          system.get_symbol(*context, OSL::ustring("result"), type));
      require(color && std::fabs(color[0] - globals.u * (globals.u + 1.f)) < 1e-6 &&
                  std::fabs(color[1] - (globals.v + globals.u + 1.f)) < 1e-6 &&
                  std::fabs(color[2] - (std::sin(globals.u) + std::cos(globals.v))) < 1e-6,
              "varying color output");
      const float *energy = static_cast<const float *>(
          system.get_symbol(*context, OSL::ustring("energy"), type));
      require(energy && std::fabs(*energy - (globals.P.dot(globals.P) + globals.u + 1.f)) < 1e-6,
              "varying energy output");
      const int *matches = static_cast<const int *>(
          system.get_symbol(*context, OSL::ustring("matches"), type));
      require(matches && *matches == (globals.u < .5f ? 31 : 20), "string function output");
      const uint64_t *echoed = static_cast<const uint64_t *>(
          system.get_symbol(*context, OSL::ustring("echoed"), type));
      require(echoed && *echoed == OSL::ustring(globals.u < .5f ? "browser" : "bravo").hash(),
              "string hash output");
    }
    system.release_context(context);
    system.destroy_thread_info(thread);
  };
  std::mutex mutex;
  std::condition_variable condition;
  unsigned ready = 0;
  bool execute = false;
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < 4; ++i) {
    workers.emplace_back([&] {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        condition.notify_all();
        condition.wait(lock, [&] { return execute; });
      }
      shade();
    });
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return ready == 4; });
  }
  std::atomic<bool> stop_busy{false}, busy_ready{false};
  std::thread unrelated([&] {
    busy_ready.store(true);
    while (!stop_busy.load(std::memory_order_acquire)) {
    };
  });
  while (!busy_ready.load()) {
  }
  std::thread compiler([&] { shade(); });
  compiler.join();
  stop_busy.store(true, std::memory_order_release);
  unrelated.join();
  require(callback.calls == 128, "callback before replacement");
  callback.reset();
  callback.create();
  {
    std::lock_guard<std::mutex> lock(mutex);
    execute = true;
  }
  condition.notify_all();
  for (auto &worker : workers) {
    worker.join();
  }
  std::thread fresh([&] { shade(); });
  fresh.join();
  require(callback.calls == 768, "JIT renderer invokes original libffi callbacks across workers");
  require(renderer.calls == 1536, "renderer callbacks on all threads");
  return 0;
}

int main(int argc, char **argv)
{
  unsigned table_warm = 0;
  for (unsigned cycle = 0; cycle < 8; cycle++) {
    if (run_system(argc, argv))
      return 1;
    require(GHOST_web_osl_live_programs() == 0,
            "native OSL mapped owner releases all program records");
    require(owned_clean(), "native OSL mapped owner releases all program data/table entries");
    if (cycle == 1)
      table_warm = EM_ASM_INT({ return wasmTable.length; });
    if (cycle > 1)
      require(unsigned(EM_ASM_INT({ return wasmTable.length; })) == table_warm,
              "OSL system recreation reuses program table capacity");
  }
  std::puts(
      "{\"ownedOSLSystems\":8,\"nativeOwnerRetirement\":true,\"tableReused\":"
      "true,\"automaticPublication\":true,\"mixedFFICallbacks\":6144,"
      "\"workerCompilation\":true,\"busyWorkerPublication\":true,\"shadingPoints\":6144,"
      "\"rendererCallbacks\":12288}");
}
