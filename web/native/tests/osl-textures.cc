// SPDX-License-Identifier: GPL-2.0-or-later
#include "GHOST_WebJITProbe.hh"
#include "GHOST_WebOSL.hh"
#include "GHOST_WebWorkerResources.hh"
#include <OSL/oslexec.h>
#include <OSL/rendererservices.h>
#include <OSL/shaderglobals.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__EMSCRIPTEN__)
#  include <emscripten.h>
#  include <emscripten/threading.h>
#endif
#include <OSL/genclosure.h>
#include <OSL/oslclosure.h>
#include <OpenImageIO/imageio.h>
#include <ffi.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "OSL execution: %s\n", message);
    std::exit(1);
  }
}

struct DiffuseParams {
  OSL::Vec3 N;
  OSL::ustringhash label;
};
struct TagParams {
  OSL::ustringhash tag;
};
struct EmptyParams {
  int padding;
};
struct ClosureWeights {
  OSL::Color3 diffuse{0}, emission{0}, debug{0};
  uint64_t expectedTag = 0;
};
static void collect(const OSL::ClosureColor *node, OSL::Color3 weight, ClosureWeights &out)
{
  require(node, "non-null closure");
  if (node->id == OSL::ClosureColor::ADD) {
    collect(node->as_add()->closureA, weight, out);
    collect(node->as_add()->closureB, weight, out);
  }
  else if (node->id == OSL::ClosureColor::MUL)
    collect(node->as_mul()->closure, weight * node->as_mul()->weight, out);
  else if (node->id == 1) {
    const auto *c = node->as_comp();
    out.diffuse += weight * c->w;
    require(c->as<DiffuseParams>()->N == OSL::Vec3(0, 0, 1), "closure parameter layout");
    require(c->as<DiffuseParams>()->label.hash() == out.expectedTag,
            "full closure keyword string hash");
  }
  else if (node->id == 2)
    out.emission += weight * node->as_comp()->w;
  else if (node->id == 3) {
    out.debug += weight * node->as_comp()->w;
    require(node->as_comp()->as<TagParams>()->tag.hash() == out.expectedTag,
            "full closure formal string hash");
  }
  else
    require(false, "unexpected closure id");
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
  const std::string texture_path = std::string(argc > 1 ? argv[1] : "/fixtures") + "/texture.exr";
  const float pixels[] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1};
  auto image = OIIO::ImageOutput::create(texture_path);
  require(bool(image), "create original OpenImageIO image writer");
  require(image->open(texture_path, OIIO::ImageSpec(2, 2, 3, OIIO::TypeDesc::FLOAT)),
          "open EXR fixture");
  require(image->write_image(OIIO::TypeDesc::FLOAT, pixels) && image->close(),
          "write EXR texture fixture");
  image.reset();
  using namespace OSL;
  const ClosureParam diffuse_params[] = {CLOSURE_VECTOR_PARAM(DiffuseParams, N),
                                         CLOSURE_STRING_KEYPARAM(DiffuseParams, label, "label"),
                                         CLOSURE_FINISH_PARAM(DiffuseParams)};
  const ClosureParam emission_params[] = {CLOSURE_FINISH_PARAM(EmptyParams)};
  system.register_closure("diffuse", 1, diffuse_params, nullptr, nullptr);
  system.register_closure("emission", 2, emission_params, nullptr, nullptr);
  const ClosureParam tag_params[] = {CLOSURE_STRING_PARAM(TagParams, tag),
                                     CLOSURE_FINISH_PARAM(TagParams)};
  system.register_closure("debug", 3, tag_params, nullptr, nullptr);
  system.attribute("searchpath:shader", argc > 1 ? argv[1] : "/fixtures");
  system.attribute("llvm_jit_target", "none");
  const OSL::ustring outputs[] = {OSL::ustring("result"),
                                  OSL::ustring("energy"),
                                  OSL::ustring("matches"),
                                  OSL::ustring("echoed"),
                                  OSL::ustring("lobes"),
                                  OSL::ustring("texel")};
  system.attribute("renderer_outputs", OSL::TypeDesc(OSL::TypeDesc::STRING, 6), outputs);
  auto make_group = [&](const char *name) {
    auto group = system.ShaderGroupBegin(name);
    require(system.Parameter(*group, "gain", 1.25f, false), "varying gain parameter");
    require(system.Parameter(*group, "texname", OSL::ustring(texture_path), true),
            "texture filename parameter");
    require(system.Parameter(*group, "tag", OSL::ustring("browser"), false),
            "varying string parameter");
    require(system.Shader(*group, "surface", "web_execute", "layer"), "load shader");
    require(system.ShaderGroupEnd(*group), "finish group");
    return group;
  };
  auto group = make_group("browser-a"), second = make_group("browser-b");
  std::mutex phase_mutex;
  std::condition_variable phase_condition;
  std::atomic<bool> cancelled{false}, cancellation_ready{false};
  auto shade = [&](OSL::ShaderGroupRef target, bool cancel = false) {
    auto *thread = system.create_thread_info();
    auto *context = system.get_context(thread);
    OSL::ShaderGlobals globals{};
    for (unsigned i = 0; i < 128; ++i) {
      if (cancel && i == 16) {
        std::unique_lock<std::mutex> lock(phase_mutex);
        cancellation_ready.store(true, std::memory_order_release);
        phase_condition.notify_all();
        phase_condition.wait(lock, [&] { return cancelled.load(std::memory_order_acquire); });
      }
      if (cancel && cancelled.load(std::memory_order_acquire))
        break;
      globals.u = float(i) / 127;
      globals.v = float(127 - i) / 127;
      globals.P = OSL::Vec3(globals.u, globals.v, .5f);
      require(system.execute(context, *target, globals), "execute original OSL shader");
      OSL::TypeDesc type;
      auto *texel = static_cast<const float *>(
          system.get_symbol(*context, OSL::ustring("texel"), type));
      require(texel, "texture output");
      const unsigned texel_offset = ((globals.v < .5f ? 0 : 1) * 2 + (globals.u < .5f ? 0 : 1)) *
                                    3;
      for (unsigned channel = 0; channel < 3; channel++)
        require(std::fabs(texel[channel] - pixels[texel_offset + channel]) < 1e-6f,
                "original OSL/OpenImageIO nearest texture samples");
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
      auto *lobes = static_cast<OSL::ClosureColor *const *>(
          system.get_symbol(*context, OSL::ustring("lobes"), type));
      require(lobes && *lobes, "closure output");
      ClosureWeights weights;
      weights.expectedTag = OSL::ustring(globals.u < .5f ? "browser" : "bravo").hash();
      collect(*lobes, OSL::Color3(1), weights);
      const OSL::Color3 reference_diffuse(globals.u, .25f, globals.v),
          reference_emission(.1f, .2f, .3f);
      for (unsigned channel = 0; channel < 3; channel++)
        require(std::fabs(weights.diffuse[channel] - reference_diffuse[channel]) < 1e-6f &&
                    std::fabs(weights.emission[channel] - reference_emission[channel]) < 1e-6f,
                "closure add/multiply/component weights");
      for (unsigned channel = 0; channel < 3; channel++)
        require(std::fabs(weights.debug[channel] - .4f) < 1e-6f, "debug closure weight");
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
    workers.emplace_back([&, i] {
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        condition.notify_all();
        condition.wait(lock, [&] { return execute; });
      }
      shade(i % 2 ? group : second);
    });
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return ready == 4; });
  }
  std::atomic<bool> stop_busy{false}, busy_ready{false};
  std::thread unrelated([&] {
    {
      std::lock_guard<std::mutex> lock(phase_mutex);
      busy_ready.store(true);
      phase_condition.notify_all();
    }
    while (!stop_busy.load(std::memory_order_acquire)) {
    }
  });
  {
    std::unique_lock<std::mutex> lock(phase_mutex);
    phase_condition.wait(lock, [&] { return busy_ready.load(); });
  }
  std::atomic<unsigned> compile_ready{0};
  auto compile = [&](OSL::ShaderGroupRef target) {
    compile_ready.fetch_add(1);
    while (compile_ready.load() < 2) {
    }
    shade(target);
  };
  std::thread compiler([&] { compile(group); });
  std::thread other_compiler([&] { compile(second); });
  compiler.join();
  other_compiler.join();
