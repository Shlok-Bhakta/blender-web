/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU texture. Creates a real WGPUTexture (+ default view) when a device is
 * present; stays a metadata-only object in the device-less headless/diagnostic
 * mode (node without a browser device). Format/usage/dimension are mapped from
 * Blender's TextureFormat / eGPUTextureUsage / GPUTextureType.
 */

#pragma once

#include "gpu_texture_private.hh"

#include <map>

#include <webgpu/webgpu.h>

namespace blender::gpu {

class WebGPUTexture : public Texture {
 private:
  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  WGPUTextureFormat wgpu_format_ = WGPUTextureFormat_RGBA8Unorm;
  /* Single-layer/mip views for render attachments (key: layer<<8 | mip) and the
   * depth-only view used when a depth-stencil texture is bound for sampling
   * (WebGPU forbids multi-aspect views in texture bindings). */
  std::map<uint32_t, WGPUTextureView> attachment_views_;
  WGPUTextureView sample_view_ = nullptr;
  /* Sampling mip window (GPU_texture_mip_range_set): Blender clamps sampling
   * to the mips it has actually uploaded/generated. Ignoring it samples
   * Dawn-zero-initialized mips (black blended in). */
  int mip_min_ = 0;
  int mip_max_ = -1; /* -1 = full chain. */
  WGPUTextureView mip_range_view_ = nullptr;
  WGPUTextureView storage_view_ = nullptr;
  /* Set when this texture is a view onto another texture's WGPUTexture (which
   * it then only references, never destroys). Reads/attachments offset into the
   * shared texture by view_layer_/view_mip_. */
  bool is_view_ = false;
  int view_layer_ = 0;
  int view_mip_ = 0;
  long long alloc_bytes_ = 0;

 public:
  WebGPUTexture(const char *name);
  ~WebGPUTexture() override;

  void update_sub(int mip,
                  int offset[3],
                  int extent[3],
                  eGPUDataFormat format,
                  const void *data,
                  uint unpack_row_length = 0) override;
  void update_sub(int /*offset*/[3],
                  int /*extent*/[3],
                  eGPUDataFormat /*format*/,
                  GPUPixelBuffer * /*pixbuf*/) override
  {
  }

  void generate_mipmap() override;
  void copy_to(Texture *dst, IndexRange mip_levels) override;
  void clear(const double4 data) override;
  void swizzle_set(const char /*swizzle_mask*/[4]) override {}
  void mip_range_set(int min, int max) override;
  void read(int mip, eGPUDataFormat format, void *dst) override;

  /* Debug probes (WGPU_SUM_TEX) match live textures by name. */
  const std::string &debug_name() const
  {
    return name_;
  }
  void debug_rename(const char *name)
  {
    name_ = name;
  }
  WGPUTexture wgpu_texture() const
  {
    return texture_;
  }
  int view_mip() const
  {
    return view_mip_;
  }
  int view_layer() const
  {
    return view_layer_;
  }
  bool is_view() const
  {
    return is_view_;
  }
  /* True when this texture's CREATED WGPUTextureUsage covers what `req` needs.
   * usage_set() after init cannot widen the wgpu-side usage, so pool reuse
   * must check against creation flags: handing a non-RenderAttachment texture
   * to a framebuffer invalidates the whole command buffer at submit (every
   * later pass in it is silently dropped). */
  bool wgpu_usage_covers(eGPUTextureUsage req) const;
  WGPUTextureView wgpu_view() const
  {
    return view_;
  }
  /* View for use as a render-pass attachment: a single mip of a single layer
   * (WebGPU render attachments must have exactly one layer). layer < 0 = 0. */
  WGPUTextureView wgpu_attachment_view(int layer, int mip);
  /* View for use in a sampled-texture binding: depth-only aspect for
   * depth-stencil formats, the default whole-texture view otherwise. */
  WGPUTextureView wgpu_sample_view();
  /* Create a view of `other` (same size/format/mip layout as this texture's
   * base) using THIS texture object's sampled-view window (mip/layer offsets,
   * dimension). Used to sample snapshot copies. Caller owns the view. */
  WGPUTextureView make_view_of(WGPUTexture other);
  /* View for use in a storage-texture binding: WebGPU requires exactly one mip
   * level there (the default view spans all mips). All layers included. */
  WGPUTextureView wgpu_storage_view();
  bool is_depth_format() const
  {
    switch (wgpu_format_) {
      case WGPUTextureFormat_Depth16Unorm:
      case WGPUTextureFormat_Depth24Plus:
      case WGPUTextureFormat_Depth24PlusStencil8:
      case WGPUTextureFormat_Depth32Float:
      case WGPUTextureFormat_Depth32FloatStencil8:
        return true;
      default:
        return false;
    }
  }
  WGPUTextureFormat wgpu_format() const
  {
    return wgpu_format_;
  }

 protected:
  bool init_internal() override;
  bool init_internal(VertBuf *vbo) override;
  bool init_internal(gpu::Texture *src,
                     int mip_offset,
                     int layer_offset,
                     bool use_stencil) override;
};

WGPUTextureFormat webgpu_texture_format(TextureFormat format);
uint32_t webgpu_format_bytes_per_pixel(WGPUTextureFormat f);

}  // namespace blender::gpu
