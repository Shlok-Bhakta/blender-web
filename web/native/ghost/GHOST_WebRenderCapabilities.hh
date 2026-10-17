/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <emscripten.h>
#include <cstring>
#include "BKE_report.hh"
#include "DNA_scene_types.h"

inline bool GHOST_web_render_supported(const blender::Scene *scene, blender::ReportList *reports)
{
  if (!emscripten_has_asyncify()) {
    blender::BKE_report(reports, blender::RPT_ERROR,
        "Rendering is not available in this browser yet. Use current desktop Chrome to render");
    return false;
  }
  if (scene && std::strcmp(scene->r.engine, "BLENDER_EEVEE") == 0) {
    const bool supported = EM_ASM_INT({
      const device = Module.preinitializedWebGPUDevice;
      return device && device.features.has('texture-formats-tier1') &&
        device.limits.maxComputeInvocationsPerWorkgroup >= 1024 &&
        device.limits.maxStorageTexturesPerShaderStage >= 8 &&
        device.limits.maxStorageBufferBindingSize >= 512 * 1024 * 1024 + 256;
    });
    if (!supported) {
      blender::BKE_report(reports, blender::RPT_ERROR,
          "This graphics adapter cannot run Eevee in this browser yet");
      return false;
    }
  }
  return true;
}