#if defined(__EMSCRIPTEN__)
  require(GHOST_web_osl_live_programs() == 2, "two concurrently requested programs remain live");
#endif
  stop_busy.store(true, std::memory_order_release);
  unrelated.join();
  require(callback.calls == 256, "callback before replacement");
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

  std::thread fresh([&] { shade(group); });
  fresh.join();
  std::thread cancel_worker([&] { shade(second, true); });
  {
    std::unique_lock<std::mutex> lock(phase_mutex);
    phase_condition.wait(lock, [&] { return cancellation_ready.load(std::memory_order_acquire); });
    cancelled.store(true, std::memory_order_release);
    phase_condition.notify_all();
  }
  cancel_worker.join();
#if defined(__EMSCRIPTEN__)
  require(GHOST_web_osl_live_programs() == 2,
          "cancellation preserves native owner until all users join");
#endif
  require(callback.calls == 912, "JIT renderer invokes original libffi callbacks across workers");
  require(renderer.calls == 1824, "renderer callbacks on all threads");
  return 0;
}

#if defined(__EMSCRIPTEN__)
static int root_clean()
{
  int result = 0;
  GHOST_web_invoke_runtime_thread([](void *value) { *static_cast<int *>(value) = owned_clean(); },
                                  &result);
  return result;
}
static unsigned root_table_length()
{
  unsigned result = 0;
  GHOST_web_invoke_runtime_thread(
      [](void *value) {
        *static_cast<unsigned *>(value) = EM_ASM_INT({ return wasmTable.length; });
      },
      &result);
  return result;
}
#endif
int main(int argc, char **argv)
{
#if defined(__EMSCRIPTEN__)
  require(!emscripten_is_main_runtime_thread(),
          "texture execution must leave the browser runtime thread available");
#endif
  unsigned table_warm = 0;
  for (unsigned cycle = 0; cycle < 8; cycle++) {
    if (run_system(argc, argv))
      return 1;
#if defined(__EMSCRIPTEN__)
    require(GHOST_web_osl_live_programs() == 0,
            "native OSL mapped owner releases all program records");
    require(root_clean(), "native OSL mapped owner releases all program data/table entries");
    if (cycle == 1)
      table_warm = root_table_length();
    if (cycle > 1)
      require(unsigned(root_table_length()) == table_warm,
              "OSL system recreation reuses program table capacity");
#endif
  }
#if defined(__EMSCRIPTEN__)
  std::puts(
      "{\"ownedOSLSystems\":8,\"nativeOwnerRetirement\":true,\"tableReused\":"
      "true,\"automaticPublication\":true,\"mixedFFICallbacks\":7296,"
      "\"workerCompilation\":true,\"busyWorkerPublication\":true,\"shadingPoints\":7296,"
      "\"rendererCallbacks\":14592,\"concurrentCompilation\":true,\"cooperativeCancellation\":"
      "true,\"closureTrees\":true,\"closureStrings\":true,\"textureSamples\":true,"
      "\"mainOnWorker\":true}");
#else
  std::puts(
      "{\"nativeClosureTrees\":true,\"nativeClosureStrings\":true,\"nativeTextureSamples\":true,"
      "\"shadingPoints\":7296,\"rendererCallbacks\":14592,"
      "\"ffiCallbacks\":7296}");
#endif
}
