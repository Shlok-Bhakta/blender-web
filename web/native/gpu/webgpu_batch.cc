/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "GPU_vertex_format.hh"

#include "webgpu_batch.hh"
#include "webgpu_context.hh"
#include "webgpu_framebuffer.hh"
#include "webgpu_index_buffer.hh"
#include "webgpu_shader.hh"
#include "webgpu_shader_interface.hh"
#include "webgpu_storage_buffer.hh"
#include "webgpu_texture.hh"
#include "webgpu_vertex_buffer.hh"

namespace blender::gpu {

static WGPUPrimitiveTopology to_wgpu_topology(GPUPrimType prim)
{
  switch (prim) {
    case GPU_PRIM_POINTS:
      return WGPUPrimitiveTopology_PointList;
    case GPU_PRIM_LINES:
      return WGPUPrimitiveTopology_LineList;
    case GPU_PRIM_LINE_STRIP:
      return WGPUPrimitiveTopology_LineStrip;
    case GPU_PRIM_TRI_STRIP:
      return WGPUPrimitiveTopology_TriangleStrip;
    case GPU_PRIM_TRIS:
    case GPU_PRIM_TRI_FAN: /* No fan in WebGPU; treated as list (approximate). */
    default:
      return WGPUPrimitiveTopology_TriangleList;
  }
}

static bool is_strip(GPUPrimType prim)
{
  return prim == GPU_PRIM_LINE_STRIP || prim == GPU_PRIM_TRI_STRIP;
}

/* Map a Blender vertex attribute (component type/count/fetch) to a WGPU vertex
 * format. Covers the cases EEVEE/overlay batches actually use; unknowns fall
 * back to Float32x4 so pipeline creation does not hard-fail. */
static WGPUVertexFormat to_wgpu_vertex_format(GPUVertCompType comp,
                                              int len,
                                              GPUVertFetchMode fetch)
{
  const bool norm = (fetch == GPU_FETCH_INT_TO_FLOAT_UNIT);
  switch (comp) {
    case GPU_COMP_F32:
      return len == 1 ? WGPUVertexFormat_Float32 :
             len == 2 ? WGPUVertexFormat_Float32x2 :
             len == 3 ? WGPUVertexFormat_Float32x3 :
                        WGPUVertexFormat_Float32x4;
    case GPU_COMP_I32:
      return len == 1 ? WGPUVertexFormat_Sint32 :
             len == 2 ? WGPUVertexFormat_Sint32x2 :
             len == 3 ? WGPUVertexFormat_Sint32x3 :
                        WGPUVertexFormat_Sint32x4;
    case GPU_COMP_U32:
      return len == 1 ? WGPUVertexFormat_Uint32 :
             len == 2 ? WGPUVertexFormat_Uint32x2 :
             len == 3 ? WGPUVertexFormat_Uint32x3 :
                        WGPUVertexFormat_Uint32x4;
    case GPU_COMP_I16:
      if (len <= 2) {
        return norm ? WGPUVertexFormat_Snorm16x2 : WGPUVertexFormat_Sint16x2;
      }
      return norm ? WGPUVertexFormat_Snorm16x4 : WGPUVertexFormat_Sint16x4;
    case GPU_COMP_U16:
      if (len <= 2) {
        return norm ? WGPUVertexFormat_Unorm16x2 : WGPUVertexFormat_Uint16x2;
      }
      return norm ? WGPUVertexFormat_Unorm16x4 : WGPUVertexFormat_Uint16x4;
    case GPU_COMP_I8:
      if (len <= 2) {
        return norm ? WGPUVertexFormat_Snorm8x2 : WGPUVertexFormat_Sint8x2;
      }
      return norm ? WGPUVertexFormat_Snorm8x4 : WGPUVertexFormat_Sint8x4;
    case GPU_COMP_U8:
      if (len <= 2) {
        return norm ? WGPUVertexFormat_Unorm8x2 : WGPUVertexFormat_Uint8x2;
      }
      return norm ? WGPUVertexFormat_Unorm8x4 : WGPUVertexFormat_Uint8x4;
    case GPU_COMP_I10:
      /* Signed 10_10_10_2 has no WebGPU vertex format. Unorm10_10_10_2 keeps the
       * 4-byte stride so the pipeline stays valid (values are wrong — avoid by
       * enabling GCaps.use_hq_normals_workaround, which sidesteps I10 entirely). */
      return WGPUVertexFormat_Unorm10_10_10_2;
    default:
      fprintf(stderr, "WGPU_BATCH unmapped vertex format comp=%d len=%d\n", int(comp), len);
      return WGPUVertexFormat_Float32x4;
  }
}

/* Map Blender's pending GPU state (StateManager::state — set by DRW before each
 * draw; apply_state is deferred to pipeline creation on this backend). */
static WGPUCompareFunction to_wgpu_depth_compare(GPUDepthTest t)
{
  switch (t) {
    case GPU_DEPTH_NONE:
    case GPU_DEPTH_ALWAYS:
      return WGPUCompareFunction_Always;
    case GPU_DEPTH_LESS:
      return WGPUCompareFunction_Less;
    case GPU_DEPTH_LESS_EQUAL:
      return WGPUCompareFunction_LessEqual;
    case GPU_DEPTH_EQUAL:
      return WGPUCompareFunction_Equal;
    case GPU_DEPTH_GREATER:
      return WGPUCompareFunction_Greater;
    case GPU_DEPTH_GREATER_EQUAL:
      return WGPUCompareFunction_GreaterEqual;
    default:
      return WGPUCompareFunction_LessEqual;
  }
}

/* Stencil state resolved from the pending GPU state. The reference value is
 * dynamic (SetStencilReference); everything else bakes into the pipeline. */
struct WGPUStencilParams {
  bool enabled = false;
  WGPUStencilFaceState front = {WGPUCompareFunction_Always,
                                WGPUStencilOperation_Keep,
                                WGPUStencilOperation_Keep,
                                WGPUStencilOperation_Keep};
  WGPUStencilFaceState back = {WGPUCompareFunction_Always,
                               WGPUStencilOperation_Keep,
                               WGPUStencilOperation_Keep,
                               WGPUStencilOperation_Keep};
  uint32_t read_mask = 0xFF;
  uint32_t write_mask = 0;
};

static WGPUStencilParams to_wgpu_stencil(const GPUState &gst,
                                         const GPUStateMutable &mst,
                                         WGPUTextureFormat depth_fmt)
{
  WGPUStencilParams p;
  const bool fmt_has_stencil = depth_fmt == WGPUTextureFormat_Depth24PlusStencil8 ||
                               depth_fmt == WGPUTextureFormat_Depth32FloatStencil8;
  const GPUStencilTest test = GPUStencilTest(gst.stencil_test);
  if (!fmt_has_stencil || test == GPU_STENCIL_NONE) {
    return p;
  }
  p.enabled = true;
  const WGPUCompareFunction cmp = (test == GPU_STENCIL_EQUAL)  ? WGPUCompareFunction_Equal :
                                  (test == GPU_STENCIL_NEQUAL) ? WGPUCompareFunction_NotEqual :
                                                                 WGPUCompareFunction_Always;
  p.front.compare = p.back.compare = cmp;
  /* Semantics from gl_state.cc set_stencil_test. NOTE: our frontFace mapping
   * already compensates the y-flip, so WGPU front == GL front here. */
  switch (GPUStencilOp(gst.stencil_op)) {
    case GPU_STENCIL_OP_REPLACE:
      p.front.passOp = p.back.passOp = WGPUStencilOperation_Replace;
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_PASS:
      p.back.passOp = WGPUStencilOperation_IncrementWrap;
      p.front.passOp = WGPUStencilOperation_DecrementWrap;
      break;
    case GPU_STENCIL_OP_COUNT_DEPTH_FAIL:
      p.back.depthFailOp = WGPUStencilOperation_DecrementWrap;
      p.front.depthFailOp = WGPUStencilOperation_IncrementWrap;
      break;
    case GPU_STENCIL_OP_NONE:
    default:
      break;
  }
  p.read_mask = mst.stencil_compare_mask;
  p.write_mask = (gst.write_mask & GPU_WRITE_STENCIL) ? mst.stencil_write_mask : 0;
  return p;
}

/* Returns true when `blend` maps to a WGPU blend state (written to r_bs). */
static bool to_wgpu_blend(GPUBlend blend, WGPUBlendState &r_bs)
{
  auto comp = [](WGPUBlendFactor src, WGPUBlendFactor dst) {
    WGPUBlendComponent c = {};
    c.operation = WGPUBlendOperation_Add;
    c.srcFactor = src;
    c.dstFactor = dst;
    return c;
  };
  switch (blend) {
    case GPU_BLEND_ALPHA:
      r_bs.color = comp(WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha);
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha);
      return true;
    case GPU_BLEND_ALPHA_PREMULT:
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha);
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha);
      return true;
    case GPU_BLEND_ADDITIVE:
      r_bs.color = comp(WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_One);
      r_bs.alpha = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_One);
      return true;
    case GPU_BLEND_ADDITIVE_PREMULT:
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      return true;
    case GPU_BLEND_MULTIPLY:
      r_bs.color = comp(WGPUBlendFactor_Dst, WGPUBlendFactor_Zero);
      r_bs.alpha = comp(WGPUBlendFactor_DstAlpha, WGPUBlendFactor_Zero);
      return true;
    case GPU_BLEND_SUBTRACT:
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.color.operation = WGPUBlendOperation_ReverseSubtract;
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.alpha.operation = WGPUBlendOperation_ReverseSubtract;
      return true;
    case GPU_BLEND_ALPHA_UNDER_PREMUL:
      r_bs.color = comp(WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_One);
      r_bs.alpha = comp(WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_One);
      return true;
    case GPU_BLEND_BACKGROUND:
      /* Matches gl_state.cc: src.rgb * (1 - dst.a) + dst.rgb * src.a — the
       * DST-alpha-driven equation overlay_background relies on to slot the
       * theme color BEHIND the (already drawn) render result. The previous
       * src-alpha mapping overwrote the viewport with the theme/black. */
      r_bs.color = comp(WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_SrcAlpha);
      r_bs.alpha = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_SrcAlpha);
      return true;
    case GPU_BLEND_OIT:
      /* Weighted-blended OIT (workbench transparent/x-ray, wireframe mode):
       * accum += src (additive rgb); revealage *= (1 - src.a). Matches
       * gl_state.cc ONE/ONE + ZERO/ONE_MINUS_SRC_ALPHA on every target. */
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.alpha = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_OneMinusSrcAlpha);
      return true;
    case GPU_BLEND_MIN:
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.color.operation = WGPUBlendOperation_Min;
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.alpha.operation = WGPUBlendOperation_Min;
      return true;
    case GPU_BLEND_MAX:
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.color.operation = WGPUBlendOperation_Max;
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_One);
      r_bs.alpha.operation = WGPUBlendOperation_Max;
      return true;
    case GPU_BLEND_INVERT:
      r_bs.color = comp(WGPUBlendFactor_OneMinusDst, WGPUBlendFactor_Zero);
      r_bs.alpha = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_One);
      return true;
    case GPU_BLEND_CUSTOM:
      /* Dual-source: src.rgb + dst.rgb * src1.rgb (gl_state.cc ONE/SRC1_COLOR).
       * Requires the fragment to declare @blend_src outputs — Blender's
       * dual-source shaders (eevee_forward_resolve, volume resolve, gpencil AA)
       * declare index(1), which survives as SPIR-V Index -> WGSL @blend_src. */
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_Src1);
      r_bs.alpha = comp(WGPUBlendFactor_One, WGPUBlendFactor_Src1Alpha);
      return true;
    case GPU_BLEND_OVERLAY_MASK_FROM_ALPHA:
      /* dst *= (1 - src.a) — overlay masking (gl_state.cc ZERO/ONE_MINUS_SRC_ALPHA). */
      r_bs.color = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_OneMinusSrcAlpha);
      r_bs.alpha = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_OneMinusSrcAlpha);
      return true;
    case GPU_BLEND_TRANSPARENCY:
      /* EEVEE transposed transparency accumulation (per-channel MRT targets:
       * (radiance_ch, ..., transmittance_ch)): rgb accumulates behind-to-front
       * as src + dst*src.a; alpha holds the transmittance PRODUCT dst.a*src.a.
       * Unmapped, this fell to blending-DISABLED: every transparent surface
       * REPLACED the accumulation, so only the last-drawn layer survived where
       * transparent objects overlap (alpha_blend: first plane's tint erased
       * over the second's footprint; single layers were exact). */
      r_bs.color = comp(WGPUBlendFactor_One, WGPUBlendFactor_SrcAlpha);
      r_bs.alpha = comp(WGPUBlendFactor_Zero, WGPUBlendFactor_SrcAlpha);
      return true;
    default: {
      /* Silently disabling blending for an unmapped mode cost DAYS on
       * GPU_BLEND_TRANSPARENCY (overlapping transparency replaced instead of
       * accumulated, single layers pixel-perfect). Never again: log it. */
      static int s_unmapped_logged = 0;
      if (s_unmapped_logged < 8) {
        s_unmapped_logged++;
        fprintf(stderr, "WGPU_BLEND UNMAPPED mode=%d — drawing with blending DISABLED\n",
                int(blend));
        fflush(stderr);
      }
      return false;
    }
  }
}

