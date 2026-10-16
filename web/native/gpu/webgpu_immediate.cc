/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "GPU_matrix.hh"
#include "GPU_shader.hh"

#include "webgpu_batch.hh"
#include "webgpu_context.hh"
#include "webgpu_immediate.hh"
#include "webgpu_shader.hh"

namespace blender::gpu {

uchar *WebGPUImmediate::begin()
{
  const size_t bytes_needed = size_t(vertex_len) * vertex_format.stride;
  /* +1 vertex of slack for the LINE_LOOP -> LINE_STRIP close in end(). */
  data_.resize(bytes_needed + vertex_format.stride);
  return data_.data();
}

void WebGPUImmediate::end()
{
  if (vertex_idx == 0) {
    return;
  }
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx == nullptr || ctx->device() == nullptr) {
    return;
  }

  if (prim_type == GPU_PRIM_LINE_LOOP) {
    /* Close the loop by appending the first vertex (slack reserved in begin()). */
    std::memcpy(data_.data() + size_t(vertex_idx) * vertex_format.stride,
                data_.data(),
                vertex_format.stride);
    prim_type = GPU_PRIM_LINE_STRIP;
    vertex_idx += 1;
  }
  else if (prim_type == GPU_PRIM_TRI_FAN && vertex_idx >= 3) {
    /* WebGPU has no triangle-fan topology; mapping a fan onto a list drops all
     * but the first triangle (the splash image rendered as a lone diagonal
     * half). Expand fan (v0, vi, vi+1) into an explicit triangle list. */
    const size_t stride = vertex_format.stride;
    const uint tri_count = vertex_idx - 2;
    std::vector<uchar> fan(3 * size_t(tri_count) * stride);
    for (uint t = 0; t < tri_count; t++) {
      std::memcpy(fan.data() + (3 * size_t(t) + 0) * stride, data_.data(), stride);
      std::memcpy(fan.data() + (3 * size_t(t) + 1) * stride,
                  data_.data() + size_t(t + 1) * stride,
                  stride);
      std::memcpy(fan.data() + (3 * size_t(t) + 2) * stride,
                  data_.data() + size_t(t + 2) * stride,
                  stride);
    }
    data_.assign(fan.begin(), fan.end());
    prim_type = GPU_PRIM_TRIS;
    vertex_idx = 3 * tri_count;
  }

  const size_t used = size_t(vertex_idx) * vertex_format.stride;
  const size_t buf_size = (used + 3) & ~size_t(3);

  /* No flush needed: the buffer is FRESH, so the queued write cannot
   * retroactively affect any already-recorded draw (they reference their own
   * buffers). A flush here = one full submit per immBegin/End = pathological. */
  WGPUBufferDescriptor bd = {};
  bd.label = {"immediate", WGPU_STRLEN};
  bd.size = buf_size;
  bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  WGPUBuffer buf = wgpuDeviceCreateBuffer(ctx->device(), &bd);
  if (buf == nullptr) {
    return;
  }
  wgpuQueueWriteBuffer(ctx->queue(), buf, 0, data_.data(), buf_size);

  if (shader->is_polyline) {
    /* Polyline shaders read positions/colors from SSBOs and expand to triangles
     * (the wide-line workaround). Bind the vertex data at the polyline slots. */
    ctx->bind_ssbo(GPU_SSBO_POLYLINE_POS_BUF_SLOT, buf);
    ctx->bind_ssbo(GPU_SSBO_POLYLINE_COL_BUF_SLOT, buf);
    ctx->bind_ssbo(GPU_SSBO_INDEX_BUF_SLOT, buf);
    this->polyline_draw_workaround(0);
  }
  else {
    GPU_matrix_bind(ctx->shader);
    webgpu_immediate_draw(prim_type, vertex_format, buf, vertex_idx);
  }

  /* The recorded commands (and bind groups) hold their own references. */
  wgpuBufferRelease(buf);
}

}  // namespace blender::gpu
