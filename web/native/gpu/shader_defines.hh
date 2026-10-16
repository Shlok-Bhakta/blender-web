/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "GPU_platform_backend_enum.h"
inline bool GHOST_web_standard_shader_defines(blender::GPUBackendType backend,
                                             blender::Vector<blender::StringRefNull> &sources)
{
  if (backend != blender::GPU_BACKEND_WEBGPU) return false;
  sources.append("#define GPU_WEBGPU\n");
  return true;
}
