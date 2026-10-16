/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU immediate-mode drawing: vertices are accumulated into a CPU staging
 * vector between immBegin/immEnd, then uploaded into a transient WGPUBuffer and
 * drawn through the shared pipeline-cache path (webgpu_immediate_draw). The
 * command encoder retains the buffer, so it is released right after recording.
 */

#pragma once

#include <vector>

#include "gpu_immediate_private.hh"

#include <webgpu/webgpu.h>

namespace blender::gpu {

class WebGPUImmediate : public Immediate {
 private:
  std::vector<uchar> data_;

 public:
  uchar *begin() override;
  void end() override;
};

}  // namespace blender::gpu
