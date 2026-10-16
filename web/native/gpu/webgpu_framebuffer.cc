/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>

#include "GPU_texture.hh"

#include "webgpu_context.hh"
#include "webgpu_framebuffer.hh"
#include "webgpu_texture.hh"

namespace blender::gpu {

void WebGPUFrameBuffer::bind(bool enabled_srgb)
{
  static int s_bind_log = 0;
  if (s_bind_log < 12) {
    fprintf(stderr,
            "WGPU_FB::bind #%d '%s' colors=%d depth=%d\n",
            s_bind_log++,
            name_,
            color_attachment_count(),
            int(depth_tex() != nullptr));
    fflush(stderr);
  }
  enabled_srgb_ = enabled_srgb;
  Context *ctx = Context::get();
  if (ctx) {
    /* A change of framebuffer ends the current render pass; the next draw
     * re-opens one over this framebuffer's attachments. */
    WebGPUContext::get()->render_pass_end();
    ctx->active_fb = this;
  }
  /* Derive size from the first present attachment. */
  for (int i = 0; i < GPU_FB_MAX_ATTACHMENT; i++) {
    if (attachments_[i].tex) {
      gpu::Texture *t = attachments_[i].tex;
      size_set(t->width_get(), std::max(t->height_get(), 1));
      break;
    }
  }
  /* Mirror the GL backend: when the attachment set changes, default the viewport
   * and scissor to the full framebuffer. Callers that need a sub-rect set it
   * explicitly afterwards (viewport_set only marks dirty_state_, not
   * dirty_attachments_, so their value survives later binds). Without this the
   * framebuffer's viewport_ stays {0,0,0,0}: GPU_viewport_size_get_f then reads
   * back a zero size, and the polyline wide-line shader divides its screen-space
   * edge expansion by that zero viewport -> NaN, so every overlay/gizmo wide line
   * (e.g. the transform gizmo arrow stems) collapses and never renders. */
  if (dirty_attachments_) {
    viewport_reset();
    scissor_reset();
    dirty_attachments_ = false;
  }
  Shader::set_framebuffer_srgb_target(enabled_srgb);
}

bool WebGPUFrameBuffer::check(char /*err_out*/[256])
{
  return true;
}

void WebGPUFrameBuffer::execute_pending_clears()
{
  /* Execute the clear NOW (GL glClear semantics) by opening a pass — pass begin
   * consumes the pending flags as Clear load ops — and ending it empty. Deferring
   * to "the next draw's pass" is wrong for clear-only framebuffers: engines
   * (workbench resources_.clear_fb) clear via a dedicated fb that aliases the
   * render targets but never receives draws, so a deferred clear never runs. */
  bool any_pending = depth_clear_pending_;
  for (int i = 0; i < GPU_FB_MAX_COLOR_ATTACHMENT && !any_pending; i++) {
    any_pending |= color_clear_pending_[i];
  }
  if (!any_pending) {
    return;
  }
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx == nullptr || ctx->device() == nullptr) {
    return;
  }
  ctx->render_pass_end();
  ctx->render_pass_ensure(*this);
  ctx->render_pass_end();
}

void WebGPUFrameBuffer::clear(GPUFrameBufferBits buffers,
                              const double4 clear_color,
                              float clear_depth,
                              uint /*clear_stencil*/)
{
  if (buffers & GPU_COLOR_BIT) {
    for (int i = 0; i < GPU_FB_MAX_COLOR_ATTACHMENT; i++) {
      color_clear_pending_[i] = true;
      clear_color_[i] = clear_color;
    }
  }
  if (buffers & GPU_DEPTH_BIT) {
    depth_clear_pending_ = true;
    clear_depth_ = clear_depth;
  }
  execute_pending_clears();
}

void WebGPUFrameBuffer::clear_multi(Span<double4> clear_cols)
{
  for (int i = 0; i < GPU_FB_MAX_COLOR_ATTACHMENT && i < clear_cols.size(); i++) {
    color_clear_pending_[i] = true;
    clear_color_[i] = clear_cols[i];
  }
  execute_pending_clears();
}

