/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "BLI_math_half.hh"
#include <cstdlib>

#include "webgpu_context.hh"
#include "webgpu_texture.hh"

namespace blender::gpu {

static WebGPUContext *ctx_get()
{
  return static_cast<WebGPUContext *>(Context::get());
}

/* Map Blender's logical texture format to the closest WebGPU format. Covers the
 * formats EEVEE/Workbench commonly use; unmapped formats fall back to RGBA8 so
 * texture creation never hard-fails (correctness for exotic formats TBD). */
WGPUTextureFormat webgpu_texture_format(TextureFormat format)
{
  switch (format) {
    case TextureFormat::UNORM_8_8_8_8:
      return WGPUTextureFormat_RGBA8Unorm;
    case TextureFormat::SRGBA_8_8_8_8:
      return WGPUTextureFormat_RGBA8UnormSrgb;
    case TextureFormat::UNORM_8:
      return WGPUTextureFormat_R8Unorm;
    case TextureFormat::UNORM_8_8:
      return WGPUTextureFormat_RG8Unorm;
    case TextureFormat::UINT_8:
      return WGPUTextureFormat_R8Uint;
    case TextureFormat::SFLOAT_16:
      return WGPUTextureFormat_R16Float;
    case TextureFormat::SFLOAT_16_16:
      return WGPUTextureFormat_RG16Float;
    case TextureFormat::SFLOAT_16_16_16:
      /* No RGB formats in WebGPU: pad to RGBA (uploads expand, alpha = 1).
       * MISSING THIS mapped OCIO's AgX 3D LUT to the default RGBA8Unorm with a
       * misaligned upload — every AgX/Filmic view transform showed false-color
       * garbage. */
    case TextureFormat::SFLOAT_16_16_16_16:
      return WGPUTextureFormat_RGBA16Float;
    case TextureFormat::SFLOAT_32:
      return WGPUTextureFormat_R32Float;
    case TextureFormat::SFLOAT_32_32:
      return WGPUTextureFormat_RG32Float;
    case TextureFormat::SFLOAT_32_32_32:
    case TextureFormat::SFLOAT_32_32_32_32:
      return WGPUTextureFormat_RGBA32Float;
    case TextureFormat::UINT_32:
      return WGPUTextureFormat_R32Uint;
    case TextureFormat::UINT_32_32:
      return WGPUTextureFormat_RG32Uint;
    case TextureFormat::UINT_32_32_32_32:
      return WGPUTextureFormat_RGBA32Uint;
    case TextureFormat::SINT_32:
      return WGPUTextureFormat_R32Sint;
    case TextureFormat::UINT_16:
      return WGPUTextureFormat_R16Uint;
    case TextureFormat::UINT_16_16_16_16:
      return WGPUTextureFormat_RGBA16Uint;
    case TextureFormat::UNORM_10_10_10_2:
      return WGPUTextureFormat_RGB10A2Unorm;
    case TextureFormat::UFLOAT_11_11_10:
      return WGPUTextureFormat_RG11B10Ufloat;
    case TextureFormat::UNORM_16_DEPTH:
      return WGPUTextureFormat_Depth16Unorm;
    case TextureFormat::SFLOAT_32_DEPTH:
      return WGPUTextureFormat_Depth32Float;
    case TextureFormat::SFLOAT_32_DEPTH_UINT_8:
      return WGPUTextureFormat_Depth32FloatStencil8;
    default:
      return WGPUTextureFormat_RGBA8Unorm;
  }
}

/* WebGPU only permits STORAGE_BINDING on a fixed set of formats. EEVEE liberally
 * tags textures SHADER_WRITE in formats outside the base set (R16Float,
 * RG11B10Ufloat, 8/16-bit int, rgb10a2, ...). Those become storage-capable when
 * the device has `texture-formats-tier1` (requested by the JS harness when the
 * adapter exposes it), so gate on the device feature: with tier1 the whole EEVEE
 * set gets StorageBinding; without it fall back to the base list so texture
 * creation at least succeeds as sampled/attachment. */
static bool webgpu_format_supports_storage(WGPUTextureFormat f, WGPUDevice device)
{
  static int tier1 = -1;
  if (tier1 < 0 && device != nullptr) {
    tier1 = wgpuDeviceHasFeature(device, WGPUFeatureName_TextureFormatsTier1) ? 1 : 0;
    fprintf(stderr, "WGPU_TEX texture-formats-tier1=%d\n", tier1);
    fflush(stderr);
  }
  if (tier1 == 1) {
    switch (f) {
      case WGPUTextureFormat_R8Unorm:
      case WGPUTextureFormat_R8Snorm:
      case WGPUTextureFormat_R8Uint:
      case WGPUTextureFormat_R8Sint:
      case WGPUTextureFormat_RG8Unorm:
      case WGPUTextureFormat_RG8Snorm:
      case WGPUTextureFormat_RG8Uint:
      case WGPUTextureFormat_RG8Sint:
      case WGPUTextureFormat_R16Uint:
      case WGPUTextureFormat_R16Sint:
      case WGPUTextureFormat_R16Float:
      case WGPUTextureFormat_RG16Uint:
      case WGPUTextureFormat_RG16Sint:
      case WGPUTextureFormat_RG16Float:
      case WGPUTextureFormat_RGB10A2Uint:
      case WGPUTextureFormat_RGB10A2Unorm:
      case WGPUTextureFormat_RG11B10Ufloat:
        return true;
      default:
        break;
    }
  }
  switch (f) {
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_R32Float:
    case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8Snorm:
    case WGPUTextureFormat_RGBA8Uint:
    case WGPUTextureFormat_RGBA8Sint:
    case WGPUTextureFormat_RGBA16Uint:
    case WGPUTextureFormat_RGBA16Sint:
    case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGBA32Sint:
    case WGPUTextureFormat_RGBA32Float:
      return true;
    default:
      return false;
  }
}

static WGPUTextureUsage webgpu_texture_usage(eGPUTextureUsage usage,
                                             WGPUTextureFormat format,
                                             WGPUDevice device)
{
  WGPUTextureUsage flags = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
  if (usage & GPU_TEXTURE_USAGE_SHADER_READ) {
    flags |= WGPUTextureUsage_TextureBinding;
  }
  if ((usage & GPU_TEXTURE_USAGE_SHADER_WRITE) && webgpu_format_supports_storage(format, device)) {
    flags |= WGPUTextureUsage_StorageBinding;
  }
  if (usage & GPU_TEXTURE_USAGE_ATTACHMENT) {
    flags |= WGPUTextureUsage_RenderAttachment;
  }
  return flags;
}

bool WebGPUTexture::wgpu_usage_covers(eGPUTextureUsage req) const
{
  if (texture_ == nullptr) {
    return true;
  }
  WebGPUContext *ctx = WebGPUContext::get();
  WGPUDevice dev = ctx ? ctx->device() : nullptr;
  const WGPUTextureUsage need = webgpu_texture_usage(
      req, wgpuTextureGetFormat(texture_), dev);
  return (wgpuTextureGetUsage(texture_) & need) == need;
}

/* Bytes per texel for the WebGPU formats produced by webgpu_texture_format().
 * Used to size queue writes (bytesPerRow). Defaults to 4 for unmapped formats. */
uint32_t webgpu_format_bytes_per_pixel(WGPUTextureFormat f)
{
  switch (f) {
    case WGPUTextureFormat_R8Unorm:
    case WGPUTextureFormat_R8Uint:
      return 1;
    case WGPUTextureFormat_RG8Unorm:
    case WGPUTextureFormat_R16Float:
    case WGPUTextureFormat_R16Uint:
    case WGPUTextureFormat_Depth16Unorm:
      return 2;
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8UnormSrgb:
    case WGPUTextureFormat_RG16Float:
    case WGPUTextureFormat_R32Float:
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RGB10A2Unorm:
    case WGPUTextureFormat_RG11B10Ufloat:
    case WGPUTextureFormat_Depth32Float:
      return 4;
    case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_RGBA16Uint:
    case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_RG32Uint:
      return 8;
    case WGPUTextureFormat_RGBA32Float:
    case WGPUTextureFormat_RGBA32Uint:
      return 16;
    default:
      return 4;
  }
}

/* DEBUG registry so env-gated probes can find a live texture by name
 * (WGPU_SUM_TEX). Single GPU thread — no locking. */
std::vector<WebGPUTexture *> g_wgpu_live_textures;

WebGPUTexture::WebGPUTexture(const char *name) : Texture(name)
{
  if (name && name_.empty()) {
    /* Upstream only keeps names under G_DEBUG_GPU; keep them always — debug
     * probes match by name and labels make validation errors attributable. */
    name_ = name;
  }
  g_wgpu_live_textures.push_back(this);
}

WebGPUTexture::~WebGPUTexture()
{
  g_wgpu_live_textures.erase(
      std::remove(g_wgpu_live_textures.begin(), g_wgpu_live_textures.end(), this),
      g_wgpu_live_textures.end());
  WebGPUContext::scrub_texture_all_contexts(this);
  for (auto &kv : attachment_views_) {
    if (kv.second) {
      wgpuTextureViewRelease(kv.second);
    }
  }
  if (sample_view_) {
    wgpuTextureViewRelease(sample_view_);
  }
  if (mip_range_view_) {
    wgpuTextureViewRelease(mip_range_view_);
  }
  if (storage_view_) {
    wgpuTextureViewRelease(storage_view_);
  }
  if (view_) {
    wgpuTextureViewRelease(view_);
  }
  {
    extern long long g_stat_tex_bytes;
    extern int g_stat_tex_count;
    g_stat_tex_bytes -= alloc_bytes_;
    if (alloc_bytes_ > 0) {
      g_stat_tex_count--;
    }
  }
  if (texture_) {
    /* Release only — NEVER wgpuTextureDestroy. Blender frees textures that
     * pending (unsubmitted) command buffers still reference (legal in GL);
     * Destroy would poison the whole submit ("Destroyed texture used in a
     * submit"), dropping every draw in the frame. Dropping the last ref lets
     * Dawn reclaim the memory once queued work completes. */
    wgpuTextureRelease(texture_);
  }
}

bool WebGPUTexture::init_internal()
{
  wgpu_format_ = webgpu_texture_format(format_);

  WebGPUContext *ctx = ctx_get();
  if (ctx == nullptr || ctx->device() == nullptr) {
    /* Device-less (headless diagnostics): metadata-only, succeed so the higher
     * layers proceed. Real GPU resource is created when a device is present. */
    return true;
  }

  WGPUTextureDescriptor desc = {};
  /* 1D (and 1D-array) textures are PROMOTED to 2D with height 1: WGSL has no
   * 1D array textures at all, and Tint's SPIR-V reader mis-types plain 1D
   * textureDimensions (u32 vs vec2<u32>). The GLSL is patched to match
   * (webgpu_shader.cc promote_1d_samplers). Blender stores 1D-array layer
   * counts in h_, which promotion moves to the array-layer axis. */
  desc.dimension = (type_ & GPU_TEXTURE_3D) ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
  desc.size.width = std::max(w_, 1);
  desc.size.height = (type_ & GPU_TEXTURE_1D) ? 1 : std::max(h_, 1);
  /* depthOrArrayLayers: 3D depth, or array layer count, min 1. */
  desc.size.depthOrArrayLayers = (type_ & GPU_TEXTURE_1D) ?
                                     ((type_ & GPU_TEXTURE_ARRAY) ? std::max(h_, 1) : 1) :
                                     std::max(d_, 1);
  desc.format = wgpu_format_;
  desc.mipLevelCount = std::max(mipmaps_, 1);
  desc.sampleCount = 1;
  desc.usage = webgpu_texture_usage(gpu_image_usage_flags_, wgpu_format_, ctx->device());
  /* Mipmaps are generated with a blit render pass (compute mipgen is disabled
   * on this backend), so every mip level becomes a color attachment. Image
   * textures request SHADER_READ only — without this, mipgen poisons the whole
   * frame's command buffer ("usage doesn't include RenderAttachment"). */
  if (desc.mipLevelCount > 1 && !is_depth_format()) {
    switch (wgpu_format_) {
      case WGPUTextureFormat_RGBA8Unorm:
      case WGPUTextureFormat_RGBA8UnormSrgb:
      case WGPUTextureFormat_R8Unorm:
      case WGPUTextureFormat_RG8Unorm:
      case WGPUTextureFormat_RGBA16Float:
      case WGPUTextureFormat_RG16Float:
      case WGPUTextureFormat_R16Float:
      case WGPUTextureFormat_RGBA32Float:
      case WGPUTextureFormat_RG32Float:
      case WGPUTextureFormat_R32Float:
      case WGPUTextureFormat_RGB10A2Unorm:
      case WGPUTextureFormat_RG11B10Ufloat:
        desc.usage |= WGPUTextureUsage_RenderAttachment;
        break;
      default:
        break;
    }
  }

  texture_ = wgpuDeviceCreateTexture(ctx->device(), &desc);
  if (getenv("WGPU_LOG_TEXCREATE")) {
    fprintf(stderr, "WGPU_TEXCREATE '%s' %ux%ux%u fmt=%d usage=0x%x mips=%u\n",
            name_.c_str(), desc.size.width, desc.size.height, desc.size.depthOrArrayLayers,
            int(desc.format), unsigned(desc.usage), desc.mipLevelCount);
    fflush(stderr);
  }
  if (texture_ == nullptr) {
    return false;
  }
  {
    /* Rough VRAM accounting for leak hunting (WGPU_STATS). */
    extern long long g_stat_tex_bytes;
    extern int g_stat_tex_count;
    const uint32_t bpp = webgpu_format_bytes_per_pixel(wgpu_format_);
    alloc_bytes_ = (long long)desc.size.width * desc.size.height *
                   desc.size.depthOrArrayLayers * bpp;
    if (desc.mipLevelCount > 1) {
      alloc_bytes_ += alloc_bytes_ / 2; /* mip chain approx. */
    }
    g_stat_tex_bytes += alloc_bytes_;
    g_stat_tex_count++;
  }
  WGPUTextureViewDescriptor vdesc = {};
  vdesc.format = wgpu_format_;
  vdesc.dimension = (desc.dimension == WGPUTextureDimension_3D) ? WGPUTextureViewDimension_3D :
                    (type_ & GPU_TEXTURE_CUBE)                  ? WGPUTextureViewDimension_Cube :
                    (type_ & GPU_TEXTURE_ARRAY) ? WGPUTextureViewDimension_2DArray :
                                                  WGPUTextureViewDimension_2D;
  vdesc.baseMipLevel = 0;
  vdesc.mipLevelCount = desc.mipLevelCount;
  vdesc.baseArrayLayer = 0;
  vdesc.arrayLayerCount = (desc.dimension == WGPUTextureDimension_2D) ?
                              desc.size.depthOrArrayLayers :
                              1;
  view_ = wgpuTextureCreateView(texture_, &vdesc);
  return true;
}

static bool webgpu_format_has_stencil(WGPUTextureFormat f)
{
  return f == WGPUTextureFormat_Depth24PlusStencil8 ||
         f == WGPUTextureFormat_Depth32FloatStencil8;
}

WGPUTextureView WebGPUTexture::wgpu_attachment_view(int layer, int mip)
{
  if (texture_ == nullptr) {
    return nullptr;
  }
  const uint32_t l = uint32_t(std::max(layer, 0) + view_layer_);
  const uint32_t m = uint32_t(std::max(mip, 0) + view_mip_);
  const uint32_t key = (l << 8) | m;
  auto it = attachment_views_.find(key);
  if (it != attachment_views_.end()) {
    return it->second;
  }
  WGPUTextureViewDescriptor vd = {};
  vd.format = wgpu_format_;
  vd.dimension = WGPUTextureViewDimension_2D;
  vd.baseMipLevel = m;
  vd.mipLevelCount = 1;
  vd.baseArrayLayer = l;
  vd.arrayLayerCount = 1;
  WGPUTextureView v = wgpuTextureCreateView(texture_, &vd);
  attachment_views_[key] = v;
  return v;
}

void WebGPUTexture::mip_range_set(int min, int max)
{
  min = std::max(min, 0);
  if (min == mip_min_ && max == mip_max_) {
    return;
  }
  mip_min_ = min;
  mip_max_ = max;
  if (mip_range_view_) {
    wgpuTextureViewRelease(mip_range_view_);
    mip_range_view_ = nullptr;
  }
}

WGPUTextureView WebGPUTexture::wgpu_sample_view()
{
  if (!webgpu_format_has_stencil(wgpu_format_)) {
    /* Honor the sampling mip window: Blender clamps to the mips it actually
       uploaded/generated; the default view spans the full chain and samples
       Dawn-zero-initialized levels (black blended in — file byte textures
       rendered dark/muddy: tex_srgb_file scene). */
    const int total = std::max(mipmaps_, 1);
    const int lo = std::min(mip_min_, total - 1);
    const int hi = (mip_max_ < 0) ? total - 1 : std::min(mip_max_, total - 1);
    if (lo == 0 && hi == total - 1) {
      return view_;
    }
    if (mip_range_view_ == nullptr && texture_ != nullptr) {
      WGPUTextureViewDescriptor vd = {};
      vd.format = wgpu_format_;
      vd.dimension = (type_ & GPU_TEXTURE_3D)    ? WGPUTextureViewDimension_3D :
                     (type_ & GPU_TEXTURE_CUBE)  ? WGPUTextureViewDimension_Cube :
                     (type_ & GPU_TEXTURE_ARRAY) ? WGPUTextureViewDimension_2DArray :
                                                   WGPUTextureViewDimension_2D;
      vd.baseMipLevel = uint32_t(view_mip_ + lo);
      vd.mipLevelCount = uint32_t(hi - lo + 1);
      vd.baseArrayLayer = uint32_t(view_layer_);
      vd.arrayLayerCount = (vd.dimension == WGPUTextureViewDimension_2DArray) ?
                               uint32_t(std::max((type_ & GPU_TEXTURE_1D) ? h_ : d_, 1)) :
                           (vd.dimension == WGPUTextureViewDimension_Cube) ? 6u : 1u;
      mip_range_view_ = wgpuTextureCreateView(texture_, &vd);
    }
    return mip_range_view_ ? mip_range_view_ : view_;
  }
  if (sample_view_ == nullptr && texture_ != nullptr) {
    WGPUTextureViewDescriptor vd = {};
    /* A single-aspect view's format is the aspect's format, not the combined
     * depth-stencil format. */
    vd.format = (wgpu_format_ == WGPUTextureFormat_Depth32FloatStencil8) ?
                    WGPUTextureFormat_Depth32Float :
                    WGPUTextureFormat_Depth24Plus;
    vd.dimension = (type_ & GPU_TEXTURE_ARRAY) ? WGPUTextureViewDimension_2DArray :
                                                 WGPUTextureViewDimension_2D;
    vd.baseMipLevel = 0;
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = (type_ & GPU_TEXTURE_ARRAY) ? uint32_t(std::max(d_, 1)) : 1;
    vd.aspect = WGPUTextureAspect_DepthOnly;
    sample_view_ = wgpuTextureCreateView(texture_, &vd);
  }
  return sample_view_;
}

WGPUTextureView WebGPUTexture::make_view_of(WGPUTexture other)
{
  if (other == nullptr) {
    return nullptr;
  }
  WGPUTextureViewDescriptor vd = {};
  vd.format = wgpu_format_;
  vd.dimension = (type_ & GPU_TEXTURE_3D)    ? WGPUTextureViewDimension_3D :
                 (type_ & GPU_TEXTURE_CUBE)  ? WGPUTextureViewDimension_Cube :
                 (type_ & GPU_TEXTURE_ARRAY) ? WGPUTextureViewDimension_2DArray :
                                               WGPUTextureViewDimension_2D;
  vd.baseMipLevel = uint32_t(view_mip_);
  vd.mipLevelCount = uint32_t(std::max(mipmaps_, 1));
  vd.baseArrayLayer = uint32_t(view_layer_);
  vd.arrayLayerCount = (vd.dimension == WGPUTextureViewDimension_2DArray) ?
                           uint32_t(std::max((type_ & GPU_TEXTURE_1D) ? h_ : d_, 1)) :
                       (vd.dimension == WGPUTextureViewDimension_Cube) ? 6u : 1u;
  return wgpuTextureCreateView(other, &vd);
}

WGPUTextureView WebGPUTexture::wgpu_storage_view()
{
  if (mipmaps_ <= 1 && !is_view_) {
    return view_; /* Default view already spans exactly one mip. */
  }
  if (storage_view_ == nullptr && texture_ != nullptr) {
    WGPUTextureViewDescriptor vd = {};
    vd.format = wgpu_format_;
    vd.dimension = (type_ & GPU_TEXTURE_3D)    ? WGPUTextureViewDimension_3D :
                   (type_ & GPU_TEXTURE_ARRAY) ? WGPUTextureViewDimension_2DArray :
                                                 WGPUTextureViewDimension_2D;
    vd.baseMipLevel = uint32_t(view_mip_);
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = uint32_t(view_layer_);
    vd.arrayLayerCount = (vd.dimension == WGPUTextureViewDimension_2DArray) ?
                             uint32_t(std::max((type_ & GPU_TEXTURE_1D) ? h_ : d_, 1)) :
                             1;
    storage_view_ = wgpuTextureCreateView(texture_, &vd);
  }
  return storage_view_;
}

bool WebGPUTexture::init_internal(VertBuf * /*vbo*/)
{
  /* Texture-from-buffer not implemented yet. */
  wgpu_format_ = webgpu_texture_format(format_);
  return true;
}

bool WebGPUTexture::init_internal(gpu::Texture *src,
                                  int mip_offset,
                                  int layer_offset,
                                  bool /*use_stencil*/)
{
  /* Texture view: share the source's WGPUTexture, expose a WGPUTextureView
   * covering this view's layer/mip window. Film::read_pass reads per-layer
   * views of the accumulation textures, so reads must also work (read() offsets
   * the copy origin by view_layer_). */
  wgpu_format_ = webgpu_texture_format(format_);
  WebGPUTexture *s = static_cast<WebGPUTexture *>(src);
  if (s == nullptr || s->texture_ == nullptr) {
    /* Device-less / source not materialized: metadata-only, like init_internal. */
    return true;
  }
  texture_ = s->texture_;
  wgpuTextureAddRef(texture_);
  is_view_ = true;
  view_layer_ = std::max(layer_offset, 0);
  view_mip_ = std::max(mip_offset, 0);

  WGPUTextureViewDescriptor vd = {};
  vd.format = wgpu_format_;
  vd.dimension = (type_ & GPU_TEXTURE_3D)    ? WGPUTextureViewDimension_3D :
                 (type_ & GPU_TEXTURE_CUBE)  ? WGPUTextureViewDimension_Cube :
                 (type_ & GPU_TEXTURE_ARRAY) ? WGPUTextureViewDimension_2DArray :
                                               WGPUTextureViewDimension_2D;
  vd.baseMipLevel = uint32_t(view_mip_);
  vd.mipLevelCount = uint32_t(std::max(mipmaps_, 1));
  vd.baseArrayLayer = uint32_t(view_layer_);
  vd.arrayLayerCount = (vd.dimension == WGPUTextureViewDimension_2DArray) ?
                           uint32_t(std::max((type_ & GPU_TEXTURE_1D) ? h_ : d_, 1)) :
                       (vd.dimension == WGPUTextureViewDimension_Cube) ? 6u : 1u;
  view_ = wgpuTextureCreateView(texture_, &vd);
  return view_ != nullptr;
}

void WebGPUTexture::update_sub(int mip,
                               int offset[3],
                               int extent[3],
                               eGPUDataFormat format,
                               const void *data,
                               uint unpack_row_length)
{
  WebGPUContext *ctx = ctx_get();
  if (!texture_ || !ctx || !ctx->queue() || !data) {
    return;
  }
  if (is_depth_format()) {
    /* WebGPU forbids writing the depth aspect via WriteTexture. */
    static int s_depth_up = 0;
    if (s_depth_up++ < 4) {
      fprintf(stderr, "WGPU_TEX update_sub '%s': depth upload skipped\n", name_.c_str());
      fflush(stderr);
    }
    return;
  }
  /* WriteTexture executes before later submits — don't let it retroactively
   * change what an in-recording pass reads (same hazard as WriteBuffer). */
  ctx->flush_if_pass_open("tex");

  if (getenv("WGPU_LOG_TEXUP")) {
    fprintf(stderr,
            "WGPU_TEXUP '%s' tex=%dx%d up=%dx%dx%d off=%d,%d,%d mip=%d fmt=%d data_fmt=%d "
            "rowlen=%u\n",
            name_.c_str(),
            w_, h_,
            extent[0], extent[1], extent[2],
            offset[0], offset[1], offset[2],
            mip, int(wgpu_format_), int(format), unpack_row_length);
    fflush(stderr);
  }

  const uint32_t bpp = webgpu_format_bytes_per_pixel(wgpu_format_);
  const uint32_t w = uint32_t(std::max(extent[0], 1));
  const uint32_t h = uint32_t(std::max(extent[1], 1));
  const uint32_t d = uint32_t(std::max(extent[2], 1));
  /* `format` is the SOURCE texel encoding, which need not match the texture's
   * texel size (e.g. EEVEE's utility LUT/blue-noise tables upload float32 data
   * into an RGBA16F array). Raw-copying mismatched data floods the texture with
   * garbage — every stochastic decision downstream (gbuffer closure packing,
   * light/shadow sampling) then peppers per-pixel. Convert/repack when needed. */
  const size_t src_px = to_bytesize(format_, format);
  const uint32_t src_row_px = (unpack_row_length != 0) ? unpack_row_length : w;

  const void *upload = data;
  void *tmp = nullptr;
  if (src_px != bpp || src_row_px != w) {
    const bool needs_convert = (src_px != bpp);
    enum { CONV_NONE, CONV_F16, CONV_F32, CONV_11_11_10, CONV_10_10_10_2, CONV_UNORM8 } conv =
        CONV_NONE;
    if (needs_convert && format == GPU_DATA_FLOAT) {
      switch (wgpu_format_) {
        case WGPUTextureFormat_RGBA16Float:
        case WGPUTextureFormat_RG16Float:
        case WGPUTextureFormat_R16Float:
          conv = CONV_F16;
          break;
        case WGPUTextureFormat_RGBA32Float:
          conv = CONV_F32; /* Channel expansion only (RGB32F source). */
          break;
        case WGPUTextureFormat_RG11B10Ufloat:
          conv = CONV_11_11_10;
          break;
        case WGPUTextureFormat_RGB10A2Unorm:
          conv = CONV_10_10_10_2;
          break;
        case WGPUTextureFormat_RGBA8Unorm:
        case WGPUTextureFormat_RGBA8UnormSrgb:
        case WGPUTextureFormat_RG8Unorm:
        case WGPUTextureFormat_R8Unorm:
          conv = CONV_UNORM8;
          break;
        default:
          break;
      }
    }
    if (needs_convert && conv == CONV_NONE) {
      static int s_conv_log = 0;
      if (s_conv_log < 20) {
        s_conv_log++;
        fprintf(stderr,
                "WGPU_TEX update_sub '%s': unsupported conversion data_fmt=%d src_px=%zu "
                "tex_bpp=%u wgpu_fmt=%d (upload skipped)\n",
                name_.c_str(),
                int(format),
                src_px,
                bpp,
                int(wgpu_format_));
        fflush(stderr);
      }
      return;
    }
    /* Positive-only small unsigned floats: drop sign, truncate half mantissa. */
    auto to_uf11 = [](float v) -> uint32_t {
      uint16_t half = math::float_to_half(v > 0.0f ? v : 0.0f);
      return uint32_t(half >> 4) & 0x7FFu;
    };
    auto to_uf10 = [](float v) -> uint32_t {
      uint16_t half = math::float_to_half(v > 0.0f ? v : 0.0f);
      return uint32_t(half >> 5) & 0x3FFu;
    };
    auto to_unorm = [](float v, uint32_t max) -> uint32_t {
      v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      return uint32_t(v * float(max) + 0.5f);
    };
    tmp = malloc(size_t(w) * h * d * bpp);
    const uint8_t *src_bytes = static_cast<const uint8_t *>(data);
    for (uint32_t row = 0; row < h * d; row++) {
      const uint8_t *src_row = src_bytes + size_t(row) * src_row_px * src_px;
      uint8_t *dst_row = static_cast<uint8_t *>(tmp) + size_t(row) * w * bpp;
      const uint32_t src_ch = uint32_t(src_px / 4); /* float32 components */
      switch (conv) {
        case CONV_F16: {
          const uint32_t dst_ch = bpp / 2;
          if (src_ch == dst_ch) {
            math::float_to_half_array(reinterpret_cast<const float *>(src_row),
                                      reinterpret_cast<uint16_t *>(dst_row),
                                      size_t(w) * dst_ch);
          }
          else {
            /* Channel expansion (RGB source into padded RGBA texture — the
             * OCIO LUTs): convert per texel, pad alpha with 1. */
            const float *s = reinterpret_cast<const float *>(src_row);
            uint16_t *d = reinterpret_cast<uint16_t *>(dst_row);
            for (uint32_t x = 0; x < w; x++) {
              for (uint32_t c = 0; c < dst_ch; c++) {
                const float v = (c < src_ch) ? s[x * src_ch + c] : (c == 3 ? 1.0f : 0.0f);
                d[x * dst_ch + c] = math::float_to_half(v);
              }
            }
          }
          break;
        }
        case CONV_F32: {
          /* Float32 target with channel expansion. */
          const uint32_t dst_ch = bpp / 4;
          const float *s = reinterpret_cast<const float *>(src_row);
          float *d = reinterpret_cast<float *>(dst_row);
          for (uint32_t x = 0; x < w; x++) {
            for (uint32_t c = 0; c < dst_ch; c++) {
              d[x * dst_ch + c] = (c < src_ch) ? s[x * src_ch + c] : (c == 3 ? 1.0f : 0.0f);
            }
          }
          break;
        }
        case CONV_11_11_10: {
          const float *s = reinterpret_cast<const float *>(src_row);
          uint32_t *dw = reinterpret_cast<uint32_t *>(dst_row);
          for (uint32_t x = 0; x < w; x++, s += 3) {
            dw[x] = to_uf11(s[0]) | (to_uf11(s[1]) << 11) | (to_uf10(s[2]) << 22);
          }
          break;
        }
        case CONV_10_10_10_2: {
          const float *s = reinterpret_cast<const float *>(src_row);
          uint32_t *dw = reinterpret_cast<uint32_t *>(dst_row);
          for (uint32_t x = 0; x < w; x++, s += 4) {
            dw[x] = to_unorm(s[0], 1023) | (to_unorm(s[1], 1023) << 10) |
                    (to_unorm(s[2], 1023) << 20) | (to_unorm(s[3], 3) << 30);
          }
          break;
        }
        case CONV_UNORM8: {
          const float *s = reinterpret_cast<const float *>(src_row);
          const uint32_t dst_ch = bpp; /* 1 byte per component. */
          if (src_ch == dst_ch) {
            for (uint32_t x = 0; x < w * dst_ch; x++) {
              dst_row[x] = uint8_t(to_unorm(s[x], 255));
            }
          }
          else {
            for (uint32_t x = 0; x < w; x++) {
              for (uint32_t c = 0; c < dst_ch; c++) {
                const float v = (c < src_ch) ? s[x * src_ch + c] : (c == 3 ? 1.0f : 0.0f);
                dst_row[x * dst_ch + c] = uint8_t(to_unorm(v, 255));
              }
            }
          }
          break;
        }
        case CONV_NONE:
          std::memcpy(dst_row, src_row, size_t(w) * bpp);
          break;
      }
    }
    upload = tmp;
  }

  WGPUTexelCopyTextureInfo dst = {};
  dst.texture = texture_;
  dst.mipLevel = uint32_t(mip + view_mip_);
  const uint32_t layer_off = (type_ & GPU_TEXTURE_3D) ? 0u : uint32_t(view_layer_);
  dst.origin = {uint32_t(offset[0]), uint32_t(offset[1]), uint32_t(offset[2]) + layer_off};
  WGPUTexelCopyBufferLayout layout = {};
  layout.offset = 0;
  layout.bytesPerRow = w * bpp;
  layout.rowsPerImage = h;
  WGPUExtent3D wext = {w, h, d};
  if (type_ & GPU_TEXTURE_1D) {
    /* Promoted 1D: the source's second axis (extent[1]/offset[1]) is the layer
     * axis; the memory layout (one w-texel row per layer) is unchanged. */
    dst.origin = {uint32_t(offset[0]), 0, uint32_t(offset[1]) + layer_off};
    wext = {w, 1, h};
    layout.rowsPerImage = 1;
  }
  const size_t byte_size = size_t(w) * bpp * h * d;
  if (getenv("WGPU_UTILTX_SYNTH") && w == 64 && d == 20 && tmp != nullptr) {
    /* Synthetic layer pattern: layer l filled with l/100 (fp16). */
    uint16_t *th = static_cast<uint16_t *>(tmp);
    for (uint32_t l = 0; l < d; l++) {
      const uint16_t hv = math::float_to_half(float(l) * 0.01f);
      for (size_t i = 0; i < size_t(w) * h * (bpp / 2); i++) {
        th[size_t(l) * w * h * (bpp / 2) + i] = hv;
      }
    }
    fprintf(stderr, "WGPU_UTILTX_SYNTH applied\n");
    fflush(stderr);
  }
  if (getenv("WGPU_DUMP_UTILTX") && w == 64 && d == 20) {
    const float *sf = static_cast<const float *>(data);
    const uint16_t *uh = static_cast<const uint16_t *>(upload);
    fprintf(stderr,
            "WGPU_UTILTX up=%ux%ux%u bpp=%u src_px=%zu row_px=%u conv=%d src0=[%.4f %.4f %.4f "
            "%.4f %.4f %.4f %.4f %.4f] dst0=[%04x %04x %04x %04x %04x %04x %04x %04x]\n",
            w, h, d, bpp, src_px, src_row_px, int(upload != data),
            sf[0], sf[1], sf[2], sf[3], sf[4], sf[5], sf[6], sf[7],
            uh[0], uh[1], uh[2], uh[3], uh[4], uh[5], uh[6], uh[7]);
    for (uint32_t l = 0; l < d; l += 1) {
      const size_t off = size_t(l) * h * w * (bpp / 2); /* uint16 index of layer start. */
      const float *sl = sf + size_t(l) * h * w * (src_px / 4);
      fprintf(stderr,
              "WGPU_UTILTX layer%u src=[%.4f %.4f %.4f %.4f] dst=[%04x %04x %04x %04x]\n",
              l, sl[0], sl[1], sl[2], sl[3], uh[off], uh[off + 1], uh[off + 2], uh[off + 3]);
    }
    fflush(stderr);
  }
  /* WriteTexture executes before recorded-but-unsubmitted passes — flush so a
   * mid-render texture update cannot retroactively clobber data that pending
   * passes were recorded against (same hazard as the clear() zero-fill). */
  ctx->flush_if_pass_open("tex_update");
  wgpuQueueWriteTexture(ctx->queue(), &dst, upload, byte_size, &layout, &wext);
  if (tmp) {
    free(tmp);
  }
}

void WebGPUTexture::generate_mipmap()
{
  WebGPUContext *ctx = ctx_get();
  if (ctx != nullptr) {
    ctx->generate_mipmaps(this);
  }
}

void WebGPUTexture::copy_to(Texture *dst_tex, IndexRange mip_levels)
{
  WebGPUTexture *dst = static_cast<WebGPUTexture *>(dst_tex);
  WebGPUContext *ctx = ctx_get();
  if (ctx == nullptr || texture_ == nullptr || dst == nullptr || dst->texture_ == nullptr) {
    return;
  }
  /* Copies must not land inside an open pass, and must execute after any draws
   * already recorded into it. */
  if (getenv("WGPU_LOG_RP") && ctx->render_pass() != nullptr) {
    fprintf(stderr, "WGPU_SPLIT copy_to '%s'\n", name_.c_str());
    fflush(stderr);
  }
  ctx->render_pass_end();
  WGPUCommandEncoder enc = ctx->ensure_encoder();
  if (enc == nullptr) {
    return;
  }
  const int layers = std::max(layer_count(), 1);
  for (const int mip : mip_levels) {
    WGPUTexelCopyTextureInfo src_info = {};
    src_info.texture = texture_;
    src_info.mipLevel = uint32_t(mip + view_mip_);
    src_info.origin = {0, 0, uint32_t(view_layer_)};
    WGPUTexelCopyTextureInfo dst_info = {};
    dst_info.texture = dst->texture_;
    dst_info.mipLevel = uint32_t(mip + dst->view_mip_);
    dst_info.origin = {0, 0, uint32_t(dst->view_layer_)};
    const uint32_t mw = std::max(w_ >> mip, 1);
    const uint32_t mh = std::max(h_ >> mip, 1);
    WGPUExtent3D ext = {mw, uint32_t(std::max(int(mh), 1)), uint32_t(layers)};
    wgpuCommandEncoderCopyTextureToTexture(enc, &src_info, &dst_info, &ext);
  }
  ctx->flush_encoder();
}

void WebGPUTexture::clear(const double4 data)
{
  /* Clear via a render pass per layer (works for color and depth formats that
   * carry RenderAttachment usage — Blender's pool textures do). Non-renderable
   * textures fall back to a zero upload when clearing to zero. */
  WebGPUContext *ctx = ctx_get();
  if (ctx == nullptr || texture_ == nullptr || ctx->device() == nullptr) {
    return;
  }
  const bool is_depth = is_depth_format();
  const uint32_t usage = wgpuTextureGetUsage(texture_);
  const bool is_3d = (type_ & GPU_TEXTURE_3D) != 0;
  const int layers = is_3d ? 1 : std::max(d_, 1);
  /* 3D textures cannot take 2D attachment views — a single invalid view POISONS
   * the whole command buffer at submit (dropping every valid draw recorded with
   * it). Route 3D clears through the upload path below. */
  if (!is_3d && (usage & WGPUTextureUsage_RenderAttachment)) {
    if (getenv("WGPU_LOG_RP") && ctx->render_pass() != nullptr) {
      fprintf(stderr, "WGPU_SPLIT tex_clear_rp '%s'\n", name_.c_str());
      fflush(stderr);
    }
    ctx->render_pass_end();
    WGPUCommandEncoder enc = ctx->ensure_encoder();
    if (enc == nullptr) {
      return;
    }
    for (int l = 0; l < layers; l++) {
      WGPUTextureView v = wgpu_attachment_view(l, 0);
      if (v == nullptr) {
        continue;
      }
      WGPURenderPassDescriptor desc = {};
      WGPURenderPassColorAttachment ca = {};
      WGPURenderPassDepthStencilAttachment da = {};
      if (is_depth) {
        da.view = v;
        da.depthLoadOp = WGPULoadOp_Clear;
        da.depthStoreOp = WGPUStoreOp_Store;
        da.depthClearValue = float(data.x);
        if (webgpu_format_has_stencil(wgpu_format_)) {
          da.stencilLoadOp = WGPULoadOp_Clear;
          da.stencilStoreOp = WGPUStoreOp_Store;
          da.stencilClearValue = 0;
        }
        desc.depthStencilAttachment = &da;
      }
      else {
        ca.view = v;
        ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = {data.x, data.y, data.z, data.w};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &ca;
      }
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &desc);
      if (pass) {
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
      }
    }
    return;
  }
  /* Texel-pattern fill for non-renderable (and 3D) textures: zero clears fill
   * with memset-zero; non-zero clears replicate one converted texel (formats
   * added as needed — a skipped clear leaves stale texels, e.g. volume
   * hit-depth cleared to non-zero was silently dropped before). */
  const bool is_zero = (data.x == 0.0 && data.y == 0.0 && data.z == 0.0 && data.w == 0.0);
  uint8_t texel[16] = {0};
  bool fill_supported = is_zero;
  if (!is_zero && !is_depth) {
    fill_supported = true;
    switch (wgpu_format_) {
      case WGPUTextureFormat_R32Uint:
      case WGPUTextureFormat_R32Sint: {
        const uint32_t v = uint32_t(data.x);
        std::memcpy(texel, &v, 4);
        break;
      }
      case WGPUTextureFormat_R32Float: {
        const float v = float(data.x);
        std::memcpy(texel, &v, 4);
        break;
      }
      case WGPUTextureFormat_RGBA32Float: {
        const float v[4] = {float(data.x), float(data.y), float(data.z), float(data.w)};
        std::memcpy(texel, v, 16);
        break;
      }
      case WGPUTextureFormat_R16Float:
      case WGPUTextureFormat_RG16Float:
      case WGPUTextureFormat_RGBA16Float: {
        const uint16_t h[4] = {math::float_to_half(float(data.x)),
                               math::float_to_half(float(data.y)),
                               math::float_to_half(float(data.z)),
                               math::float_to_half(float(data.w))};
        std::memcpy(texel, h, 8);
        break;
      }
      case WGPUTextureFormat_RG11B10Ufloat: {
        auto uf11 = [](float v) -> uint32_t {
          return (uint32_t(math::float_to_half(v > 0.0f ? v : 0.0f)) >> 4) & 0x7FFu;
        };
        auto uf10 = [](float v) -> uint32_t {
          return (uint32_t(math::float_to_half(v > 0.0f ? v : 0.0f)) >> 5) & 0x3FFu;
        };
        const uint32_t dw = uf11(float(data.x)) | (uf11(float(data.y)) << 11) |
                            (uf10(float(data.z)) << 22);
        std::memcpy(texel, &dw, 4);
        break;
      }
      default:
        fill_supported = false;
        break;
    }
  }
  if (fill_supported && !is_depth && ctx->queue() != nullptr) {
    /* MUST flush first: WriteTexture executes before any recorded-but-
     * unsubmitted passes — an unguarded atlas clear raced the batched shadow
     * passes (the once-per-render bimodal weak-shadow flake: whether the
     * encoder happened to be flushed decided the clear/write order). */
    ctx->flush_if_pass_open("tex_clear");
    {
      static int s_zc_log = 0;
      if (s_zc_log < 48) {
        s_zc_log++;
        fprintf(stderr, "WGPU_ZCLEAR '%s' %dx%dx%d fmt=%d view=%d mip=%d layer=%d\n",
                name_.c_str(), w_, h_, std::max(d_, 1), int(wgpu_format_), int(is_view_),
                view_mip_, view_layer_);
        fflush(stderr);
      }
    }
    const int depth_or_layers = std::max(d_, 1);
    const uint32_t bpp = webgpu_format_bytes_per_pixel(wgpu_format_);
    const size_t row = size_t(std::max(w_, 1)) * bpp;
    std::vector<uint8_t> zeros(row * size_t(std::max(h_, 1)) * size_t(depth_or_layers), 0);
    if (!is_zero) {
      for (size_t i = 0; i < zeros.size(); i += bpp) {
        std::memcpy(zeros.data() + i, texel, bpp);
      }
    }
    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = texture_;
    /* Views share the parent WGPUTexture — without these offsets a mip-view
     * clear stomps a zero rect into MIP 0 of the parent (this wiped baked
     * sphere-probe atlas regions whenever the clear executed after the bake). */
    dst.mipLevel = uint32_t(view_mip_);
    dst.origin = {0, 0, is_3d ? 0u : uint32_t(view_layer_)};
    WGPUTexelCopyBufferLayout layout = {};
    layout.bytesPerRow = uint32_t(row);
    layout.rowsPerImage = uint32_t(std::max(h_, 1));
    WGPUExtent3D ext = {
        uint32_t(std::max(w_, 1)), uint32_t(std::max(h_, 1)), uint32_t(depth_or_layers)};
    wgpuQueueWriteTexture(ctx->queue(), &dst, zeros.data(), zeros.size(), &layout, &ext);
    return;
  }
  static int s_unimpl_logged = 0;
  if (s_unimpl_logged < 8) {
    s_unimpl_logged++;
    fprintf(stderr, "WGPU_TEX clear unimplemented for fmt=%d usage=0x%x\n", int(wgpu_format_),
            usage);
    fflush(stderr);
  }
}

void WebGPUTexture::read(int /*mip*/, eGPUDataFormat format, void *dst)
{
  WebGPUContext *ctx = ctx_get();
  if (ctx == nullptr || texture_ == nullptr || dst == nullptr) {
    return;
  }
  /* Channel count of the destination matches the texture's component count. */
  int channels = 4;
  switch (wgpu_format_) {
    case WGPUTextureFormat_R8Unorm:
    case WGPUTextureFormat_R8Uint:
    case WGPUTextureFormat_R16Float:
    case WGPUTextureFormat_R16Uint:
    case WGPUTextureFormat_R32Float:
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
      channels = 1; break;
    case WGPUTextureFormat_RG8Unorm:
    case WGPUTextureFormat_RG16Float:
    case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_RG32Uint:
      channels = 2; break;
    default:
      channels = 4; break;
  }
  ctx->read_color_sync(texture_,
                       wgpu_format_,
                       0,
                       0,
                       width_get(),
                       height_get(),
                       format,
                       channels,
                       dst,
                       view_layer_,
                       view_mip_);
}

}  // namespace blender::gpu