/* Apply the framebuffer's viewport + scissor to the pass (UI region drawing
 * depends on both). GL rects are y-up-from-bottom; WebGPU y-down-from-top. */
static void apply_viewport_scissor(WGPURenderPassEncoder pass, WebGPUFrameBuffer *fb)
{
  const int2 size = fb->size_get();
  if (size.x <= 0 || size.y <= 0) {
    return;
  }
  if (fb->multi_viewport()) {
    /* gl_ViewportIndex emulation (EEVEE shadows): the shader applies the
     * per-index rect itself; the hardware viewport must cover the full target.
     * Using viewport_[0] here squeezed every shadow view into the first 256px
     * corner — pages landed at wrong atlas texels and shadow_eval read empty
     * pages (the "no cast shadows" bug). */
    wgpuRenderPassEncoderSetViewport(
        pass, 0.0f, 0.0f, float(size.x), float(size.y), 0.0f, 1.0f);
    return;
  }
  /* The window backbuffer stores content in GL's bottom-up row order and is
   * flipped once at present (see webgpu_context present blit). Its GL y-bottom
   * origin therefore maps directly to a WebGPU top-left offset — re-flipping it
   * here double-counts the region's vertical offset, dropping overlays drawn
   * straight to the window (e.g. the box-select marquee) below the cursor.
   * Offscreen targets are never present-flipped, so they still need the flip. */
  const bool flip_y = !fb->is_backbuffer();
  int vp[4];
  fb->viewport_get(vp);
  if (vp[2] > 0 && vp[3] > 0) {
    /* Clamp inside the attachment (WebGPU validates strictly). */
    int x = std::max(0, vp[0]);
    int w = std::min(vp[2], size.x - x);
    int y_bottom = std::max(0, vp[1]);
    int h = std::min(vp[3], size.y - y_bottom);
    int y = flip_y ? (size.y - y_bottom - h) : y_bottom;
    if (w > 0 && h > 0) {
      wgpuRenderPassEncoderSetViewport(pass, float(x), float(y), float(w), float(h), 0.0f, 1.0f);
    }
  }
  if (fb->scissor_test_get()) {
    int sc[4];
    fb->scissor_get(sc);
    int x = std::max(0, sc[0]);
    int w = std::min(sc[2], size.x - x);
    int y_bottom = std::max(0, sc[1]);
    int h = std::min(sc[3], size.y - y_bottom);
    int y = flip_y ? (size.y - y_bottom - h) : y_bottom;
    if (w > 0 && h > 0) {
      wgpuRenderPassEncoderSetScissorRect(pass, uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h));
    }
  }
  else {
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, uint32_t(size.x), uint32_t(size.y));
  }
}

static void hash_append(uint64_t &h, const void *data, size_t len)
{
  const uint8_t *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; i++) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
}

void WebGPUBatch::draw(int vertex_first, int vertex_count, int instance_first, int instance_count)
{
  record_draw(vertex_first, vertex_count, instance_first, instance_count, nullptr, 0, 0, 0);
}

