/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <emscripten.h>
#include <blender-web-numpy.hh>
#include <blender-web-python.hh>
#include "GHOST_WebAudio.hh"
#include "GHOST_WebGLTF.hh"
#ifdef WITH_USD
#  include <blender-web-usd.hh>
#endif
#include <emscripten/wasmfs.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int web_argc;
const char **web_argv;
std::vector<std::string> web_argument_storage;
std::vector<const char *> web_arguments;
int (*web_main)(int, const char **);
bool web_device_ready = false;
}
extern "C" EMSCRIPTEN_KEEPALIVE void blender_web_device_ready()
{
  web_device_ready = true;
  web_main(web_argc, web_argv);
}
inline bool GHOST_web_bootstrap(int argc, const char **argv,
                                int (*entry)(int, const char **))
{
  if (web_device_ready) {
    GHOST_web_register_numpy();
    GHOST_web_register_python_extensions();
    GHOST_web_register_gltf();
#ifdef WITH_AUDASPACE
    GHOST_web_register_audio();
#endif
#ifdef WITH_USD
    GHOST_web_register_usd();
#endif
    auto backend = wasmfs_create_opfs_backend();
    if (!backend || wasmfs_create_directory("/user", 0777, backend)) {
      fprintf(stderr, "Persistent browser storage unavailable; this session uses memory\n");
      mkdir("/user", 0777);
    }
    mkdir("/user/blender-web", 0777);
    mkdir("/user/blender-web/home", 0777);
    setenv("HOME", "/user/blender-web/home", 1);
    setenv("BLENDER_SYSTEM_PYTHON", "/5.2/python", 1);
    setenv("BLENDER_SYSTEM_SCRIPTS", "/5.2/scripts", 1);
    setenv("BLENDER_SYSTEM_DATAFILES", "/5.2/datafiles", 1);
    return false;
  }
  // Emscripten releases callMain's stack when the async bootstrap unwinds.
  // Keep argv on the heap until the device callback re-enters the application.
  web_argument_storage.reserve(argc);
  web_arguments.reserve(argc + 1);
  for (int i = 0; i < argc; i++) web_argument_storage.emplace_back(argv[i]);
  for (const auto &argument : web_argument_storage) web_arguments.push_back(argument.c_str());
  web_arguments.push_back(nullptr);
  web_argc = argc;
  web_argv = web_arguments.data();
  web_main = entry;
  EM_ASM({
    (async () => {
      try {
        // The pinned emdawnwebgpu port predates the hyphenated alpha spelling.
        // Keep dual-source blend descriptors compatible with browser WebGPU.
        WebGPU.BlendFactor = WebGPU.BlendFactor.map(value =>
          value === 'src1alpha' ? 'src1-alpha' :
          value === 'one-minus-src1alpha' ? 'one-minus-src1-alpha' : value);
        await globalThis['blenderLoadShaderCache']();
        try {
          const storage = await navigator.storage.estimate();
          const available = storage.quota - storage.usage;
          globalThis['blenderStorageAvailable'] = Number.isFinite(available) ? Math.max(0, available) : -1;
        } catch (error) { console.warn('Storage estimate unavailable', error); }
        const adapter = await navigator.gpu.requestAdapter({powerPreference: 'high-performance'});
        if (!adapter) throw new Error('WebGPU adapter unavailable');
        const requiredFeatures = [...adapter.features].filter(f =>
          ['dual-source-blending', 'float32-filterable', 'rg11b10ufloat-renderable',
           'bgra8unorm-storage', 'shader-f16', 'depth32float-stencil8',
           'indirect-first-instance', 'clip-distances', 'texture-formats-tier1', 'texture-formats-tier2', 'float32-blendable'].includes(f));
        const requiredLimits = {};
        for (const key of ['maxStorageTexturesPerShaderStage', 'maxStorageBuffersPerShaderStage',
          'maxSampledTexturesPerShaderStage', 'maxSamplersPerShaderStage',
          'maxUniformBuffersPerShaderStage', 'maxComputeWorkgroupSizeX',
          'maxComputeWorkgroupSizeY', 'maxComputeWorkgroupSizeZ',
          'maxComputeInvocationsPerWorkgroup', 'maxComputeWorkgroupStorageSize',
          'maxStorageBufferBindingSize', 'maxUniformBufferBindingSize', 'maxBufferSize',
          'maxBindGroups', 'maxBindingsPerBindGroup', 'maxTextureDimension2D',
          'maxTextureDimension3D', 'maxTextureArrayLayers', 'maxColorAttachments',
          'maxColorAttachmentBytesPerSample']) {
          if (adapter.limits[key] !== undefined) requiredLimits[key] = adapter.limits[key];
        }
        Module.preinitializedWebGPUDevice = await adapter.requestDevice({requiredFeatures, requiredLimits});
        Module.preinitializedWebGPUDevice.addEventListener('uncapturederror', event => {
          console.error('Blender WebGPU error', event.error.message);
        });
        Module.preinitializedWebGPUDevice.lost.then(info => {
          console.error('Blender WebGPU device lost', info.message);
        });
        _blender_web_device_ready();
      } catch (error) {
        if (error !== 'unwind') console.error('Blender startup failed', error);
      }
    })();
  });
  emscripten_exit_with_live_runtime();
  return true;
}
