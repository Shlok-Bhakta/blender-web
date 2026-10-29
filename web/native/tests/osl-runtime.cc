// SPDX-License-Identifier: GPL-2.0-or-later
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

struct Renderer : OSL::RendererServices {
  std::atomic<unsigned> calls{0};
  bool get_userdata(bool derivatives,
                    OSL::ustringhash name,
                    OSL::TypeDesc type,
                    OSL::ShaderGlobals *globals,
                    void *value) override
  {
    if (name == OSL::ustringhash("gain") && type == OSL::TypeDesc::FLOAT) {
      auto *out = static_cast<float *>(value);
      out[0] = globals->u + 1.0f;
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

int main(int argc, char **argv)
{
#if defined(__EMSCRIPTEN__)
  GHOST_web_osl_install_probe_engine();
#endif
  Renderer renderer;
  OSL::ShadingSystem system(&renderer);
  system.attribute("searchpath:shader", argc > 1 ? argv[1] : "/fixtures");
  system.attribute("llvm_jit_target", "none");
  const OSL::ustring outputs[] = {OSL::ustring("result"), OSL::ustring("energy"),
                                  OSL::ustring("matches"), OSL::ustring("echoed")};
  system.attribute("renderer_outputs", OSL::TypeDesc(OSL::TypeDesc::STRING, 4), outputs);
  auto group = system.ShaderGroupBegin("browser");
  require(system.Parameter(*group, "gain", 1.25f, false), "varying gain parameter");
  require(system.Parameter(*group, "tag", OSL::ustring("browser"), false), "varying string parameter");
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
  shade();
  {
    std::lock_guard<std::mutex> lock(mutex);
    execute = true;
  }
  condition.notify_all();
  for (auto &worker : workers) {
    worker.join();
  }
  std::thread fresh(shade);
  fresh.join();
  require(renderer.calls == 1536, "renderer callbacks on all threads");
  std::puts("{\"varyingOSLExecution\":true,\"points\":768,\"rendererCallbacks\":1536,"
            "\"existingWorkers\":4,\"newWorkers\":1,\"stringOperations\":true,"
            "\"reclamationVerified\":false}");
}