void WebGPUBatch::record_draw(int vertex_first,
                              int vertex_count,
                              int instance_first,
                              int instance_count,
                              StorageBuf *indirect_buf,
                              int indirect_count,
                              intptr_t indirect_offset,
                              intptr_t indirect_stride)
{
  WebGPUContext *ctx = WebGPUContext::get();
  static int s_entry_log = 0;
  if (s_entry_log < 8) {
    WebGPUShader *es = ctx ? static_cast<WebGPUShader *>(ctx->shader) : nullptr;
    fprintf(stderr,
            "WGPU_BATCH::draw entry #%d ctx=%p dev=%p shader=%p valid=%d fb=%p\n",
            s_entry_log++,
            (void *)ctx,
            ctx ? (void *)ctx->device() : nullptr,
            (void *)es,
            es ? int(es->is_valid()) : -1,
            ctx ? (void *)ctx->active_fb : nullptr);
    fflush(stderr);
  }
  if (ctx == nullptr || ctx->device() == nullptr) {
    return;
  }
  WebGPUShader *shader = static_cast<WebGPUShader *>(ctx->shader);
  if (shader == nullptr || !shader->is_valid() || shader->vertex_module() == nullptr ||
      shader->fragment_module() == nullptr || shader->interface == nullptr)
  {
    static int s_skip_logged = 0;
    if (shader && s_skip_logged < 200) {
      s_skip_logged++;
      fprintf(stderr,
              "WGPU_BATCH skipped '%s' valid=%d vmod=%p fmod=%p iface=%p\n",
              shader->name_get().c_str(),
              int(shader->is_valid()),
              (void *)shader->vertex_module(),
              (void *)shader->fragment_module(),
              (void *)shader->interface);
      fflush(stderr);
    }
    return;
  }
  WebGPUFrameBuffer *fb = static_cast<WebGPUFrameBuffer *>(ctx->active_fb);
  if (fb == nullptr) {
    return;
  }
  WebGPUShaderInterface *iface = static_cast<WebGPUShaderInterface *>(shader->interface);

  if (instance_count == 0) {
    instance_count = 1;
  }

  /* --- Vertex buffer layouts from the batch's vertex formats. --- */
  std::vector<WGPUVertexBufferLayout> vb_layouts;
  std::vector<std::vector<WGPUVertexAttribute>> vb_attrs; /* stable storage */
  std::vector<WGPUBuffer> vb_buffers;
  /* One shader location may match aliases in SEVERAL vbos (instancing vbos
   * repeat mesh attribute names) — a location bound twice is a pipeline
   * validation error. First matching vbo wins, like GL's bind order. */
  uint32_t bound_locations = 0;
  for (int vi = 0; vi < GPU_BATCH_VBO_MAX_LEN; vi++) {
    VertBuf *vbo = verts_(vi);
    if (vbo == nullptr) {
      continue;
    }
    WGPUBuffer buf = static_cast<WebGPUVertexBuffer *>(vbo)->wgpu_buffer();
    if (buf == nullptr) {
      continue;
    }
    const GPUVertFormat &fmt = vbo->format;
    if (getenv("WGPU_LOG_ATTRS") && strstr(shader->name_get().c_str(), "MATP") != nullptr) {
      fprintf(stderr, "WGPU_ATTR_VBO '%s' vbo%d:", shader->name_get().c_str(), vi);
      for (uint ai = 0; ai < fmt.attr_len; ai++) {
        for (uint ni = 0; ni < fmt.attrs[ai].name_len; ni++) {
          fprintf(stderr, " %s", GPU_vertformat_attr_name_get(&fmt, &fmt.attrs[ai], ni));
        }
        fprintf(stderr, ai + 1 < fmt.attr_len ? " |" : "");
      }
      fprintf(stderr, "\n");
      fflush(stderr);
    }
    std::vector<WGPUVertexAttribute> attrs;
    for (uint ai = 0; ai < fmt.attr_len; ai++) {
      const GPUVertAttr &attr = fmt.attrs[ai];
      /* Match EVERY alias name against the shader interface (mesh UV/color
       * layers register hashed alias names after the base name; materials
       * reference the alias — matching only name 0 left UVs unfed and
       * zero-padded: every texture sampled at (0, 0)). GL binds each matching
       * alias; mirror that, one vertex attribute per matched location. */
      for (uint ni = 0; ni < attr.name_len; ni++) {
        const char *name = GPU_vertformat_attr_name_get(&fmt, &attr, ni);
        const ShaderInput *in = iface->attr_get(name);
        if (in == nullptr || in->location < 0) {
          continue;
        }
        if (in->location < 32 && (bound_locations & (1u << in->location))) {
          continue;
        }
        bound_locations |= (in->location < 32) ? (1u << in->location) : 0;
        WGPUVertexAttribute wa = {};
        wa.format = to_wgpu_vertex_format(
            attr.type.comp_type(), attr.type.comp_len(), attr.type.fetch_mode());
        wa.offset = attr.offset;
        wa.shaderLocation = uint32_t(in->location);
        attrs.push_back(wa);
      }
    }
    if (attrs.empty()) {
      continue;
    }
    vb_attrs.push_back(std::move(attrs));
    vb_buffers.push_back(buf);
    WGPUVertexBufferLayout layout = {};
    layout.arrayStride = fmt.stride;
    layout.stepMode = WGPUVertexStepMode_Vertex;
    layout.attributeCount = vb_attrs.back().size();
    layout.attributes = vb_attrs.back().data();
    vb_layouts.push_back(layout);
  }

  /* WebGPU rejects a pipeline whose vertex shader reads a @location absent from
   * the VertexState (GL fetches a default constant instead). Feed the locations
   * the batch's VBOs don't cover from a zeroed buffer at arrayStride 0. */
  {
    uint32_t covered = 0;
    for (const std::vector<WGPUVertexAttribute> &av : vb_attrs) {
      for (const WGPUVertexAttribute &a : av) {
        covered |= 1u << a.shaderLocation;
      }
    }
    std::vector<WGPUVertexAttribute> pad;
    for (const WgslVertexInput &vin : shader->vertex_inputs()) {
      if (vin.location < 32 && !(covered & (1u << vin.location))) {
        WGPUVertexAttribute wa = {};
        wa.format = vin.format;
        wa.offset = 0;
        wa.shaderLocation = vin.location;
        pad.push_back(wa);
        if (getenv("WGPU_LOG_ATTRS")) {
          /* Which interface attr name owns this location? */
          const char *owner = "?";
          for (uint ii = 0; ii < iface->attr_len_; ii++) {
            const ShaderInput *si = iface->inputs_ + ii;
            if (si && si->location == int(vin.location)) {
              owner = iface->input_name_get(si);
              break;
            }
          }
          fprintf(stderr,
                  "WGPU_ATTR_PAD '%s' loc=%u name='%s'\n",
                  shader->name_get().c_str(),
                  vin.location,
                  owner);
          fflush(stderr);
        }
      }
    }
    WGPUBuffer nb = pad.empty() ? nullptr : ctx->null_attr_buffer();
    if (nb != nullptr) {
      vb_attrs.push_back(std::move(pad));
      vb_buffers.push_back(nb);
      WGPUVertexBufferLayout layout = {};
      layout.arrayStride = 0;
      layout.stepMode = WGPUVertexStepMode_Vertex;
      layout.attributeCount = vb_attrs.back().size();
      layout.attributes = vb_attrs.back().data();
      vb_layouts.push_back(layout);
    }
  }

  /* --- Pipeline cache key. --- */
  const int color_count = fb->color_attachment_count();
  WGPUTextureFormat color_fmt[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
  for (int i = 0; i < color_count; i++) {
    color_fmt[i] = fb->color_format(i);
  }
  const WGPUTextureFormat depth_fmt = fb->depth_format();
  WGPUPrimitiveTopology topo = to_wgpu_topology(prim_type);

  /* Point-sprite emulation: expand each point into a per-instance quad (WebGPU
   * points are 1px). Vertex buffers switch to per-instance stepping so each
   * quad instance reads its point's attributes; the vertex wrapper offsets the
   * 4 strip corners by gl_PointSize (gpu_point_expand override = 1). */
  int vp_rect[4] = {0, 0, 0, 0};
  fb->viewport_get(vp_rect);
  if (vp_rect[2] <= 0 || vp_rect[3] <= 0) {
    /* No explicit viewport: the pass uses the full attachment. */
    const int2 fb_size = fb->size_get();
    vp_rect[2] = fb_size.x;
    vp_rect[3] = fb_size.y;
  }
  const bool expand_points = (topo == WGPUPrimitiveTopology_PointList) &&
                             shader->can_expand_points() && instance_count == 1 &&
                             elem_() == nullptr && indirect_buf == nullptr && vp_rect[2] > 0 &&
                             vp_rect[3] > 0;
  if (topo == WGPUPrimitiveTopology_PointList && getenv("WGPU_LOG_POINTS")) {
    fprintf(stderr,
            "WGPU_PTS batch '%s' expand=%d can=%d inst=%d elem=%d vp=%dx%d\n",
            shader->name_get().c_str(),
            int(expand_points),
            int(shader->can_expand_points()),
            instance_count,
            int(elem_() != nullptr),
            vp_rect[2], vp_rect[3]);
    fflush(stderr);
  }
  if (expand_points) {
    topo = WGPUPrimitiveTopology_TriangleStrip;
    for (WGPUVertexBufferLayout &l : vb_layouts) {
      l.stepMode = WGPUVertexStepMode_Instance;
    }
  }

  /* Pending DRW/GPU state that shapes the pipeline. */
  const GPUState &gst = ctx->state_manager->state;
  GPUDepthTest depth_test = GPUDepthTest(gst.depth_test);
  const bool depth_write = (gst.write_mask & GPU_WRITE_DEPTH) != 0;
  const GPUBlend blend = GPUBlend(gst.blend);
  /* DEBUG bisect: ENV.WGPU_TRANSP_NODEPTH=1 disables the depth test for
   * dual-source (forward transparent) draws — discriminates depth-rejection
   * bugs from blend/accumulation bugs. */
  if (blend == GPU_BLEND_CUSTOM && getenv("WGPU_TRANSP_NODEPTH")) {
    depth_test = GPU_DEPTH_ALWAYS;
  }
  const GPUFaceCullTest cull = GPUFaceCullTest(gst.culling_test);
  const GPUStateMutable &mst = ctx->state_manager->mutable_state;
  const WGPUStencilParams stencil = to_wgpu_stencil(gst, mst, depth_fmt);
  /* DEBUG: ENV.WGPU_LOG_DRAW=<fb-name substr> — draw sequence + depth/blend
   * state for every draw targeting matching framebuffers. */
  if (const char *dpat = getenv("WGPU_LOG_DRAW")) {
    if (fb != nullptr && strstr(fb->name_get(), dpat) != nullptr) {
      fprintf(stderr,
              "WGPU_DRAW fb='%s' sh='%s' dtest=%d dwrite=%d blend=%d cull=%d\n",
              fb->name_get(),
              shader->name_get().c_str(),
              int(depth_test),
              int(depth_write),
              int(blend),
              int(cull));
      fflush(stderr);
    }
  }
  if (getenv("WGPU_LOG_STENCIL") && GPUStencilTest(gst.stencil_test) != GPU_STENCIL_NONE) {
    fprintf(stderr,
            "WGPU_STENCIL '%s' test=%d op=%d ref=0x%02x cmp_mask=0x%02x wr_mask=0x%02x "
            "enabled=%d depth_fmt=%d\n",
            shader->name_get().c_str(),
            int(gst.stencil_test),
            int(gst.stencil_op),
            mst.stencil_reference,
            mst.stencil_compare_mask,
            mst.stencil_write_mask,
            int(stencil.enabled),
            int(depth_fmt));
    fflush(stderr);
  }

  uint64_t key = 1469598103934665603ull;
  void *sh_ptr = shader;
  hash_append(key, &sh_ptr, sizeof(sh_ptr));
  /* Specialization-constant values select a distinct pipeline (WGSL overrides). */
  const uint64_t spec_h = shader->spec_hash();
  hash_append(key, &spec_h, sizeof(spec_h));
  hash_append(key, &topo, sizeof(topo));
  hash_append(key, &color_count, sizeof(color_count));
  hash_append(key, color_fmt, sizeof(WGPUTextureFormat) * color_count);
  hash_append(key, &depth_fmt, sizeof(depth_fmt));
  hash_append(key, &depth_test, sizeof(depth_test));
  hash_append(key, &depth_write, sizeof(depth_write));
  /* The COLOR write mask shapes the pipeline too — without it in the key a
   * depth-only prepass (wireframe/x-ray hidden-line, color writes masked)
   * reuses a color-writing pipeline and fills geometry black. */
  const uint32_t write_mask_bits = uint32_t(gst.write_mask);
  hash_append(key, &write_mask_bits, sizeof(write_mask_bits));
  hash_append(key, &blend, sizeof(blend));
  hash_append(key, &cull, sizeof(cull));
  hash_append(key, &stencil, sizeof(stencil));
  for (const WGPUVertexBufferLayout &l : vb_layouts) {
    hash_append(key, &l.arrayStride, sizeof(l.arrayStride));
    for (size_t a = 0; a < l.attributeCount; a++) {
      hash_append(key, &l.attributes[a], sizeof(WGPUVertexAttribute));
    }
  }
  hash_append(key, &expand_points, sizeof(expand_points));
  if (expand_points) {
    hash_append(key, &vp_rect[2], sizeof(int) * 2);
  }

  WGPURenderPipeline pipeline = ctx->render_pipeline_get(key);
  if (pipeline == nullptr) {
    WGPUColorTargetState targets[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
    WGPUBlendState blend_state = {};
    const bool has_blend = to_wgpu_blend(blend, blend_state);
    const uint32_t out_mask = shader->fragment_output_mask();
    WGPUColorWriteMask color_write = WGPUColorWriteMask_None;
    if (gst.write_mask & GPU_WRITE_RED) {
      color_write |= WGPUColorWriteMask_Red;
    }
    if (gst.write_mask & GPU_WRITE_GREEN) {
      color_write |= WGPUColorWriteMask_Green;
    }
    if (gst.write_mask & GPU_WRITE_BLUE) {
      color_write |= WGPUColorWriteMask_Blue;
    }
    if (gst.write_mask & GPU_WRITE_ALPHA) {
      color_write |= WGPUColorWriteMask_Alpha;
    }
    for (int i = 0; i < color_count; i++) {
      targets[i].format = color_fmt[i];
      /* A color target with no matching fragment output must have writeMask 0
       * (EEVEE prepasses bind targets their fragment shader never writes). */
      targets[i].writeMask = (out_mask & (1u << i)) ? color_write : WGPUColorWriteMask_None;
      targets[i].blend = has_blend ? &blend_state : nullptr;
    }
    /* Per-submit specialization-constant values as WGSL override entries. */
    std::vector<WGPUConstantEntry> vert_consts, frag_consts;
    shader->spec_entries(shader->vertex_wgsl(), vert_consts);
    shader->spec_entries(shader->fragment_wgsl(), frag_consts);
    if (expand_points) {
      /* Wrapper overrides: @id(240/241) viewport px, @id(242) expand enable. */
      WGPUConstantEntry e = {};
      e.key = {"240", WGPU_STRLEN};
      e.value = double(vp_rect[2]);
      vert_consts.push_back(e);
      e.key = {"241", WGPU_STRLEN};
      e.value = double(vp_rect[3]);
      vert_consts.push_back(e);
      e.key = {"242", WGPU_STRLEN};
      e.value = 1.0;
      vert_consts.push_back(e);
    }

    WGPUFragmentState frag = {};
    frag.module = shader->fragment_module();
    frag.entryPoint = {"main", WGPU_STRLEN};
    frag.targetCount = uint32_t(color_count);
    frag.targets = color_count ? targets : nullptr;
    frag.constantCount = frag_consts.size();
    frag.constants = frag_consts.empty() ? nullptr : frag_consts.data();

    WGPUDepthStencilState ds = {};
    bool has_depth = depth_fmt != WGPUTextureFormat_Undefined;
    if (has_depth) {
      ds.format = depth_fmt;
      ds.depthWriteEnabled = depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
      ds.depthCompare = to_wgpu_depth_compare(depth_test);
      ds.stencilFront = stencil.front;
      ds.stencilBack = stencil.back;
      ds.stencilReadMask = stencil.read_mask;
      ds.stencilWriteMask = stencil.write_mask;
    }

    /* Explicit pipeline layout from the parsed WGSL bindings so the layout has
     * exactly those bindings (auto-layout would strip declared-but-unused ones,
     * mismatching our complete bind group). */
    WGPUBindGroupLayout bgl_explicit = ctx->make_bind_group_layout(
        shader->render_bindings(), false, shader->interface);
    WGPUPipelineLayout pipe_layout = ctx->make_pipeline_layout(bgl_explicit);

    WGPURenderPipelineDescriptor rpd = {};
    /* Label = shader name so Dawn validation errors are attributable. */
    rpd.label = {shader->name_get().c_str(), WGPU_STRLEN};
    rpd.layout = pipe_layout;
    rpd.vertex.module = shader->vertex_module();
    rpd.vertex.entryPoint = {"main", WGPU_STRLEN};
    rpd.vertex.bufferCount = uint32_t(vb_layouts.size());
    rpd.vertex.buffers = vb_layouts.empty() ? nullptr : vb_layouts.data();
    rpd.vertex.constantCount = vert_consts.size();
    rpd.vertex.constants = vert_consts.empty() ? nullptr : vert_consts.data();
    rpd.primitive.topology = topo;
    /* Keep GL's winding convention (CCW = front). Empirically verified via
     * gl_FrontFacing: with CW-front here every triangle reported BACKFACING and
     * EEVEE negated all shading normals (lights appeared to come from the view
     * direction). Our pipeline does not flip y in the vertex stage, so NDC
     * winding matches GL directly. */
    /* The vertex wrapper negates gl_Position.y (GL bottom-up convention), which
     * inverts screen-space winding — so the GL default CCW maps to CW here. */
    rpd.primitive.frontFace = gst.invert_facing ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
    rpd.primitive.cullMode = (cull == GPU_CULL_FRONT) ? WGPUCullMode_Front :
                             (cull == GPU_CULL_BACK)  ? WGPUCullMode_Back :
                                                        WGPUCullMode_None;
    if (is_strip(prim_type) && elem_() != nullptr) {
      rpd.primitive.stripIndexFormat =
          static_cast<WebGPUIndexBuf *>(elem_())->wgpu_index_format();
    }
    rpd.multisample.count = 1;
    rpd.multisample.mask = 0xFFFFFFFFu;
    rpd.fragment = &frag;
    rpd.depthStencil = has_depth ? &ds : nullptr;

    pipeline = wgpuDeviceCreateRenderPipeline(ctx->device(), &rpd);
    if (pipe_layout) {
      wgpuPipelineLayoutRelease(pipe_layout);
    }
    if (bgl_explicit) {
      wgpuBindGroupLayoutRelease(bgl_explicit);
    }
    if (pipeline == nullptr) {
      fprintf(stderr, "WGPU_BATCH pipeline creation failed for '%s'\n",
              shader->name_get().c_str());
      return;
    }
    ctx->render_pipeline_put(key, pipeline);
    fprintf(stderr,
            "WGPU_BATCH pipeline created shader='%s' colors=%d depth=%d vbos=%zu\n",
            shader->name_get().c_str(),
            color_count,
            int(depth_fmt != WGPUTextureFormat_Undefined),
            vb_layouts.size());
    fflush(stderr);
  }

  /* Build the bind group BEFORE opening the render pass: bind-group assembly
     triggers lazy buffer uploads (push constants, dirty UBOs) whose GL-ordering
     guard flushes any open pass (see flush_if_pass_open). */
  WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroup bg = ctx->build_bind_group(shader, shader->render_bindings(), bgl);

  WGPUBuffer ibuf = nullptr;
  if (elem_() != nullptr) {
    /* Fetch (and lazily upload) the index buffer before the pass opens too. */
    ibuf = static_cast<WebGPUIndexBuf *>(elem_())->wgpu_buffer();
  }

  /* DEBUG BISECT: dump the cube's index buffer bytes (mapped from JS). */
  {
    extern bool g_capture_debug_world;
    static int s_ib_cap = 0;
    if (g_capture_debug_world && s_ib_cap == 0 && ibuf != nullptr &&
        strstr(shader->name_get().c_str(), "depth_velocity_mesh") != nullptr)
    {
      s_ib_cap = 1;
      fprintf(stderr,
              "WGPU_IBDUMP fmt=%d index_len=%u size=%llu\n",
              int(static_cast<WebGPUIndexBuf *>(elem_())->wgpu_index_format()),
              static_cast<WebGPUIndexBuf *>(elem_())->index_len_get(),
              (unsigned long long)wgpuBufferGetSize(ibuf));
      fflush(stderr);
      ctx->debug_capture_buffer(ibuf, 96);
    }
  }

  /* DEBUG: ENV.WGPU_CAP_TEX_SHADER=<substr> [+ WGPU_CAP_TEX_UNIT=<n>] captures
   * the texture bound at that unit when the named shader draws (deferred
   * readback; JS maps it via wgpu_capture_map). Must run BEFORE the pass opens
   * (read_color_sync submits). */
  if (const char *cap_pat = getenv("WGPU_CAP_TEX_SHADER")) {
    /* WGPU_CAP_TEX_SKIP=<n>: ignore the first n matching draws (capture a
     * later render's state). Each matching draw decrements. */
    static int s_cap_skip = getenv("WGPU_CAP_TEX_SKIP") ? atoi(getenv("WGPU_CAP_TEX_SKIP")) : 0;
    if (strstr(shader->name_get().c_str(), cap_pat) != nullptr && s_cap_skip-- <= 0) {
      const char *us = getenv("WGPU_CAP_TEX_UNIT");
      int unit = us ? atoi(us) : 0;
      /* WGPU_CAP_TEX_NAME resolves the flat WGSL unit via the interface (the
       * create-info slot number is NOT the bind-table index for samplers). */
      if (const char *un = getenv("WGPU_CAP_TEX_NAME")) {
        const ShaderInput *si = iface->uniform_get(un);
        if (si == nullptr) {
          static int s_name_warn = 0;
          if (s_name_warn++ < 3) {
            fprintf(stderr, "WGPU_CAPTEX no uniform '%s' in '%s'\n", un,
                    shader->name_get().c_str());
          }
          unit = -1;
        }
        else {
          unit = si->binding;
        }
      }
      WebGPUTexture *t = unit >= 0 ? ctx->tex_at(unit) : nullptr;
      if (t != nullptr && t->wgpu_texture() != nullptr) {
        fprintf(stderr,
                "WGPU_CAPTEX '%s' unit=%d %dx%d fmt=%d\n",
                shader->name_get().c_str(),
                unit,
                t->width_get(),
                t->height_get(),
                int(t->wgpu_format()));
        fflush(stderr);
        const char *ls = getenv("WGPU_CAP_TEX_LAYER");
        ctx->read_color_sync(t->wgpu_texture(), t->wgpu_format(), 0, 0, t->width_get(),
                             t->height_get(), GPU_DATA_FLOAT, 4, nullptr, ls ? atoi(ls) : 0, 0);
      }
    }
  }

  /* DEBUG: ENV.WGPU_CAP_STENCIL=<shader substr> captures the depth
   * attachment's STENCIL aspect right before that shader's first draw. */
  if (const char *cap_pat = getenv("WGPU_CAP_STENCIL")) {
    static int s_st_cap = 0;
    if (s_st_cap == 0 && strstr(shader->name_get().c_str(), cap_pat) != nullptr) {
      Texture *dt = fb->depth_tex();
      if (dt != nullptr) {
        s_st_cap = 1;
        WebGPUTexture *wdt = static_cast<WebGPUTexture *>(dt);
        fprintf(stderr,
                "WGPU_CAPSTENCIL '%s' %dx%d\n",
                shader->name_get().c_str(),
                wdt->width_get(),
                wdt->height_get());
        fflush(stderr);
        ctx->debug_capture_stencil(
            wdt->wgpu_texture(), uint32_t(wdt->width_get()), uint32_t(wdt->height_get()));
      }
    }
  }

  /* DEBUG: ENV.WGPU_CAP_SSBO_SHADER=<substr> + WGPU_CAP_SSBO_NAME=<ssbo name>
   * [+ WGPU_CAP_SSBO_BYTES=<n>] captures the named SSBO's GPU bytes when that
   * shader draws (raw-word deferred capture; JS maps via wgpu_capture_map). */
  if (const char *cap_pat = getenv("WGPU_CAP_SSBO_SHADER")) {
    static int s_ssbo_cap = 0;
    const char *nm = getenv("WGPU_CAP_SSBO_NAME");
    if (s_ssbo_cap == 0 && nm && strstr(shader->name_get().c_str(), cap_pat) != nullptr) {
      const ShaderInput *si = iface->ssbo_get(nm);
      WGPUBuffer sb = si ? ctx->ssbo_at(si->location) : nullptr;
      if (sb != nullptr) {
        s_ssbo_cap = 1;
        const char *bs = getenv("WGPU_CAP_SSBO_BYTES");
        size_t nbytes = bs ? size_t(atoi(bs)) : 256;
        nbytes = std::min(nbytes, size_t(wgpuBufferGetSize(sb)));
        fprintf(stderr,
                "WGPU_CAPSSBO '%s' ssbo='%s' slot=%d size=%llu cap=%zu\n",
                shader->name_get().c_str(),
                nm,
                si->location,
                (unsigned long long)wgpuBufferGetSize(sb),
                nbytes);
        fflush(stderr);
        ctx->debug_capture_buffer(sb, nbytes);
      }
    }
  }

  /* --- Begin render pass + record draw. --- */
  if (ctx->pending_draw_conflicts()) {
    /* This draw binds a buffer the open pass already used with different
     * writability (e.g. volume occupancy: prepass writes, material reads).
     * WebGPU usage scopes are per pass — split so both stay valid. */
    if (getenv("WGPU_LOG_RP")) {
      fprintf(stderr, "WGPU_SPLIT conflict '%s'\n", shader->name_get().c_str());
      fflush(stderr);
    }
    ctx->render_pass_end();
  }
  ctx->render_pass_ensure(*fb);
  ctx->commit_pending_draw_buffers();
  WGPURenderPassEncoder pass = ctx->render_pass();
  if (pass == nullptr) {
    if (bg) {
      wgpuBindGroupRelease(bg);
    }
    wgpuBindGroupLayoutRelease(bgl);
    return;
  }
  apply_viewport_scissor(pass, fb);
  if (stencil.enabled) {
    wgpuRenderPassEncoderSetStencilReference(pass, mst.stencil_reference);
    if (getenv("WGPU_LOG_STENCIL")) {
      fprintf(stderr,
              "WGPU_SREF '%s' ref=0x%02x test=%d spec=%llx\n",
              shader->name_get().c_str(),
              mst.stencil_reference,
              int(gst.stencil_test),
              (unsigned long long)shader->spec_hash());
      fflush(stderr);
    }
  }

  {
    static int s_mesh_state_log = 0;
    if (s_mesh_state_log < 8 && strstr(shader->name_get().c_str(), "_mesh") != nullptr) {
      s_mesh_state_log++;
      fprintf(stderr,
              "WGPU_MESHDRAW '%s' fb='%s' depth_test=%d write=0x%x blend=%d cull=%d invert=%d "
              "out_mask=0x%x colors=%d\n",
              shader->name_get().c_str(),
              fb->name_get(),
              int(gst.depth_test),
              uint(gst.write_mask),
              int(gst.blend),
              int(gst.culling_test),
              int(gst.invert_facing),
              shader->fragment_output_mask(),
              color_count);
      fflush(stderr);
    }
  }
  static int s_draw_log = 0;
  if (s_draw_log < 16) {
    fprintf(stderr,
            "WGPU_BATCH draw #%d shader='%s' verts=%d inst=%d indexed=%d bg=%p\n",
            s_draw_log++,
            shader->name_get().c_str(),
            vertex_count,
            instance_count,
            int(elem_() != nullptr),
            (void *)bg);
    fflush(stderr);
  }

  /* A draw whose bind group failed to build would be a validation error that
   * POISONS the whole command buffer at submit — skip the draw instead. */
  if (bg == nullptr && !shader->render_bindings().empty()) {
    return;
  }
  ctx->occlusion_query_draw_hook();
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  if (bg) {
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  }
  for (size_t i = 0; i < vb_buffers.size(); i++) {
    wgpuRenderPassEncoderSetVertexBuffer(pass, uint32_t(i), vb_buffers[i], 0, WGPU_WHOLE_SIZE);
  }

  if (ibuf) {
    WebGPUIndexBuf *ibo = static_cast<WebGPUIndexBuf *>(elem_());
    /* Subrange ibos share the parent's buffer; offset selects the slice. */
    wgpuRenderPassEncoderSetIndexBuffer(
        pass, ibuf, ibo->wgpu_index_format(), ibo->wgpu_index_offset_bytes(), WGPU_WHOLE_SIZE);
  }

  if (indirect_buf != nullptr) {
    /* Indirect path: DRW's draw commands generated on GPU (draw_command_generate).
     * Blender's DrawCommand(Indexed) layouts match WebGPU's (Draw|DrawIndexed)
     * Indirect layouts. Core WebGPU has no multi-draw: record one call per
     * command. Non-zero firstInstance in the commands needs the
     * `indirect-first-instance` device feature (requested by the harness). */
    WGPUBuffer ind = static_cast<WebGPUStorageBuf *>(indirect_buf)->buffer();
    if (ind != nullptr) {
      const intptr_t stride = (indirect_stride > 0) ?
                                  indirect_stride :
                                  intptr_t((elem_() != nullptr) ? 5 * sizeof(uint32_t) :
                                                                  4 * sizeof(uint32_t));
      for (int i = 0; i < indirect_count; i++) {
        const uint64_t ofs = uint64_t(indirect_offset + i * stride);
        if (elem_() != nullptr && ibuf != nullptr) {
          wgpuRenderPassEncoderDrawIndexedIndirect(pass, ind, ofs);
        }
        else {
          wgpuRenderPassEncoderDrawIndirect(pass, ind, ofs);
        }
      }
    }
  }
  else if (elem_() != nullptr) {
    if (ibuf) {
      WebGPUIndexBuf *ibo = static_cast<WebGPUIndexBuf *>(elem_());
      const uint32_t index_count = (vertex_count > 0) ? uint32_t(vertex_count) :
                                                        ibo->index_len_get();
      /* vertex_first = firstIndex for indexed draws (GPU_batch_draw_range
       * semantics); index_base_ = baseVertex for min-index-compressed IBOs.
       * Hardcoded zeros here shifted every sub-range indexed draw's attribute
       * fetches (vertex_color_facet scrambling investigation). */
      wgpuRenderPassEncoderDrawIndexed(pass,
                                       index_count,
                                       uint32_t(instance_count),
                                       uint32_t(vertex_first),
                                       ibo->index_base_get(),
                                       uint32_t(instance_first));
    }
  }
  else {
    uint32_t v_count = (vertex_count > 0) ? uint32_t(vertex_count) :
                                            (verts_(0) ? verts_(0)->vertex_len : 0);
    if (v_count > 0) {
      if (expand_points) {
        /* One 4-vertex strip quad per point; per-instance vertex stepping
         * delivers each point's attributes to its quad. */
        wgpuRenderPassEncoderDraw(pass, 4, v_count, 0, uint32_t(vertex_first));
      }
      else {
        wgpuRenderPassEncoderDraw(pass,
                                  v_count,
                                  uint32_t(instance_count),
                                  uint32_t(vertex_first),
                                  uint32_t(instance_first));
      }
    }
  }

  /* Debug capture: remember the color target of a real content draw so it can be
   * read back from JS after the render. EEVEE's final "combined" output is
   * assembled by COMPUTE film passes (which can't run on SwiftShader), so target
   * the world/background draw (WOWorld_world_world) — actual rendered color — or
   * any non-clear draw, whichever we can read. Only on a valid bind group. */
  if (bg) {
    const char *shname = shader->name_get().c_str();
    const char *fbname = fb->name_get();
    /* Capture any content draw (procedural or geometry) to a main-resolution
     * color attachment (width >= 32 excludes tiny probe/tile targets); last wins. */
    gpu::Texture *ct = fb->color_tex(0);
    const uint32_t v = (vertex_count > 0) ? uint32_t(vertex_count)
                                          : (verts_(0) ? verts_(0)->vertex_len : 0);
    /* Capture the world/background draw into the EEVEE final "combined"
     * framebuffer — a fullscreen triangle that paints the rendered world color,
     * which is the actual graphics output we can read back. We deliberately skip
     * other combined-fb draws: the per-sample "eevee_renderpass_clear" zeroes the
     * target (capturing after it yields a black frame), and the off-screen
     * lightprobe "Capture.Combine" pass is rendered black before the world is lit.
     * (EEVEE's full film composite runs in COMPUTE passes that exceed SwiftShader's
     * baseline limits, so the world draw is the meaningful capture here.) */
    const bool is_combined = fbname && strstr(fbname, "combined_fb_") != nullptr;
    const bool is_world = shname && strstr(shname, "world") != nullptr;
    /* Disabled: the debug capture is now driven by WebGPUContext::read_color_sync
     * off Blender's own Film::read_result of the composited combined texture (the
     * real GPU runs the full compute film pipeline). Capturing the world draw here
     * was a SwiftShader-era workaround and would clobber the combined capture. */
    extern bool g_capture_debug_world;
    /* Debug bisect: capture the gbuffer NORMAL attachment (slot 2 of gbuffer_fb_)
     * right after the gbuffer content draw, to check whether the lit-face pepper
     * speckle originates in the gbuffer itself or in the deferred eval. */
    extern bool wgpu_env_cap_gbuf();
    if (wgpu_env_cap_gbuf() && shname && strstr(shname, "deferred_light") != nullptr) {
      /* Bisect: capture the direct radiance output of the light eval. */
      const char *img_name = getenv("WGPU_CAP_GBUF_IMG");
      const ShaderInput *si = iface->uniform_get(img_name ? img_name : "direct_radiance_1_img");
      WebGPUTexture *nt = (si && si->binding >= 0) ? ctx->image_at(si->binding) : nullptr;
      if (nt != nullptr) {
        ctx->set_capture_target(nt);
      }
    }
    /* Debug bisect variant: capture the mesh gbuffer draw instead. */
    const bool is_mesh = shname && strstr(shname, "depth_velocity_mesh") != nullptr;
    if (false && g_capture_debug_world && is_mesh) {
      /* Depth bisect: capture the prepass DEPTH attachment. */
      ct = fb->depth_tex();
    }
    if (false && g_capture_debug_world && is_mesh && ct != nullptr) {
      ctx->set_capture_target(static_cast<WebGPUTexture *>(ct));
      static int s_mcap_log = 0;
      if (s_mcap_log < 4) {
        s_mcap_log++;
        fprintf(stderr,
                "WGPU_CAPTURE mesh-target fb='%s' tex=%dx%d fmt=%d\n",
                fbname ? fbname : "?",
                ct->width_get(),
                ct->height_get(),
                int(static_cast<WebGPUTexture *>(ct)->wgpu_format()));
        fflush(stderr);
      }
    }
    if (false && g_capture_debug_world && is_combined && is_world && v > 0) {
      /* Debug bisect: capture the color ATTACHMENT the world draw renders into
       * (the render-buffer combined texture that film_comp then reads). Prefer a
       * real render-pass color image only when it is not the 1x1 dummy. */
      WebGPUTexture *rp = ctx->bound_storage_image(shader, "rp_color_img");
      if (rp && rp->width_get() <= 1) {
        rp = nullptr;
      }
      WebGPUTexture *cap = ct ? static_cast<WebGPUTexture *>(ct) : rp;
      if (cap) {
        ctx->set_capture_target(cap);
        static int s_cap_log = 0;
        if (s_cap_log < 16) {
          fprintf(stderr,
                  "WGPU_CAPTURE target fb='%s' shader='%s' src=%s %dx%d verts=%u\n",
                  fbname ? fbname : "?",
                  shname ? shname : "?",
                  rp ? "rp_color_img" : "color_attachment",
                  cap->width_get(),
                  cap->height_get(),
                  v);
          fflush(stderr);
          s_cap_log++;
        }
      }
    }
  }

  if (bg) {
    wgpuBindGroupRelease(bg);
  }
  wgpuBindGroupLayoutRelease(bgl);
}

void webgpu_immediate_draw(GPUPrimType prim_type,
                           const GPUVertFormat &format,
                           WGPUBuffer vbo,
                           uint vertex_count)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx == nullptr || ctx->device() == nullptr || vbo == nullptr || vertex_count == 0) {
    return;
  }
  WebGPUShader *shader = static_cast<WebGPUShader *>(ctx->shader);
  if (shader == nullptr || !shader->is_valid() || shader->vertex_module() == nullptr ||
      shader->fragment_module() == nullptr || shader->interface == nullptr)
  {
    static int s_imm_skip = 0;
    if (shader && s_imm_skip < 40) {
      s_imm_skip++;
      fprintf(stderr, "WGPU_IMM skipped '%s' (invalid/missing modules)\n",
              shader->name_get().c_str());
      fflush(stderr);
    }
    return;
  }
  WebGPUFrameBuffer *fb = static_cast<WebGPUFrameBuffer *>(ctx->active_fb);
  if (fb == nullptr) {
    return;
  }
  WebGPUShaderInterface *iface = static_cast<WebGPUShaderInterface *>(shader->interface);

  /* Vertex layout from the immediate format. */
  std::vector<WGPUVertexAttribute> attrs;
  for (uint ai = 0; ai < format.attr_len; ai++) {
    const GPUVertAttr &attr = format.attrs[ai];
    const char *name = GPU_vertformat_attr_name_get(&format, &attr, 0);
    const ShaderInput *in = iface->attr_get(name);
    if (in == nullptr || in->location < 0) {
      continue;
    }
    WGPUVertexAttribute wa = {};
    wa.format = to_wgpu_vertex_format(
        attr.type.comp_type(), attr.type.comp_len(), attr.type.fetch_mode());
    wa.offset = attr.offset;
    wa.shaderLocation = uint32_t(in->location);
    attrs.push_back(wa);
  }
  WGPUVertexBufferLayout layouts[2] = {};
  uint32_t layout_count = 0;
  if (!attrs.empty()) {
    layouts[layout_count].arrayStride = format.stride;
    layouts[layout_count].stepMode = WGPUVertexStepMode_Vertex;
    layouts[layout_count].attributeCount = attrs.size();
    layouts[layout_count].attributes = attrs.data();
    layout_count++;
  }
  /* Pad shader input locations the immediate format doesn't cover (see
   * record_draw — WebGPU rejects unfed @locations). */
  std::vector<WGPUVertexAttribute> pad_attrs;
  {
    uint32_t covered = 0;
    for (const WGPUVertexAttribute &a : attrs) {
      covered |= 1u << a.shaderLocation;
    }
    for (const WgslVertexInput &vin : shader->vertex_inputs()) {
      if (vin.location < 32 && !(covered & (1u << vin.location))) {
        WGPUVertexAttribute wa = {};
        wa.format = vin.format;
        wa.offset = 0;
        wa.shaderLocation = vin.location;
        pad_attrs.push_back(wa);
      }
    }
    if (!pad_attrs.empty() && ctx->null_attr_buffer() != nullptr) {
      layouts[layout_count].arrayStride = 0;
      layouts[layout_count].stepMode = WGPUVertexStepMode_Vertex;
      layouts[layout_count].attributeCount = pad_attrs.size();
      layouts[layout_count].attributes = pad_attrs.data();
      layout_count++;
    }
  }

  /* --- Pipeline key/state (mirrors record_draw). --- */
  const int color_count = fb->color_attachment_count();
  WGPUTextureFormat color_fmt[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
  for (int i = 0; i < color_count; i++) {
    color_fmt[i] = fb->color_format(i);
  }
  const WGPUTextureFormat depth_fmt = fb->depth_format();
  WGPUPrimitiveTopology topo = to_wgpu_topology(prim_type);
  int vp_rect[4] = {0, 0, 0, 0};
  fb->viewport_get(vp_rect);
  if (vp_rect[2] <= 0 || vp_rect[3] <= 0) {
    const int2 fb_size = fb->size_get();
    vp_rect[2] = fb_size.x;
    vp_rect[3] = fb_size.y;
  }
  const bool expand_points = (topo == WGPUPrimitiveTopology_PointList) &&
                             shader->can_expand_points() && vp_rect[2] > 0 && vp_rect[3] > 0;
  if (topo == WGPUPrimitiveTopology_PointList && getenv("WGPU_LOG_POINTS")) {
    fprintf(stderr,
            "WGPU_PTS imm '%s' expand=%d can=%d vp=%dx%d\n",
            shader->name_get().c_str(),
            int(expand_points),
            int(shader->can_expand_points()),
            vp_rect[2], vp_rect[3]);
    fflush(stderr);
  }
  if (expand_points) {
    /* Point-sprite emulation: per-instance quads (see record_draw). */
    topo = WGPUPrimitiveTopology_TriangleStrip;
    for (uint32_t li = 0; li < layout_count; li++) {
      layouts[li].stepMode = WGPUVertexStepMode_Instance;
    }
  }
  const GPUState &gst = ctx->state_manager->state;
  const GPUDepthTest depth_test = GPUDepthTest(gst.depth_test);
  const bool depth_write = (gst.write_mask & GPU_WRITE_DEPTH) != 0;
  const GPUBlend blend = GPUBlend(gst.blend);
  const GPUFaceCullTest cull = GPUFaceCullTest(gst.culling_test);
  const GPUStateMutable &mst = ctx->state_manager->mutable_state;
  const WGPUStencilParams stencil = to_wgpu_stencil(gst, mst, depth_fmt);

  uint64_t key = 1469598103934665603ull;
  void *sh_ptr = shader;
  hash_append(key, &sh_ptr, sizeof(sh_ptr));
  const uint64_t spec_h = shader->spec_hash();
  hash_append(key, &spec_h, sizeof(spec_h));
  hash_append(key, &topo, sizeof(topo));
  hash_append(key, &color_count, sizeof(color_count));
  hash_append(key, color_fmt, sizeof(WGPUTextureFormat) * color_count);
  hash_append(key, &depth_fmt, sizeof(depth_fmt));
  hash_append(key, &depth_test, sizeof(depth_test));
  hash_append(key, &depth_write, sizeof(depth_write));
  /* The COLOR write mask shapes the pipeline too — without it in the key a
   * depth-only prepass (wireframe/x-ray hidden-line, color writes masked)
   * reuses a color-writing pipeline and fills geometry black. */
  const uint32_t write_mask_bits = uint32_t(gst.write_mask);
  hash_append(key, &write_mask_bits, sizeof(write_mask_bits));
  hash_append(key, &blend, sizeof(blend));
  hash_append(key, &cull, sizeof(cull));
  hash_append(key, &stencil, sizeof(stencil));
  for (uint32_t li = 0; li < layout_count; li++) {
    hash_append(key, &layouts[li].arrayStride, sizeof(layouts[li].arrayStride));
    for (size_t a = 0; a < layouts[li].attributeCount; a++) {
      hash_append(key, &layouts[li].attributes[a], sizeof(WGPUVertexAttribute));
    }
  }
  hash_append(key, &expand_points, sizeof(expand_points));
  if (expand_points) {
    hash_append(key, &vp_rect[2], sizeof(int) * 2);
  }

  WGPURenderPipeline pipeline = ctx->render_pipeline_get(key);
  if (pipeline == nullptr) {
    WGPUColorTargetState targets[GPU_FB_MAX_COLOR_ATTACHMENT] = {};
    WGPUBlendState blend_state = {};
    const bool has_blend = to_wgpu_blend(blend, blend_state);
    const uint32_t out_mask = shader->fragment_output_mask();
    WGPUColorWriteMask color_write = WGPUColorWriteMask_None;
    if (gst.write_mask & GPU_WRITE_RED) {
      color_write |= WGPUColorWriteMask_Red;
    }
    if (gst.write_mask & GPU_WRITE_GREEN) {
      color_write |= WGPUColorWriteMask_Green;
    }
    if (gst.write_mask & GPU_WRITE_BLUE) {
      color_write |= WGPUColorWriteMask_Blue;
    }
    if (gst.write_mask & GPU_WRITE_ALPHA) {
      color_write |= WGPUColorWriteMask_Alpha;
    }
    for (int i = 0; i < color_count; i++) {
      targets[i].format = color_fmt[i];
      targets[i].writeMask = (out_mask & (1u << i)) ? color_write : WGPUColorWriteMask_None;
      targets[i].blend = has_blend ? &blend_state : nullptr;
    }
    std::vector<WGPUConstantEntry> vert_consts, frag_consts;
    shader->spec_entries(shader->vertex_wgsl(), vert_consts);
    shader->spec_entries(shader->fragment_wgsl(), frag_consts);
    if (expand_points) {
      WGPUConstantEntry e = {};
      e.key = {"240", WGPU_STRLEN};
      e.value = double(vp_rect[2]);
      vert_consts.push_back(e);
      e.key = {"241", WGPU_STRLEN};
      e.value = double(vp_rect[3]);
      vert_consts.push_back(e);
      e.key = {"242", WGPU_STRLEN};
      e.value = 1.0;
      vert_consts.push_back(e);
    }

    WGPUFragmentState frag = {};
    frag.module = shader->fragment_module();
    frag.entryPoint = {"main", WGPU_STRLEN};
    frag.targetCount = uint32_t(color_count);
    frag.targets = color_count ? targets : nullptr;
    frag.constantCount = frag_consts.size();
    frag.constants = frag_consts.empty() ? nullptr : frag_consts.data();

    WGPUDepthStencilState ds = {};
    bool has_depth = depth_fmt != WGPUTextureFormat_Undefined;
    if (has_depth) {
      ds.format = depth_fmt;
      ds.depthWriteEnabled = depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
      ds.depthCompare = to_wgpu_depth_compare(depth_test);
      ds.stencilFront = stencil.front;
      ds.stencilBack = stencil.back;
      ds.stencilReadMask = stencil.read_mask;
      ds.stencilWriteMask = stencil.write_mask;
    }

    WGPUBindGroupLayout bgl_explicit = ctx->make_bind_group_layout(
        shader->render_bindings(), false, shader->interface);
    WGPUPipelineLayout pipe_layout = ctx->make_pipeline_layout(bgl_explicit);

    WGPURenderPipelineDescriptor rpd = {};
    rpd.label = {shader->name_get().c_str(), WGPU_STRLEN};
    rpd.layout = pipe_layout;
    rpd.vertex.module = shader->vertex_module();
    rpd.vertex.entryPoint = {"main", WGPU_STRLEN};
    rpd.vertex.bufferCount = layout_count;
    rpd.vertex.buffers = layout_count ? layouts : nullptr;
    rpd.vertex.constantCount = vert_consts.size();
    rpd.vertex.constants = vert_consts.empty() ? nullptr : vert_consts.data();
    rpd.primitive.topology = topo;
    /* The vertex wrapper negates gl_Position.y (GL bottom-up convention), which
     * inverts screen-space winding — so the GL default CCW maps to CW here. */
    rpd.primitive.frontFace = gst.invert_facing ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
    rpd.primitive.cullMode = (cull == GPU_CULL_FRONT) ? WGPUCullMode_Front :
                             (cull == GPU_CULL_BACK)  ? WGPUCullMode_Back :
                                                        WGPUCullMode_None;
    rpd.multisample.count = 1;
    rpd.multisample.mask = 0xFFFFFFFFu;
    rpd.fragment = &frag;
    rpd.depthStencil = has_depth ? &ds : nullptr;

    pipeline = wgpuDeviceCreateRenderPipeline(ctx->device(), &rpd);
    if (pipe_layout) {
      wgpuPipelineLayoutRelease(pipe_layout);
    }
    if (bgl_explicit) {
      wgpuBindGroupLayoutRelease(bgl_explicit);
    }
    if (pipeline == nullptr) {
      fprintf(stderr, "WGPU_IMM pipeline creation failed for '%s'\n",
              shader->name_get().c_str());
      fflush(stderr);
      return;
    }
    ctx->render_pipeline_put(key, pipeline);
  }

  /* Bind group BEFORE opening the pass (lazy uploads must not land mid-pass). */
  WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroup bg = ctx->build_bind_group(shader, shader->render_bindings(), bgl);
  if (bg == nullptr && !shader->render_bindings().empty()) {
    wgpuBindGroupLayoutRelease(bgl);
    return;
  }

  if (ctx->pending_draw_conflicts()) {
    ctx->render_pass_end();
  }
  ctx->render_pass_ensure(*fb);
  ctx->commit_pending_draw_buffers();
  WGPURenderPassEncoder pass = ctx->render_pass();
  if (pass == nullptr) {
    if (bg) {
      wgpuBindGroupRelease(bg);
    }
    wgpuBindGroupLayoutRelease(bgl);
    return;
  }
  apply_viewport_scissor(pass, fb);
  if (stencil.enabled) {
    wgpuRenderPassEncoderSetStencilReference(pass, mst.stencil_reference);
  }
  ctx->occlusion_query_draw_hook();
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  if (bg) {
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  }
  uint32_t vb_slot = 0;
  if (!attrs.empty()) {
    wgpuRenderPassEncoderSetVertexBuffer(pass, vb_slot++, vbo, 0, WGPU_WHOLE_SIZE);
  }
  if (!pad_attrs.empty() && vb_slot < layout_count) {
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, vb_slot++, ctx->null_attr_buffer(), 0, WGPU_WHOLE_SIZE);
  }
  if (expand_points) {
    wgpuRenderPassEncoderDraw(pass, 4, vertex_count, 0, 0);
  }
  else {
    wgpuRenderPassEncoderDraw(pass, vertex_count, 1, 0, 0);
  }

  if (bg) {
    wgpuBindGroupRelease(bg);
  }
  wgpuBindGroupLayoutRelease(bgl);
}

}  // namespace blender::gpu
