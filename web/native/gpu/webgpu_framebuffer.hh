/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU framebuffer. Wraps the base FrameBuffer's color/depth attachments and
 * begins a WGPURenderPass over them. Clears are deferred into the render pass's
 * load ops (WebGPU has no mid-pass clear). Pixel readback (read()) needs an async
 * buffer map, which cannot complete while wasm holds the browser main thread, so
 * it records the copy but the data is only valid once control returns to JS.
 */

#pragma once

#include "BLI_math_vector_types.hh"

#include "gpu_framebuffer_private.hh"

#include <webgpu/webgpu.h>

namespace blender::gpu {

class WebGPUFrameBuffer : public FrameBuffer {
 private:
  bool enabled_srgb_ = false;

  /* Deferred clear state, consumed by the next begin_render_pass(). */
  bool color_clear_pending_[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
  double4 clear_color_[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
  bool depth_clear_pending_ = false;
  float clear_depth_ = 1.0f;

  /* Attachment-less rendering (EEVEE shadow atlas / tag passes write only via
   * imageStore): WebGPU requires >= 1 attachment, so a dummy R8Unorm color
   * target of the framebuffer's default size is attached at slot 0. */
  WGPUTexture dummy_att_tex_ = nullptr;
  WGPUTextureView dummy_att_view_ = nullptr;
  int dummy_w_ = 0, dummy_h_ = 0;

 public:
  /* True when the fb has no color and no depth attachments (uses default size). */
  bool is_attachmentless() const
  {
    for (int i = 0; i < GPU_FB_MAX_ATTACHMENT; i++) {
      if (attachments_[i].tex) {
        return false;
      }
    }
    return width_ > 0 && height_ > 0;
  }

 private:

 private:
  /* True only for the single window backbuffer (context `back_left`). Its
   * contents are presented to the canvas with a one-shot vertical flip, so —
   * unlike offscreen targets — its viewport/scissor Y must NOT be re-flipped
   * again when translated to WebGPU's top-left origin (see apply_viewport_scissor). */
  bool is_backbuffer_ = false;

 public:
  WebGPUFrameBuffer(const char *name) : FrameBuffer(name) {}

  void set_is_backbuffer(bool value)
  {
    is_backbuffer_ = value;
  }
  bool is_backbuffer() const
  {
    return is_backbuffer_;
  }

  void bind(bool enabled_srgb) override;
  bool check(char err_out[256]) override;
  void clear(GPUFrameBufferBits buffers,
             const double4 clear_color,
             float clear_depth,
             uint clear_stencil) override;
  void clear_multi(Span<double4> clear_cols) override;
  void clear_attachment(GPUAttachmentType type, const double4 clear_value) override;
  /* Immediately record an empty pass consuming the pending clears (GL glClear
   * semantics — see impl comment). */
  void execute_pending_clears();
  void attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls) override;
  void read(GPUFrameBufferBits planes,
            eGPUDataFormat format,
            const int area[4],
            int channel_len,
            int slot,
            void *r_data) override;
  void blit_to(GPUFrameBufferBits planes,
               int src_slot,
               FrameBuffer *dst,
               int dst_slot,
               int dst_offset_x,
               int dst_offset_y) override;

  /* Begin a render pass over this framebuffer's attachments on `enc`. Consumes
   * pending clears (Clear load-op) else Load. Returns null if no usable
   * attachments / no device. */
  WGPURenderPassEncoder begin_render_pass(WGPUCommandEncoder enc);

  /* Pipeline-state queries (color/depth formats of the bound attachments). */
  int color_attachment_count() const;
  WGPUTextureFormat color_format(int slot) const;
  WGPUTextureFormat depth_format() const; /* Undefined if none. */
  /* True when per-index viewports were set (gl_ViewportIndex emulation): the
   * hardware pass viewport must then stay the FULL target — the vertex shader
   * scales clip coords per index itself. */
  bool multi_viewport() const
  {
    return multi_viewport_;
  }

 protected:
  void subpass_transition_impl(const GPUAttachmentState depth_attachment_state,
                               Span<GPUAttachmentState> color_attachment_states) override;

 private:
  /* Slots currently used as (emulated) sub-pass inputs: excluded from the
   * effective attachment set (render pass AND pipeline formats) while the
   * canonical attachments_ array stays intact, GL-style. */
  bool subpass_detached_[GPU_FB_MAX_COLOR_ATTACHMENT] = {};

 public:
};

}  // namespace blender::gpu