void WebGPUFrameBuffer::clear_attachment(GPUAttachmentType type, const double4 clear_value)
{
  if (type >= GPU_FB_COLOR_ATTACHMENT0) {
    const int slot = type - GPU_FB_COLOR_ATTACHMENT0;
    if (slot < GPU_FB_MAX_COLOR_ATTACHMENT) {
      color_clear_pending_[slot] = true;
      clear_color_[slot] = clear_value;
    }
  }
  else {
    depth_clear_pending_ = true;
    clear_depth_ = float(clear_value.x);
  }
}

void WebGPUFrameBuffer::attachment_set_loadstore_op(GPUAttachmentType type, GPULoadStore ls)
{
  /* Map GPU_LOADACTION_CLEAR onto the pending-clear mechanism consumed by
   * begin_render_pass. Ignoring this leaves the depth buffer at Dawn's zero
   * init, which fails every LessEqual depth test and draws NOTHING. */
  if (ls.load_action != GPU_LOADACTION_CLEAR) {
    return;
  }
  clear_attachment(type,
                   double4(ls.clear_value[0], ls.clear_value[1], ls.clear_value[2],
                           ls.clear_value[3]));
}

void WebGPUFrameBuffer::subpass_transition_impl(
    const GPUAttachmentState /*depth_attachment_state*/,
    Span<GPUAttachmentState> color_attachment_states)
{
  /* Emulate sub-pass inputs like the GL no-extension path: READ attachments are
   * DETACHED from the framebuffer (WebGPU forbids sampling a texture that is
   * also an attachment of the active pass) and bound as sampled textures at
   * unit == attachment index — matching the hidden `gpu_subpass_img_<i>`
   * samplers emitted by the shader (see fragment_interface_declare). WRITE
   * re-attaches a previously detached attachment. */
  WebGPUContext *ctx = WebGPUContext::get();
  bool changed = false;
  for (int i : color_attachment_states.index_range()) {
    GPUAttachmentType type = GPU_FB_COLOR_ATTACHMENT0 + i;
    if (color_attachment_states[i] == GPU_ATTACHMENT_READ) {
      if (!subpass_detached_[i]) {
        subpass_detached_[i] = true;
        changed = true;
      }
      if (attachments_[type].tex != nullptr) {
        GPU_texture_bind_ex(attachments_[type].tex, GPUSamplerState::default_sampler(), i);
      }
    }
    else if (color_attachment_states[i] == GPU_ATTACHMENT_WRITE) {
      if (subpass_detached_[i]) {
        subpass_detached_[i] = false;
        changed = true;
      }
    }
  }
  if (changed && ctx != nullptr && ctx->active_fb == this) {
    /* The effective attachment set changed: the current pass must not continue. */
    ctx->render_pass_end();
  }
}

int WebGPUFrameBuffer::color_attachment_count() const
{
  if (is_attachmentless()) {
    return 1; /* The dummy attachment (see begin_render_pass). */
  }
  int count = 0;
  for (int i = 0; i < GPU_FB_MAX_COLOR_ATTACHMENT; i++) {
    if (attachments_[GPU_FB_COLOR_ATTACHMENT0 + i].tex) {
      count = i + 1;
    }
  }
  return count;
}

WGPUTextureFormat WebGPUFrameBuffer::color_format(int slot) const
{
  /* A slot currently used as an (emulated) sub-pass input is not part of the
   * effective attachment set — for the pipeline key AND the render pass. */
  if (slot >= 0 && slot < GPU_FB_MAX_COLOR_ATTACHMENT && subpass_detached_[slot]) {
    return WGPUTextureFormat_Undefined;
  }
  if (slot == 0 && is_attachmentless()) {
    return WGPUTextureFormat_R8Unorm; /* The dummy attachment. */
  }
  gpu::Texture *t = color_tex(slot);
  return t ? webgpu_texture_format(static_cast<WebGPUTexture *>(t)->format_get()) :
             WGPUTextureFormat_Undefined;
}

WGPUTextureFormat WebGPUFrameBuffer::depth_format() const
{
  gpu::Texture *t = depth_tex();
  return t ? webgpu_texture_format(static_cast<WebGPUTexture *>(t)->format_get()) :
             WGPUTextureFormat_Undefined;
}

