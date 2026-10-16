/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU batch. Builds a WGPURenderPipeline from the bound shader's modules, the
 * active framebuffer's color/depth formats, and the batch's vertex format(s),
 * then records a draw into the active render pass. Pipelines are cached on the
 * context.
 */

#pragma once

#include "GPU_batch.hh"

#include <webgpu/webgpu.h>

namespace blender::gpu {

/* Record a draw of `vertex_count` vertices from a single raw vertex buffer laid
 * out per `format`, using the currently bound shader + GPU state (immediate-mode
 * path; implemented in webgpu_batch.cc to reuse the pipeline-cache helpers). */
void webgpu_immediate_draw(GPUPrimType prim_type,
                           const GPUVertFormat &format,
                           WGPUBuffer vbo,
                           uint vertex_count);

class WebGPUBatch : public Batch {
 public:
  void draw(int vertex_first, int vertex_count, int instance_first, int instance_count) override;
  void draw_indirect(StorageBuf *indirect_buf, intptr_t offset) override
  {
    record_draw(0, 0, 0, 1, indirect_buf, 1, offset, 0);
  }
  void multi_draw_indirect(StorageBuf *indirect_buf,
                           int count,
                           intptr_t offset,
                           intptr_t stride) override
  {
    record_draw(0, 0, 0, 1, indirect_buf, count, offset, stride);
  }

 private:
  /* Shared pipeline/bind-group setup + draw recording. Direct draws pass
   * indirect_buf == nullptr; indirect draws record `indirect_count`
   * (Draw|DrawIndexed)Indirect calls stepping `indirect_stride` bytes from
   * `indirect_offset` (core WebGPU has no multi-draw). */
  void record_draw(int vertex_first,
                   int vertex_count,
                   int instance_first,
                   int instance_count,
                   StorageBuf *indirect_buf,
                   int indirect_count,
                   intptr_t indirect_offset,
                   intptr_t indirect_stride);
};

}  // namespace blender::gpu