WGPURenderPassEncoder WebGPUFrameBuffer::begin_render_pass(WGPUCommandEncoder enc)
{
  if (enc == nullptr) {
    return nullptr;
  }
  /* Keep SPARSE slots (view == null) so attachment indices line up with the
   * pipeline's color target indices — compacting shifted e.g. the prepass's
   * only attachment from slot 2 to slot 0 and made every pipeline incompatible
   * with its pass. WebGPU explicitly allows null entries in colorAttachments. */
  WGPURenderPassColorAttachment color_att[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
  const int color_count = color_attachment_count();
  int present_count = 0;
  for (int i = 0; i < color_count; i++) {
    const GPUAttachment &att = attachments_[GPU_FB_COLOR_ATTACHMENT0 + i];
    WGPURenderPassColorAttachment &ca = color_att[i];
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    if (!att.tex || subpass_detached_[i]) {
      continue; /* Null (or sub-pass-input-detached) attachment slot. */
    }
    /* WebGPU render attachments must be exactly one array layer, so use a
     * single-layer view of the attached layer/mip. Layered rendering
     * (layer == -1 on an array texture, selected via gl_Layer) has no WebGPU
     * equivalent; those passes bind layer 0. */
    WGPUTextureView view = static_cast<WebGPUTexture *>(att.tex)->wgpu_attachment_view(att.layer,
                                                                                       att.mip);
    if (!view) {
      continue;
    }
    ca.view = view;
    ca.loadOp = color_clear_pending_[i] ? WGPULoadOp_Clear : WGPULoadOp_Load;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = {clear_color_[i].x, clear_color_[i].y, clear_color_[i].z, clear_color_[i].w};
    color_clear_pending_[i] = false;
    present_count++;
  }

  WGPURenderPassDepthStencilAttachment depth_att = {};
  bool has_depth = false;
  {
    const GPUAttachment &datt = depth_attachment();
    if (datt.tex) {
      WebGPUTexture *dt = static_cast<WebGPUTexture *>(datt.tex);
      if (WGPUTextureView dview = dt->wgpu_attachment_view(datt.layer, datt.mip)) {
        depth_att.view = dview;
        depth_att.depthLoadOp = depth_clear_pending_ ? WGPULoadOp_Clear : WGPULoadOp_Load;
        depth_att.depthStoreOp = WGPUStoreOp_Store;
        depth_att.depthClearValue = clear_depth_;
        switch (dt->wgpu_format()) {
          case WGPUTextureFormat_Depth24PlusStencil8:
          case WGPUTextureFormat_Depth32FloatStencil8:
            /* Stencil aspect present: WebGPU requires its ops to be set too. */
            depth_att.stencilLoadOp = depth_clear_pending_ ? WGPULoadOp_Clear : WGPULoadOp_Load;
            depth_att.stencilStoreOp = WGPUStoreOp_Store;
            depth_att.stencilClearValue = 0;
            break;
          default:
            break;
        }
        has_depth = true;
        depth_clear_pending_ = false;
      }
    }
  }

  if (present_count == 0 && !has_depth) {
    /* Attachment-less pass (EEVEE shadow atlas raster, tag passes): WebGPU
     * requires at least one attachment, so attach a dummy R8Unorm target of the
     * framebuffer's default size (fragments write via imageStore only). */
    WebGPUContext *wctx = WebGPUContext::get();
    if (!is_attachmentless() || wctx == nullptr || wctx->device() == nullptr) {
      return nullptr;
    }
    if (dummy_att_tex_ == nullptr || dummy_w_ != width_ || dummy_h_ != height_) {
      if (dummy_att_view_) {
        wgpuTextureViewRelease(dummy_att_view_);
        dummy_att_view_ = nullptr;
      }
      if (dummy_att_tex_) {
        /* Release only — a pending command buffer may still reference it. */
        wgpuTextureRelease(dummy_att_tex_);
        dummy_att_tex_ = nullptr;
      }
      WGPUTextureDescriptor td = {};
      td.label = {"fb_dummy_attachment", WGPU_STRLEN};
      td.usage = WGPUTextureUsage_RenderAttachment;
      td.dimension = WGPUTextureDimension_2D;
      td.size = {uint32_t(width_), uint32_t(height_), 1};
      td.format = WGPUTextureFormat_R8Unorm;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      dummy_att_tex_ = wgpuDeviceCreateTexture(wctx->device(), &td);
      dummy_att_view_ = dummy_att_tex_ ? wgpuTextureCreateView(dummy_att_tex_, nullptr) : nullptr;
      dummy_w_ = width_;
      dummy_h_ = height_;
    }
    if (dummy_att_view_ == nullptr) {
      return nullptr;
    }
    color_att[0].view = dummy_att_view_;
    color_att[0].loadOp = WGPULoadOp_Clear;
    color_att[0].storeOp = WGPUStoreOp_Discard;
    color_att[0].clearValue = {0, 0, 0, 0};
    color_att[0].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    present_count = 1;
  }

  {
    static int s_rp_log = 0;
    /* ENV.WGPU_LOG_RP=1 lifts the startup-only cap (pass-structure debugging). */
    if ((s_rp_log < 300 || getenv("WGPU_LOG_RP")) &&
        (name_get()[0] != '&' || getenv("WGPU_LOG_RP"))) {
      s_rp_log++;
      fprintf(stderr,
              "WGPU_RP '%s' colors=%d depth=%d dload=%d dclear=%.2f cload0=%d "
              "cclear0=%.2f,%.2f,%.2f,%.2f sz=%dx%d vp=%d,%d,%d,%d\n",
              name_get(),
              color_count,
              int(has_depth),
              has_depth ? int(depth_att.depthLoadOp) : -1,
              has_depth ? depth_att.depthClearValue : -1.0f,
              color_count ? int(color_att[0].loadOp) : -1,
              color_count ? color_att[0].clearValue.r : -1.0,
              color_count ? color_att[0].clearValue.g : -1.0,
              color_count ? color_att[0].clearValue.b : -1.0,
              color_count ? color_att[0].clearValue.a : -1.0,
              width_,
              height_,
              viewport_[0],
              viewport_[1],
              viewport_[2],
              viewport_[3]);
      fflush(stderr);
    }
  }

  WGPURenderPassDescriptor desc = {};
  /* Occlusion query set while a select pool is active (gizmo picking). */
  if (WebGPUContext *qctx = WebGPUContext::get()) {
    desc.occlusionQuerySet = qctx->occlusion_query_set();
  }
  desc.colorAttachmentCount = color_count;
  desc.colorAttachments = color_att;
  desc.depthStencilAttachment = has_depth ? &depth_att : nullptr;
  return wgpuCommandEncoderBeginRenderPass(enc, &desc);
}

void WebGPUFrameBuffer::read(GPUFrameBufferBits planes,
                             eGPUDataFormat format,
                             const int area[4],
                             int channel_len,
                             int slot,
                             void *r_data)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx == nullptr || r_data == nullptr) {
    return;
  }
  const size_t px = size_t(area[2]) * size_t(area[3]) * size_t(std::max(channel_len, 1));

  if (planes & GPU_DEPTH_BIT) {
    /* Depth readback is not synchronously available (no JSPI). Return the FAR
     * plane everywhere — a deterministic "nothing here". Anything else (or
     * worse, leaving r_data UNINITIALIZED, as this function used to for
     * missing textures) makes the legacy depth-pick (gizmo highlight/click)
     * hallucinate hits and wedge the event loop in a bogus gizmo modal. */
    if (format == GPU_DATA_FLOAT) {
      float *out = static_cast<float *>(r_data);
      std::fill_n(out, px, 1.0f);
    }
    else {
      std::memset(r_data, 0xFF, px * (format == GPU_DATA_UINT ? 4 : 1));
    }
    return;
  }

  gpu::Texture *t = color_tex(slot);
  WebGPUTexture *wt = static_cast<WebGPUTexture *>(t);
  if (t == nullptr || wt->wgpu_texture() == nullptr) {
    /* Never leave the caller's buffer uninitialized. */
    std::memset(r_data, 0, px * (format == GPU_DATA_FLOAT || format == GPU_DATA_UINT ||
                                 format == GPU_DATA_INT ? 4 : 1));
    return;
  }
  /* Flush any pending clear by opening+closing a render pass on this framebuffer
   * (clears are recorded as loadOps and only take effect when a pass begins);
   * read_color_sync then submits and copies the final texture content. */
  ctx->render_pass_ensure(*this);
  ctx->render_pass_end();
  ctx->read_color_sync(wt->wgpu_texture(),
                       color_format(slot),
                       area[0], area[1], area[2], area[3],
                       format, channel_len, r_data);
}

void WebGPUFrameBuffer::blit_to(GPUFrameBufferBits planes,
                                int src_slot,
                                FrameBuffer *dst,
                                int dst_slot,
                                int dst_offset_x,
                                int dst_offset_y)
{
  /* GL glBlitFramebuffer without scaling → texture-to-texture copy. This is the
   * region composite path: GPU_offscreen_draw_to_screen blits each UI region's
   * offscreen (outliner, properties, ...) into the window backbuffer. Both
   * textures use our bottom-up row convention (GL emulation), so the GL offsets
   * are storage offsets directly. */
  WebGPUContext *ctx = WebGPUContext::get();
  WebGPUFrameBuffer *dst_fb = static_cast<WebGPUFrameBuffer *>(dst);
  if (ctx == nullptr || ctx->device() == nullptr || dst_fb == nullptr) {
    return;
  }
  auto copy_one = [&](gpu::Texture *src_t, gpu::Texture *dst_t, WGPUTextureAspect aspect) {
    if (src_t == nullptr || dst_t == nullptr) {
      return;
    }
    WebGPUTexture *s = static_cast<WebGPUTexture *>(src_t);
    WebGPUTexture *d = static_cast<WebGPUTexture *>(dst_t);
    if (s->wgpu_texture() == nullptr || d->wgpu_texture() == nullptr ||
        s->wgpu_format() != d->wgpu_format())
    {
      static int s_logged = 0;
      if (s_logged < 8) {
        s_logged++;
        fprintf(stderr,
                "WGPU_FB blit_to skipped: fmt src=%d dst=%d\n",
                s ? int(s->wgpu_format()) : -1,
                d ? int(d->wgpu_format()) : -1);
        fflush(stderr);
      }
      return;
    }
    const int w = std::min(s->width_get(), d->width_get() - dst_offset_x);
    const int h = std::min(s->height_get(), d->height_get() - dst_offset_y);
    if (w <= 0 || h <= 0 || dst_offset_x < 0 || dst_offset_y < 0) {
      return;
    }
    /* Consume pending clears on BOTH sides first (they are loadOps applied at
     * pass begin; a copy would otherwise read/write pre-clear content). */
    execute_pending_clears();
    dst_fb->execute_pending_clears();
    ctx->render_pass_end();
    WGPUCommandEncoder enc = ctx->ensure_encoder();
    if (enc == nullptr) {
      return;
    }
    WGPUTexelCopyTextureInfo src_i = {};
    src_i.texture = s->wgpu_texture();
    src_i.aspect = aspect;
    WGPUTexelCopyTextureInfo dst_i = {};
    dst_i.texture = d->wgpu_texture();
    dst_i.aspect = aspect;
    dst_i.origin = {uint32_t(dst_offset_x), uint32_t(dst_offset_y), 0};
    WGPUExtent3D ext = {uint32_t(w), uint32_t(h), 1};
    wgpuCommandEncoderCopyTextureToTexture(enc, &src_i, &dst_i, &ext);
  };
  if (planes & GPU_COLOR_BIT) {
    copy_one(color_tex(src_slot), dst_fb->color_tex(dst_slot), WGPUTextureAspect_All);
  }
  if (planes & GPU_DEPTH_BIT) {
    copy_one(depth_tex(), dst_fb->depth_tex(), WGPUTextureAspect_All);
  }
}

}  // namespace blender::gpu
