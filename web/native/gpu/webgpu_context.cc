/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "BLI_math_half.hh"
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#  include <emscripten/emscripten.h>
#endif

#include "GPU_framebuffer.hh"
#include "native/ghost/GHOST_WebPresent.hh"
#include "native/ghost/GHOST_WebJobs.hh"
#include "GPU_texture.hh"

#include "webgpu_context.hh"
#include "webgpu_shader.hh"
#include "webgpu_shader_interface.hh"
#include "webgpu_immediate.hh"
#include "webgpu_state.hh"
#include "webgpu_texture.hh"

#ifdef __EMSCRIPTEN__
#  include <emscripten/emscripten.h>
/* Declared in webgpu/webgpu.h on Emscripten; returns the device created in JS
 * (Module.preinitializedWebGPUDevice). */
#endif

/* --- Debug pixel capture --------------------------------------------------
 * Copy a rendered color texture into a persistent (never-released) buffer during
 * the render, so JS can map+read it AFTER Blender quits (the wasm runtime stays
 * alive; synchronous readback is impossible mid-render with no JSPI). The C
 * exports below are driven by the harness after "Blender quit". */
namespace blender::gpu {
/* DEBUG BISECT: when true, capture the last world draw's color attachment
 * (render_pass_end path) instead of the film output (read_color_sync path), to
 * tell whether the world pass writes red or the film compute loses it.
 * Referenced via `extern` from webgpu_batch.cc. */
long long g_stat_tex_bytes = 0;
int g_stat_tex_count = 0;
bool g_capture_debug_world = false;
/* When true, the debug gbuffer-normal capture in webgpu_batch.cc owns the
 * capture buffer: the film read_color_sync capture is skipped so it can't
 * overwrite the bisect target. Also enabled by ENV.WGPU_CAP_GBUF=1. */
bool g_capture_debug_gbuf = false;
bool wgpu_env_cap_gbuf()
{
  static const bool on = getenv("WGPU_CAP_GBUF") != nullptr;
  return on || g_capture_debug_gbuf;
}
}  // namespace blender::gpu

namespace {
WGPUBuffer g_capture_buf = nullptr;
size_t g_capture_cap = 0;
uint32_t g_capture_w = 0, g_capture_h = 0, g_capture_bpr = 0, g_capture_bpp = 0;
uint8_t *g_capture_pixels = nullptr;
volatile int g_capture_ready = 0; /* 0 = pending, 1 = ok, -1 = fail */
}  // namespace

namespace blender::gpu {

/* All live contexts, for cross-context scrubbing of freed texture wrappers. */
static std::vector<WebGPUContext *> g_all_contexts;

void WebGPUContext::scrub_texture_all_contexts(WebGPUTexture *tex)
{
  for (WebGPUContext *ctx : g_all_contexts) {
    ctx->scrub_texture(tex);
  }
}

void WebGPUContext::clear_bind_group_cache_all_contexts()
{
  for (WebGPUContext *ctx : g_all_contexts) {
    ctx->clear_bind_group_cache();
  }
}

static uint32_t align256(uint32_t v)
{
  return (v + 255u) & ~255u;
}

/* The (singleton, in the browser) WGPUInstance, needed for wgpuInstanceWaitAny.
 * Must be created with the TimedWaitAny feature so a blocking wait with a
 * non-zero timeout is allowed (emdawnwebgpu implements it via JSPI). Cached. */
static WGPUInstance webgpu_instance()
{
  static WGPUInstance s_instance = nullptr;
  if (s_instance == nullptr) {
    WGPUInstanceFeatureName feats[] = {WGPUInstanceFeatureName_TimedWaitAny};
    WGPUInstanceLimits ilimits = {};
    ilimits.timedWaitAnyMaxCount = 8;
    WGPUInstanceDescriptor idesc = {};
    /* Canvas presentation and async readback do not need a blocking wait.
     * Requesting TimedWaitAny without JSPI/Asyncify makes emdawnwebgpu reject
     * instance creation, including the canvas surface. */
    if (emscripten_has_asyncify()) {
      idesc.requiredFeatureCount = 1;
      idesc.requiredFeatures = feats;
      idesc.requiredLimits = &ilimits;
    }
    s_instance = wgpuCreateInstance(&idesc);
  }
  return s_instance;
}

WebGPUContext::WebGPUContext(GHOST_IWindow * /*ghost_window*/,
                             GHOST_IContext * /*ghost_context*/)
{
#ifdef __EMSCRIPTEN__
  /* Acquire the JS-provided device ONCE and share it across all WebGPU contexts
   * (Blender creates more than one; emscripten_webgpu_get_device only hands the
   * device over on the first/right thread, and Module.preinitializedWebGPUDevice
   * isn't visible on every thread under PROXY_TO_PTHREAD). The WGPUDevice handle
   * lives in shared wasm memory, but its JS object is worker-local. GPU jobs
   * are dispatched to this same owning worker by GHOST_WebJobs.
   * Probe guards against the device-less headless case (plain node, no device). */
  static WGPUDevice s_shared_device = nullptr;
  if (s_shared_device == nullptr) {
    const int has_device = EM_ASM_INT({
      return (typeof Module !== "undefined" && Module["preinitializedWebGPUDevice"]) ? 1 : 0;
    });
    if (has_device) {
      s_shared_device = emscripten_webgpu_get_device();
    }
  }
  device_ = s_shared_device;
#endif
  if (device_) {
    queue_ = wgpuDeviceGetQueue(device_);
    fprintf(stderr, "WEBGPU_CONTEXT device=%p queue=%p (real device acquired)\n",
            (void *)device_, (void *)queue_);
  }
  else {
    fprintf(stderr, "WEBGPU_CONTEXT no device (degraded headless mode)\n");
  }
  fflush(stderr);
  /* TODO: a default framebuffer wrapping the canvas swapchain. For headless
   * render-to-texture (Cycles-style output, Workbench bring-up) this stays a
   * dummy until surface presentation is wired. */
  state_manager = new WebGPUStateManager();
  {
    WebGPUFrameBuffer *bb = new WebGPUFrameBuffer("WebGPUBackbuffer");
    bb->set_is_backbuffer(true);
    back_left = active_fb = bb;
  }
  imm = new WebGPUImmediate();
  g_all_contexts.push_back(this);
}

WebGPUContext::~WebGPUContext()
{
  clear_bind_group_cache();
  clear_snapshot_cache();
  for (WGPUBuffer &buffer : bound_tex_buffer_) {
    table_set(buffer, nullptr);
  }
  if (readback_buffer_) wgpuBufferRelease(readback_buffer_);
  g_all_contexts.erase(std::remove(g_all_contexts.begin(), g_all_contexts.end(), this),
                       g_all_contexts.end());
  render_pass_end();
  if (encoder_) {
    wgpuCommandEncoderRelease(encoder_);
  }
  if (default_sampler_) {
    wgpuSamplerRelease(default_sampler_);
  }
  if (capture_src_) {
    wgpuTextureRelease(capture_src_);
  }
  for (auto &kv : render_pipelines_) {
    if (kv.second) {
      wgpuRenderPipelineRelease(kv.second);
    }
  }
  for (auto &kv : compute_pipelines_) {
    if (kv.second) {
      wgpuComputePipelineRelease(kv.second);
    }
  }
  free_resources();
  delete state_manager;
  state_manager = nullptr;
  if (queue_) {
    wgpuQueueRelease(queue_);
  }
  /* device_ is the shared, JS-owned device (see ctor) — do NOT release it here;
   * other contexts share it and JS still holds the reference. */
  if (false && device_) {
    wgpuDeviceRelease(device_);
  }
}

void WebGPUContext::set_capture_target(WebGPUTexture *t)
{
  if (t == nullptr) {
    return;
  }
  WGPUTexture h = t->wgpu_texture();
  if (h == nullptr) {
    return;
  }
  /* Last-wins: a later draw to the final combined framebuffer should replace an
   * earlier capture (e.g. the world/background draw over the initial clear), so
   * the read-back reflects the fully rendered frame, not the first content draw.
   * AddRef now (texture is alive at draw time) so the handle survives the pooled
   * gpu::Texture being released/reused before we record the copy; release any
   * previously-held handle. */
  wgpuTextureAddRef(h);
  if (capture_src_) {
    wgpuTextureRelease(capture_src_);
  }
  capture_src_ = h;
  capture_w_ = uint32_t(std::max(t->width_get(), 1));
  capture_h_ = uint32_t(std::max(t->height_get(), 1));
  capture_fmt_ = webgpu_texture_format(t->format_get());
  capture_pending_ = true;
}

void WebGPUContext::debug_capture_buffer(WGPUBuffer buf, size_t size)
{
  if (buf == nullptr || device_ == nullptr || size == 0) {
    return;
  }
  const size_t need = (size + 255) & ~size_t(255);
  if (g_capture_buf == nullptr || g_capture_cap < need) {
    if (g_capture_buf) {
      wgpuBufferRelease(g_capture_buf);
    }
    WGPUBufferDescriptor bd = {};
    bd.size = need;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    g_capture_buf = wgpuDeviceCreateBuffer(device_, &bd);
    g_capture_cap = need;
  }
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr || g_capture_buf == nullptr) {
    return;
  }
  wgpuCommandEncoderCopyBufferToBuffer(enc, buf, 0, g_capture_buf, 0, (size + 3) & ~size_t(3));
  /* h == 1 signals "raw buffer" to the JS mapper (dumped as uint32 words). */
  g_capture_w = uint32_t(size);
  g_capture_h = 1;
  g_capture_bpr = uint32_t(size);
  g_capture_bpp = 1;
  g_capture_ready = 0;
  flush_encoder();
}

void WebGPUContext::debug_capture_stencil(WGPUTexture tex, uint32_t w, uint32_t h)
{
  if (tex == nullptr || device_ == nullptr) {
    return;
  }
  const uint32_t bpr = (w + 255u) & ~255u; /* Stencil8: 1 byte/px, 256B row align. */
  const size_t need = size_t(bpr) * h;
  if (g_capture_buf == nullptr || g_capture_cap < need) {
    if (g_capture_buf) {
      wgpuBufferRelease(g_capture_buf);
    }
    WGPUBufferDescriptor bd = {};
    bd.size = (need + 255) & ~size_t(255);
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    g_capture_buf = wgpuDeviceCreateBuffer(device_, &bd);
    g_capture_cap = bd.size;
  }
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr || g_capture_buf == nullptr) {
    return;
  }
  WGPUTexelCopyTextureInfo src = {};
  src.texture = tex;
  src.aspect = WGPUTextureAspect_StencilOnly;
  WGPUTexelCopyBufferInfo dst = {};
  dst.buffer = g_capture_buf;
  dst.layout.bytesPerRow = bpr;
  dst.layout.rowsPerImage = h;
  WGPUExtent3D ext = {w, h, 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
  g_capture_w = w;
  g_capture_h = h;
  g_capture_bpr = bpr;
  g_capture_bpp = 1;
  g_capture_ready = 0;
  flush_encoder();
}

WGPUCommandEncoder WebGPUContext::ensure_encoder()
{
  if (encoder_ == nullptr && device_ != nullptr) {
    WGPUCommandEncoderDescriptor desc = {};
    encoder_ = wgpuDeviceCreateCommandEncoder(device_, &desc);
  }
  return encoder_;
}

void WebGPUContext::render_pass_ensure(WebGPUFrameBuffer &fb)
{
  if (render_pass_ != nullptr && render_pass_fb_ == &fb) {
    return;
  }
  if (render_pass_ != nullptr) {
    extern int g_stat_fbswitch;
    g_stat_fbswitch++;
  }
  render_pass_end();
  if (ensure_encoder() == nullptr) {
    return;
  }
  render_pass_ = fb.begin_render_pass(encoder_);
  render_pass_fb_ = render_pass_ ? &fb : nullptr;
  extern int g_stat_passes;
  g_stat_passes++;
}

void WebGPUContext::render_pass_end()
{
  if (render_pass_) {
    /* An occlusion query spanning this pass must close before the pass ends
     * (it re-opens in the next pass while the pool's query is still pending). */
    if (occ_query_open_in_pass_) {
      wgpuRenderPassEncoderEndOcclusionQuery(render_pass_);
      occ_query_open_in_pass_ = false;
    }
    wgpuRenderPassEncoderEnd(render_pass_);
    wgpuRenderPassEncoderRelease(render_pass_);
    render_pass_ = nullptr;
    render_pass_fb_ = nullptr;
    pass_buffer_usage_.clear();

    /* Debug capture: copy the captured framebuffer's color into the persistent
     * readback buffer NOW, right after its render pass ends — the texture is
     * still alive (pooled targets get freed/reused soon after, so a deferred copy
     * at the final submit would read garbage). Recorded into the same encoder, so
     * it executes after the just-ended pass's draws. capture_pending_ is set by
     * set_capture_target for each new target; copying here (and clearing it) means
     * the LAST draw to the captured framebuffer wins, so we read the fully
     * rendered combined frame rather than the initial clear. */
    if (encoder_ && capture_src_ && capture_pending_ && device_) {
      const uint32_t w = capture_w_, h = capture_h_;
      const bool is_depth = capture_fmt_ == WGPUTextureFormat_Depth32FloatStencil8 ||
                            capture_fmt_ == WGPUTextureFormat_Depth32Float;
      const uint32_t bpp = is_depth ? 4 : webgpu_format_bytes_per_pixel(capture_fmt_);
      const uint32_t bpr = align256(w * bpp);
      const size_t need = size_t(bpr) * h;
      if (g_capture_buf == nullptr || g_capture_cap < need) {
        if (g_capture_buf) {
          wgpuBufferRelease(g_capture_buf);
        }
        WGPUBufferDescriptor bd = {};
        bd.size = need;
        bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        g_capture_buf = wgpuDeviceCreateBuffer(device_, &bd);
        g_capture_cap = need;
      }
      if (g_capture_buf) {
        WGPUTexelCopyTextureInfo src = {};
        src.texture = capture_src_;
        src.aspect = is_depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo dst = {};
        dst.buffer = g_capture_buf;
        dst.layout.bytesPerRow = bpr;
        dst.layout.rowsPerImage = h;
        WGPUExtent3D ext = {w, h, 1};
        wgpuCommandEncoderCopyTextureToBuffer(encoder_, &src, &dst, &ext);
        g_capture_w = w;
        g_capture_h = h;
        g_capture_bpr = bpr;
        g_capture_bpp = bpp;
        capture_pending_ = false;
      }
    }

    /* Passes accumulate in ONE encoder; the encoder is submitted at present,
     * at explicit submit()/flush points, and by the upload GL-order guards
     * (flush_if_pass_open). Historical note: we used to submit per pass so one
     * invalid command couldn't poison every other pass's draws, but at ~90
     * passes/frame the per-submit JS/browser round-trip made the GUI
     * pathologically slow, and runtime validation errors are gone now. This is
     * only correct because hot-path buffer updates write FRESH buffers
     * (uniform arena / copy-on-write) — a queue write can then never
     * retroactively change what an already-recorded pass reads; the remaining
     * in-place writes (texture uploads, shadow-less sub-updates) keep the
     * flush guard. */
  }
}

/* Per-present perf counters (all contexts; printed by present_backbuffer). */
int g_stat_submits = 0;
int g_stat_passes = 0;
int g_stat_bindgroups = 0;
int g_stat_flushes = 0;  /* passes ended by an upload GL-order guard */
int g_stat_fbswitch = 0; /* passes ended by binding a different framebuffer */
int g_stat_bg_hits = 0;  /* bind-group cache hits */

/* Tally guard flushes by call-site tag (string identity — pass literals). */
static std::map<std::string, int> g_flush_whys;
void webgpu_stat_flush_why(const char *why)
{
  g_flush_whys[why]++;
}
void webgpu_stat_flush_dump()
{
  std::string s;
  for (auto &kv : g_flush_whys) {
    s += kv.first + "=" + std::to_string(kv.second) + " ";
  }
  fprintf(stderr, "WGPU_FLUSH_WHY %s\n", s.c_str());
  fflush(stderr);
  g_flush_whys.clear();
}

void WebGPUContext::flush_encoder()
{
  if (encoder_ == nullptr) {
    return;
  }
  g_stat_submits++;
  WGPUCommandBufferDescriptor cb_desc = {};
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder_, &cb_desc);
  if (queue_ && cmd) {
    wgpuQueueSubmit(queue_, 1, &cmd);
  }
  if (cmd) {
    wgpuCommandBufferRelease(cmd);
  }
  wgpuCommandEncoderRelease(encoder_);
  encoder_ = nullptr;
}

void WebGPUContext::submit()
{
  render_pass_end();
  flush_encoder();
}

WGPUSampler WebGPUContext::default_sampler()
{
  if (default_sampler_ == nullptr && device_ != nullptr) {
    WGPUSamplerDescriptor d = {};
    d.addressModeU = WGPUAddressMode_Repeat;
    d.addressModeV = WGPUAddressMode_Repeat;
    d.addressModeW = WGPUAddressMode_Repeat;
    d.magFilter = WGPUFilterMode_Linear;
    d.minFilter = WGPUFilterMode_Linear;
    d.mipmapFilter = WGPUMipmapFilterMode_Linear;
    d.maxAnisotropy = 1;
    d.lodMaxClamp = 32.0f;
    default_sampler_ = wgpuDeviceCreateSampler(device_, &d);
  }
  return default_sampler_;
}

WGPUSampler WebGPUContext::sampler_for_state(GPUSamplerState state)
{
  if (device_ == nullptr) {
    return nullptr;
  }
  /* Custom/internal states (icon, custom) keep the legacy default. */
  if (state.type != GPU_SAMPLER_STATE_TYPE_PARAMETERS) {
    return default_sampler();
  }
  const uint32_t key = uint32_t(state.filtering) | (uint32_t(state.extend_x) << 16) |
                       (uint32_t(state.extend_yz) << 24);
  if (WGPUSampler *cached = state_samplers_.lookup_ptr(key)) {
    return *cached;
  }
  auto to_address = [](GPUSamplerExtendMode m) {
    switch (m) {
      case GPU_SAMPLER_EXTEND_MODE_REPEAT:
        return WGPUAddressMode_Repeat;
      case GPU_SAMPLER_EXTEND_MODE_MIRRORED_REPEAT:
        return WGPUAddressMode_MirrorRepeat;
      default:
        /* EXTEND and CLAMP_TO_BORDER both clamp; border color is not supported
         * in WebGPU (transparent-black border approximated by edge clamp). */
        return WGPUAddressMode_ClampToEdge;
    }
  };
  const bool linear = (state.filtering & GPU_SAMPLER_FILTERING_LINEAR) != 0;
  const bool mip = (state.filtering & GPU_SAMPLER_FILTERING_MIPMAP) != 0;
  const bool aniso = (state.filtering & GPU_SAMPLER_FILTERING_ANISOTROPIC_16) != 0;
  WGPUSamplerDescriptor d = {};
  d.addressModeU = to_address(state.extend_x);
  d.addressModeV = to_address(state.extend_yz);
  d.addressModeW = to_address(state.extend_yz);
  d.magFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
  d.minFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
  d.mipmapFilter = mip ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
  /* GL semantics: without the MIPMAP flag sampling stays on the base level. */
  d.lodMaxClamp = mip ? 32.0f : 0.0f;
  /* Anisotropy requires all-linear filters in WebGPU. */
  d.maxAnisotropy = (aniso && linear && mip) ? 4 : 1;
  WGPUSampler smp = wgpuDeviceCreateSampler(device_, &d);
  state_samplers_.add(key, smp);
  return smp;
}

WGPUTextureView WebGPUContext::dummy_storage_view(WGPUTextureFormat format,
                                                  WGPUTextureViewDimension dim)
{
  const uint64_t key = (uint64_t(format) << 8) | uint64_t(dim);
  auto it = dummy_storage_views_.find(key);
  if (it != dummy_storage_views_.end()) {
    return it->second;
  }
  if (device_ == nullptr) {
    return nullptr;
  }
  WGPUTextureDescriptor td = {};
  td.dimension = (dim == WGPUTextureViewDimension_3D) ? WGPUTextureDimension_3D :
                                                        WGPUTextureDimension_2D;
  td.size = {1, 1, 1};
  td.format = format;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.usage = WGPUTextureUsage_StorageBinding;
  WGPUTexture tex = wgpuDeviceCreateTexture(device_, &td);
  WGPUTextureView view = nullptr;
  if (tex) {
    WGPUTextureViewDescriptor vd = {};
    vd.format = format;
    vd.dimension = dim;
    vd.baseMipLevel = 0;
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = 1;
    view = wgpuTextureCreateView(tex, &vd);
  }
  dummy_storage_views_[key] = view;
  return view;
}

/* Completed occlusion rounds, keyed by the cursor position the round was
 * issued for (wgpu_select_query_hint). The gizmo handler re-queries the PRESS
 * position when a drag starts — serving that position's completed round (not
 * merely the newest one) is what makes gizmo dragging activate. Small ring. */
struct OccRound {
  int x = INT32_MIN, y = INT32_MIN;
  std::vector<uint32_t> results;
};
static OccRound g_occ_rounds[8];
static int g_occ_round_next = 0;
static int g_occ_hint_x = INT32_MIN, g_occ_hint_y = INT32_MIN;

void wgpu_select_query_hint(int x, int y)
{
  g_occ_hint_x = x;
  g_occ_hint_y = y;
}

void WebGPUContext::occlusion_pool_begin(uint32_t capacity)
{
  if (device_ == nullptr) {
    return;
  }
  if (occ_qset_ == nullptr || occ_qset_capacity_ < capacity) {
    if (occ_qset_) {
      wgpuQuerySetRelease(occ_qset_);
    }
    occ_qset_capacity_ = std::max(capacity, 256u);
    WGPUQuerySetDescriptor qd = {};
    qd.type = WGPUQueryType_Occlusion;
    qd.count = occ_qset_capacity_;
    occ_qset_ = wgpuDeviceCreateQuerySet(device_, &qd);
  }
  /* The query set must be attached at pass BEGIN — close any open pass. */
  render_pass_end();
  occ_attach_ = (occ_qset_ != nullptr);
  occ_query_pending_ = -1;
  occ_query_open_in_pass_ = false;
}

void WebGPUContext::occlusion_query_end()
{
  if (occ_query_open_in_pass_ && render_pass_ != nullptr) {
    wgpuRenderPassEncoderEndOcclusionQuery(render_pass_);
  }
  occ_query_open_in_pass_ = false;
  occ_query_pending_ = -1;
}

void WebGPUContext::occlusion_pool_read(uint32_t count, uint32_t *r_values)
{
  /* Serve a completed round synchronously. Preference order:
   * 1. A round issued at (about) THIS cursor position that HAS hits.
   * 2. The closest round within a small radius that has hits — the CLICK_DRAG
   *    re-check can land several px from the press point (drag threshold), and
   *    a thin gizmo legitimately misses there even though the user pressed ON
   *    it a moment ago.
   * 3. The exact-position round (a genuine miss), else the newest round. */
  const OccRound *serve = nullptr;
  const OccRound *near_hit = nullptr;
  int near_hit_d2 = INT32_MAX;
  for (int k = 0; k < 8; k++) {
    /* Scan newest → oldest. */
    const OccRound &r = g_occ_rounds[(g_occ_round_next - 1 - k + 16) % 8];
    if (r.results.empty()) {
      continue;
    }
    if (serve == nullptr) {
      serve = &r; /* Newest completed = last-resort fallback. */
    }
    const int dx = r.x - g_occ_hint_x, dy = r.y - g_occ_hint_y;
    const int d2 = dx * dx + dy * dy;
    bool has_hit = false;
    for (const uint32_t v : r.results) {
      if (v != 0u && v != 0xFFFFFFFFu) {
        has_hit = true;
        break;
      }
    }
    if (d2 <= 2 * 2 && has_hit) {
      serve = &r;
      near_hit = nullptr; /* Exact hit round wins outright. */
      break;
    }
    if (d2 <= 16 * 16 && has_hit && d2 < near_hit_d2) {
      near_hit = &r;
      near_hit_d2 = d2;
    }
    if (d2 <= 2 * 2 && serve == &r) {
      /* Exact miss round already the newest; keep scanning for a near hit. */
    }
  }
  if (near_hit != nullptr) {
    serve = near_hit;
  }
  for (uint32_t i = 0; i < count; i++) {
    r_values[i] = (serve && i < serve->results.size()) ? serve->results[i] : 0u;
  }
  occ_attach_ = false;
  if (device_ == nullptr || occ_qset_ == nullptr || count == 0) {
    return;
  }
  count = std::min(count, occ_qset_capacity_);
  /* Resolve this round into a mappable buffer and collect it asynchronously
   * (results are 64-bit sample counts). */
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr) {
    return;
  }
  const uint64_t size = uint64_t(count) * sizeof(uint64_t);
  WGPUBufferDescriptor rd = {};
  rd.label = {"occ_resolve", WGPU_STRLEN};
  rd.size = size;
  rd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
  WGPUBuffer resolve_buf = wgpuDeviceCreateBuffer(device_, &rd);
  WGPUBufferDescriptor md = {};
  md.label = {"occ_map", WGPU_STRLEN};
  md.size = size;
  md.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer map_buf = wgpuDeviceCreateBuffer(device_, &md);
  if (resolve_buf == nullptr || map_buf == nullptr) {
    if (resolve_buf) {
      wgpuBufferRelease(resolve_buf);
    }
    if (map_buf) {
      wgpuBufferRelease(map_buf);
    }
    return;
  }
  wgpuCommandEncoderResolveQuerySet(enc, occ_qset_, 0, count, resolve_buf, 0);
  wgpuCommandEncoderCopyBufferToBuffer(enc, resolve_buf, 0, map_buf, 0, size);
  flush_encoder();
  wgpuBufferRelease(resolve_buf);

  struct OccMapCtx {
    WGPUBuffer buf;
    uint32_t count;
    int x, y;
  };
  OccMapCtx *mc = new OccMapCtx{map_buf, count, g_occ_hint_x, g_occ_hint_y};
  WGPUBufferMapCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowSpontaneous;
  cb.userdata1 = mc;
  cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *ud1, void *) {
    OccMapCtx *mc = static_cast<OccMapCtx *>(ud1);
    if (status == WGPUMapAsyncStatus_Success) {
      const uint64_t *vals = static_cast<const uint64_t *>(
          wgpuBufferGetConstMappedRange(mc->buf, 0, uint64_t(mc->count) * sizeof(uint64_t)));
      if (vals) {
        OccRound &round = g_occ_rounds[g_occ_round_next % 8];
        g_occ_round_next++;
        round.x = mc->x;
        round.y = mc->y;
        round.results.assign(mc->count, 0u);
        for (uint32_t i = 0; i < mc->count; i++) {
          round.results[i] = uint32_t(std::min<uint64_t>(vals[i], 0xFFFFFFFFu));
        }
      }
      wgpuBufferUnmap(mc->buf);
    }
    wgpuBufferRelease(mc->buf);
    delete mc;
  };
  wgpuBufferMapAsync(map_buf, WGPUMapMode_Read, 0, size, cb);
}

/* Completed film readback for the GUI F12 path: the render job reads the film
 * texture synchronously (gets zeros — no JSPI), we self-map a capture copy
 * asynchronously here and Blender's main loop injects the pixels into the
 * RenderResult once they arrive (wm_main_step → wgpu_film_result_take). */
static struct {
  std::vector<float> pixels; /* RGBA float, GL bottom-up rows. */
  int w = 0, h = 0;
  bool fresh = false;
} g_film_result;

bool wgpu_film_result_take(std::vector<float> &r_pixels, int &r_w, int &r_h)
{
  if (!g_film_result.fresh) {
    return false;
  }
  r_pixels = std::move(g_film_result.pixels);
  r_w = g_film_result.w;
  r_h = g_film_result.h;
  g_film_result.fresh = false;
  g_film_result.pixels.clear();
  return true;
}

void WebGPUContext::film_capture_async(WGPUTexture tex, WGPUTextureFormat fmt, int w, int h)
{
  if (device_ == nullptr || tex == nullptr ||
      !(fmt == WGPUTextureFormat_RGBA16Float || fmt == WGPUTextureFormat_RGBA32Float))
  {
    return;
  }
  const uint32_t bpp = (fmt == WGPUTextureFormat_RGBA16Float) ? 8 : 16;
  const uint32_t bpr = align256(uint32_t(w) * bpp);
  const uint64_t size = uint64_t(bpr) * uint64_t(h);
  WGPUBufferDescriptor md = {};
  md.label = {"film_map", WGPU_STRLEN};
  md.size = size;
  md.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer map_buf = wgpuDeviceCreateBuffer(device_, &md);
  if (map_buf == nullptr) {
    return;
  }
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr) {
    wgpuBufferRelease(map_buf);
    return;
  }
  WGPUTexelCopyTextureInfo src = {};
  src.texture = tex;
  src.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dst = {};
  dst.buffer = map_buf;
  dst.layout.bytesPerRow = bpr;
  dst.layout.rowsPerImage = uint32_t(h);
  WGPUExtent3D ext = {uint32_t(w), uint32_t(h), 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
  flush_encoder();

  struct FilmMapCtx {
    WGPUBuffer buf;
    uint32_t w, h, bpr, bpp;
  };
  FilmMapCtx *mc = new FilmMapCtx{map_buf, uint32_t(w), uint32_t(h), bpr, bpp};
  WGPUBufferMapCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowSpontaneous;
  cb.userdata1 = mc;
  cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *ud1, void *) {
    FilmMapCtx *mc = static_cast<FilmMapCtx *>(ud1);
    if (status == WGPUMapAsyncStatus_Success) {
      const uint8_t *raw = static_cast<const uint8_t *>(
          wgpuBufferGetConstMappedRange(mc->buf, 0, uint64_t(mc->bpr) * uint64_t(mc->h)));
      if (raw) {
        g_film_result.pixels.resize(size_t(mc->w) * mc->h * 4);
        for (uint32_t y = 0; y < mc->h; y++) {
          const uint8_t *row = raw + size_t(y) * mc->bpr;
          float *out = g_film_result.pixels.data() + size_t(y) * mc->w * 4;
          if (mc->bpp == 8) {
            math::half_to_float_array(
                reinterpret_cast<const uint16_t *>(row), out, size_t(mc->w) * 4);
          }
          else {
            std::memcpy(out, row, size_t(mc->w) * 4 * sizeof(float));
          }
        }
        g_film_result.w = int(mc->w);
        g_film_result.h = int(mc->h);
        g_film_result.fresh = true;
        fprintf(stderr, "WGPU_FILM result ready %ux%u\n", mc->w, mc->h);
        fflush(stderr);
      }
      wgpuBufferUnmap(mc->buf);
    }
    wgpuBufferRelease(mc->buf);
    delete mc;
  };
  wgpuBufferMapAsync(map_buf, WGPUMapMode_Read, 0, size, cb);
}

bool WebGPUContext::uniform_arena_alloc(uint64_t size, WGPUBuffer &r_buf, uint64_t &r_off)
{
  if (device_ == nullptr || size == 0) {
    return false;
  }
  /* minUniformBufferOffsetAlignment is 256. */
  const uint64_t aligned = (size + 255u) & ~uint64_t(255u);
  if (uni_arena_buf_ == nullptr || uni_arena_off_ + aligned > uni_arena_cap_) {
    if (uni_arena_buf_) {
      wgpuBufferRelease(uni_arena_buf_);
    }
    uni_arena_cap_ = std::max<uint64_t>(aligned, 256 * 1024);
    WGPUBufferDescriptor bd = {};
    bd.label = {"uniform_arena", WGPU_STRLEN};
    bd.size = uni_arena_cap_;
    bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uni_arena_buf_ = wgpuDeviceCreateBuffer(device_, &bd);
    uni_arena_off_ = 0;
    if (uni_arena_buf_ == nullptr) {
      return false;
    }
  }
  r_buf = uni_arena_buf_;
  r_off = uni_arena_off_;
  uni_arena_off_ += aligned;
  return true;
}

WGPUBuffer WebGPUContext::null_attr_buffer()
{
  if (null_attr_buffer_ == nullptr && device_ != nullptr) {
    WGPUBufferDescriptor bd = {};
    bd.label = {"null_attr", WGPU_STRLEN};
    bd.size = 64; /* >= largest vertex format (vec4<f32> = 16 B) at offset 0. */
    bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    bd.mappedAtCreation = true;
    null_attr_buffer_ = wgpuDeviceCreateBuffer(device_, &bd);
    if (null_attr_buffer_) {
      /* GL's default for an unfed generic vertex attribute is (0, 0, 0, 1),
       * and shaders DEPEND on w == 1 to detect a missing attribute (e.g.
       * attr_load_orco: all-zero padding made every procedural texture read
       * a constant generated coordinate -> flat). Replicate GL. */
      float *w = static_cast<float *>(wgpuBufferGetMappedRange(null_attr_buffer_, 0, 64));
      if (w != nullptr) {
        for (int i = 0; i < 16; i++) {
          w[i] = (i % 4 == 3) ? 1.0f : 0.0f;
        }
      }
      wgpuBufferUnmap(null_attr_buffer_);
    }
  }
  return null_attr_buffer_;
}

WGPUBuffer WebGPUContext::snapshot_buffer(WGPUBuffer src, uint64_t size)
{
  /* Copy for the READ-ONLY side of a writable buffer alias (WebGPU forbids the
   * same buffer under read and write bindings in one pass; GL/EEVEE use it for
   * in-place updates where reads must see the pre-pass contents anyway). */
  if (src == nullptr || device_ == nullptr || size == 0) {
    return nullptr;
  }
  const uint64_t key = size;
  WGPUBuffer scratch = nullptr;
  auto it = snapshot_buffers_.find(key);
  if (it != snapshot_buffers_.end()) {
    scratch = it->second;
  }
  else {
    WGPUBufferDescriptor bd = {};
    bd.size = size;
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    scratch = wgpuDeviceCreateBuffer(device_, &bd);
    snapshot_buffers_[key] = scratch;
  }
  if (scratch == nullptr) {
    return nullptr;
  }
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr) {
    return nullptr;
  }
  wgpuCommandEncoderCopyBufferToBuffer(enc, src, 0, scratch, 0, size);
  flush_encoder();
  return scratch;
}

WGPUTextureView WebGPUContext::snapshot_for_sampling(WebGPUTexture *t)
{
  /* WebGPU forbids sampling a texture that another binding of the SAME pass
   * writes as a storage image — even for disjoint texels (EEVEE's probe remap
   * does exactly that on the atlas). Emulate GL by sampling a SNAPSHOT: copy the
   * base texture into a cached scratch texture (recorded before the pass) and
   * bind the equivalent view of the scratch. */
  WGPUTexture base = t->wgpu_texture();
  if (base == nullptr || device_ == nullptr) {
    return nullptr;
  }
  const uint32_t w = wgpuTextureGetWidth(base);
  const uint32_t h = wgpuTextureGetHeight(base);
  const uint32_t layers = wgpuTextureGetDepthOrArrayLayers(base);
  const uint32_t mips = wgpuTextureGetMipLevelCount(base);
  const WGPUTextureFormat fmt = wgpuTextureGetFormat(base);

  const uint64_t key = (uint64_t(fmt) << 48) ^ (uint64_t(w) << 28) ^ (uint64_t(h) << 8) ^
                       (uint64_t(layers) << 4) ^ uint64_t(mips);
  WGPUTexture scratch = nullptr;
  auto it = snapshot_textures_.find(key);
  if (it != snapshot_textures_.end()) {
    scratch = it->second;
  }
  else {
    WGPUTextureDescriptor td = {};
    td.dimension = wgpuTextureGetDimension(base);
    td.size = {w, h, layers};
    td.format = fmt;
    td.mipLevelCount = mips;
    td.sampleCount = 1;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    scratch = wgpuDeviceCreateTexture(device_, &td);
    snapshot_textures_[key] = scratch;
  }
  if (scratch == nullptr) {
    return nullptr;
  }

  /* Record the copy outside any pass (bind groups are built pre-pass). */
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr) {
    return nullptr;
  }
  for (uint32_t m = 0; m < mips; m++) {
    WGPUTexelCopyTextureInfo src = {};
    src.texture = base;
    src.mipLevel = m;
    src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyTextureInfo dst = src;
    dst.texture = scratch;
    WGPUExtent3D ext = {std::max(w >> m, 1u), std::max(h >> m, 1u), layers};
    wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &ext);
  }
  flush_encoder();

  /* View of the scratch mirroring the original texture object's view window. */
  WGPUTextureView view = t->make_view_of(scratch);
  return view;
}

WGPUSampler WebGPUContext::nearest_sampler()
{
  if (nearest_sampler_ == nullptr && device_ != nullptr) {
    WGPUSamplerDescriptor d = {};
    d.addressModeU = WGPUAddressMode_ClampToEdge;
    d.addressModeV = WGPUAddressMode_ClampToEdge;
    d.addressModeW = WGPUAddressMode_ClampToEdge;
    d.magFilter = WGPUFilterMode_Nearest;
    d.minFilter = WGPUFilterMode_Nearest;
    d.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    d.maxAnisotropy = 1;
    d.lodMaxClamp = 32.0f;
    nearest_sampler_ = wgpuDeviceCreateSampler(device_, &d);
  }
  return nearest_sampler_;
}

void WebGPUContext::clear_snapshot_cache()
{
  for (auto &item : snapshot_textures_) {
    if (item.second) {
      wgpuTextureRelease(item.second);
    }
  }
  snapshot_textures_.clear();
  for (auto &item : snapshot_buffers_) {
    if (item.second) {
      wgpuBufferRelease(item.second);
    }
  }
  snapshot_buffers_.clear();
}

void WebGPUContext::backbuffer_ensure(int w, int h)
{
  if (device_ == nullptr || w <= 0 || h <= 0) {
    return;
  }
  if (backbuffer_color_ != nullptr && backbuffer_w_ == w && backbuffer_h_ == h) {
    return;
  }
  render_pass_end();
  if (backbuffer_color_) {
    /* Bind groups retain texture views; alias snapshots are cached by size.
     * Resizing must release these references to old drawing buffers. Release
     * handles, rather than destroying GPU resources used by queued commands. */
    for (WebGPUContext *ctx : g_all_contexts) {
      ctx->clear_bind_group_cache();
      ctx->clear_snapshot_cache();
    }
  }
  if (backbuffer_color_) {
    GPU_texture_free(backbuffer_color_);
    backbuffer_color_ = nullptr;
  }
  if (backbuffer_depth_) {
    GPU_texture_free(backbuffer_depth_);
    backbuffer_depth_ = nullptr;
  }
  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_SHADER_READ;
  backbuffer_color_ = GPU_texture_create_2d(
      "window_backbuffer_color", w, h, 1, TextureFormat::UNORM_8_8_8_8, usage, nullptr);
  backbuffer_depth_ = GPU_texture_create_2d(
      "window_backbuffer_depth", w, h, 1, TextureFormat::SFLOAT_32_DEPTH_UINT_8, usage, nullptr);
  backbuffer_w_ = w;
  backbuffer_h_ = h;

  GPUAttachment color_att = {backbuffer_color_, -1, 0};
  GPUAttachment depth_att = {backbuffer_depth_, -1, 0};
  back_left->attachment_set(GPU_FB_COLOR_ATTACHMENT0, color_att);
  back_left->attachment_set(GPU_FB_DEPTH_STENCIL_ATTACHMENT, depth_att);
  back_left->size_set(w, h);
  fprintf(stderr, "WGPU_PRESENT backbuffer %dx%d\n", w, h);
  fflush(stderr);
}

#ifdef __EMSCRIPTEN__
/* First real frame: the demo splash/console stays up until Blender presents a
 * frame that actually drew something. The initial present(s) are the startup
 * clear — zero draw work (no bind groups); the first UI/viewport frame creates
 * bind groups. We emit WGPU_FIRST_CONTENT on that frame and the page
 * (demo/src/main.js) swaps the splash for the live canvas then. */
static bool g_wgpu_content_signaled = false;
#endif

void WebGPUContext::present_backbuffer(int w, int h)
{
  if (device_ == nullptr) {
    return;
  }
  backbuffer_ensure(w, h);
  if (backbuffer_color_ == nullptr) {
    return;
  }
  if (surface_ == nullptr) {
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas_src.selector = {"#canvas", WGPU_STRLEN};
    WGPUSurfaceDescriptor sd = {};
    sd.nextInChain = &canvas_src.chain;
    surface_ = wgpuInstanceCreateSurface(webgpu_instance(), &sd);
    if (surface_ == nullptr) {
      static int s_once = 0;
      if (s_once++ == 0) {
        fprintf(stderr, "WGPU_PRESENT surface creation FAILED\n");
        fflush(stderr);
      }
      return;
    }
  }
  if (surface_w_ != w || surface_h_ != h) {
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    cfg.device = device_;
    /* Matches the backbuffer texture so a plain texture copy presents it. */
    cfg.format = WGPUTextureFormat_RGBA8Unorm;
    cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
    cfg.width = uint32_t(w);
    cfg.height = uint32_t(h);
    cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
    cfg.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface_, &cfg);
    surface_w_ = w;
    surface_h_ = h;
  }

  WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(surface_, &st);
  if (st.texture == nullptr) {
    return;
  }
  /* The backbuffer uses GL's bottom-up row convention (the vertex wrapper
   * negates y); the canvas wants top-down. Blit with a v-flip. */
  static WGPUShaderModule flip_module = nullptr;
  if (flip_module == nullptr) {
    static const char *WGSL =
        "@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n"
        "  var p = array<vec2f, 3>(vec2f(-1.0, -3.0), vec2f(-1.0, 1.0), vec2f(3.0, 1.0));\n"
        "  return vec4f(p[i], 0.0, 1.0);\n"
        "}\n"
        "@group(0) @binding(0) var src : texture_2d<f32>;\n"
        "@group(0) @binding(1) var smp : sampler;\n"
        "@fragment fn fs(@builtin(position) pos : vec4f) -> @location(0) vec4f {\n"
        "  let d = vec2f(textureDimensions(src));\n"
        "  let uv = vec2f(pos.x / d.x, 1.0 - pos.y / d.y);\n"
        "  return vec4f(textureSample(src, smp, uv).rgb, 1.0);\n"
        "}\n";
    WGPUShaderSourceWGSL src_wgsl = {};
    src_wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    src_wgsl.code = {WGSL, WGPU_STRLEN};
    WGPUShaderModuleDescriptor md = {};
    md.nextInChain = &src_wgsl.chain;
    md.label = {"present_flip_blit", WGPU_STRLEN};
    flip_module = wgpuDeviceCreateShaderModule(device_, &md);
  }
  static WGPURenderPipeline flip_pipe = nullptr;
  if (flip_pipe == nullptr && flip_module != nullptr) {
    WGPUColorTargetState target = {};
    target.format = WGPUTextureFormat_RGBA8Unorm;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState frag = {};
    frag.module = flip_module;
    frag.entryPoint = {"fs", WGPU_STRLEN};
    frag.targetCount = 1;
    frag.targets = &target;
    WGPURenderPipelineDescriptor rpd = {};
    rpd.label = {"present_flip_blit", WGPU_STRLEN};
    rpd.vertex.module = flip_module;
    rpd.vertex.entryPoint = {"vs", WGPU_STRLEN};
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpd.multisample.count = 1;
    rpd.multisample.mask = 0xFFFFFFFFu;
    rpd.fragment = &frag;
    flip_pipe = wgpuDeviceCreateRenderPipeline(device_, &rpd);
  }
  render_pass_end();
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc && flip_pipe) {
    WGPUTextureView dst_view = wgpuTextureCreateView(st.texture, nullptr);
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(flip_pipe, 0);
    WGPUBindGroupEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].textureView = static_cast<WebGPUTexture *>(backbuffer_color_)->wgpu_sample_view();
    entries[1].binding = 1;
    entries[1].sampler = nearest_sampler();
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = bgl;
    bgd.entryCount = 2;
    bgd.entries = entries;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device_, &bgd);
    WGPURenderPassColorAttachment ca = {};
    ca.view = dst_view;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    WGPURenderPassDescriptor rp = {};
    rp.label = {"present_flip_blit", WGPU_STRLEN};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    if (pass) {
      wgpuRenderPassEncoderSetPipeline(pass, flip_pipe);
      wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
      wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
    }
    flush_encoder();
    if (bg) {
      wgpuBindGroupRelease(bg);
    }
    wgpuBindGroupLayoutRelease(bgl);
    wgpuTextureViewRelease(dst_view);
  }
  /* No wgpuSurfacePresent on emdawnwebgpu: the browser presents the canvas
   * automatically when this task yields (requestAnimationFrame-driven loop). */
  wgpuTextureRelease(st.texture);

  /* Perf stats: per-frame submit/pass/bindgroup counts (high submit counts are
   * the #1 slowness cause — each is an emscripten→Dawn→browser round trip). */
  {
    extern int g_stat_submits, g_stat_passes, g_stat_bindgroups, g_stat_flushes, g_stat_fbswitch,
        g_stat_bg_hits;
    static int s_frame = 0;
    static double s_last_ms = 0.0;
    const double now_ms = emscripten_get_now();
    const double frame_ms = s_last_ms > 0.0 ? now_ms - s_last_ms : 0.0;
    s_last_ms = now_ms;
    s_frame++;
    if (getenv("WGPU_PROFILE") && (s_frame <= 8 || (s_frame % 60) == 0 || frame_ms > 25.0)) {
      fprintf(stderr,
              "WGPU_STATS frame=%d dt=%.1fms submits=%d passes=%d bindgroups=%d bg_hits=%d "
              "guard_flushes=%d fb_switches=%d tex=%d/%lldMB\n",
              s_frame,
              frame_ms,
              g_stat_submits,
              g_stat_passes,
              g_stat_bindgroups,
              g_stat_bg_hits,
              g_stat_flushes,
              g_stat_fbswitch,
              [] { extern int g_stat_tex_count; return g_stat_tex_count; }(),
              [] { extern long long g_stat_tex_bytes; return g_stat_tex_bytes / (1024 * 1024); }());
      extern void webgpu_stat_flush_dump();
      webgpu_stat_flush_dump();
    }
#ifdef __EMSCRIPTEN__
    /* First frame with actual draw work (bind groups created) = first non-black
     * frame; the startup clear has none. Signal the demo splash to reveal. */
    if (!g_wgpu_content_signaled && g_stat_bindgroups > 0) {
      g_wgpu_content_signaled = true;
      GHOST_web_first_frame();
      fprintf(stderr, "WGPU_FIRST_CONTENT\n");
      fflush(stderr);
    }
#endif
    g_stat_submits = g_stat_passes = g_stat_bindgroups = g_stat_flushes = g_stat_fbswitch =
        g_stat_bg_hits = 0;
  }
  uniform_arena_reset();
}

/* GHOST_WindowWeb hooks (intern/ghost) — resolved at the final static link. */
extern "C" void blender_webgpu_present(int width, int height)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx) {
    ctx->present_backbuffer(width, height);
  }
}
extern "C" void blender_webgpu_backbuffer_size(int width, int height)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx) {
    ctx->backbuffer_ensure(width, height);
  }
}

void WebGPUContext::generate_mipmaps(WebGPUTexture *t)
{
  if (device_ == nullptr || t == nullptr || t->wgpu_texture() == nullptr) {
    return;
  }
  const WGPUTextureFormat fmt = t->wgpu_format();
  if (t->is_depth_format()) {
    return;
  }
  switch (fmt) {
    /* textureSample needs a float-sampleable, renderable format. */
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
      break;
    default: {
      static int s_mip_log = 0;
      if (s_mip_log < 10) {
        s_mip_log++;
        fprintf(stderr, "WGPU_MIPGEN skipped: format %d not blit-able\n", int(fmt));
        fflush(stderr);
      }
      return;
    }
  }
  const int mips = t->mip_count();
  if (mips <= 1) {
    return;
  }
  /* A mip view without RenderAttachment usage makes the render pass — and with
   * it the entire frame's command buffer — invalid at submit. Skip instead. */
  if (!(wgpuTextureGetUsage(t->wgpu_texture()) & WGPUTextureUsage_RenderAttachment)) {
    static int s_mip_att_log = 0;
    if (s_mip_att_log < 5) {
      s_mip_att_log++;
      fprintf(stderr, "WGPU_MIPGEN skipped: texture lacks RenderAttachment usage\n");
      fflush(stderr);
    }
    return;
  }

  static WGPUShaderModule blit_module = nullptr;
  if (blit_module == nullptr) {
    static const char *WGSL =
        "@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n"
        "  var p = array<vec2f, 3>(vec2f(-1.0, -3.0), vec2f(-1.0, 1.0), vec2f(3.0, 1.0));\n"
        "  return vec4f(p[i], 0.0, 1.0);\n"
        "}\n"
        "@group(0) @binding(0) var src : texture_2d<f32>;\n"
        "@group(0) @binding(1) var smp : sampler;\n"
        "@fragment fn fs(@builtin(position) pos : vec4f) -> @location(0) vec4f {\n"
        "  let sd = vec2f(textureDimensions(src));\n"
        "  let dd = max(floor(sd * 0.5), vec2f(1.0));\n"
        "  return textureSample(src, smp, pos.xy / dd);\n"
        "}\n";
    WGPUShaderSourceWGSL src = {};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = {WGSL, WGPU_STRLEN};
    WGPUShaderModuleDescriptor md = {};
    md.nextInChain = &src.chain;
    md.label = {"mipgen_blit", WGPU_STRLEN};
    blit_module = wgpuDeviceCreateShaderModule(device_, &md);
  }
  static std::map<WGPUTextureFormat, WGPURenderPipeline> pipelines;
  WGPURenderPipeline pipe = pipelines.count(fmt) ? pipelines[fmt] : nullptr;
  if (pipe == nullptr) {
    WGPUColorTargetState target = {};
    target.format = fmt;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState frag = {};
    frag.module = blit_module;
    frag.entryPoint = {"fs", WGPU_STRLEN};
    frag.targetCount = 1;
    frag.targets = &target;
    WGPURenderPipelineDescriptor rpd = {};
    rpd.label = {"mipgen_blit", WGPU_STRLEN};
    rpd.vertex.module = blit_module;
    rpd.vertex.entryPoint = {"vs", WGPU_STRLEN};
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpd.multisample.count = 1;
    rpd.multisample.mask = 0xFFFFFFFFu;
    rpd.fragment = &frag;
    pipe = wgpuDeviceCreateRenderPipeline(device_, &rpd);
    pipelines[fmt] = pipe;
  }
  if (pipe == nullptr) {
    return;
  }

  /* Blit passes cannot nest inside whatever pass is open. */
  render_pass_end();
  WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(pipe, 0);
  const int layers = std::max(t->layer_count(), 1);
  for (int layer = 0; layer < layers; layer++) {
    for (int mip = 1; mip < mips; mip++) {
      WGPUCommandEncoder enc = ensure_encoder();
      if (enc == nullptr) {
        break;
      }
      /* Single-mip single-layer source view. */
      WGPUTextureViewDescriptor sv = {};
      sv.format = fmt;
      sv.dimension = WGPUTextureViewDimension_2D;
      sv.baseMipLevel = uint32_t(mip - 1);
      sv.mipLevelCount = 1;
      sv.baseArrayLayer = uint32_t(layer);
      sv.arrayLayerCount = 1;
      sv.aspect = WGPUTextureAspect_All;
      WGPUTextureView src_view = wgpuTextureCreateView(t->wgpu_texture(), &sv);
      WGPUTextureView dst_view = t->wgpu_attachment_view(layer, mip);
      if (src_view == nullptr || dst_view == nullptr) {
        if (src_view) {
          wgpuTextureViewRelease(src_view);
        }
        break;
      }
      WGPUBindGroupEntry entries[2] = {};
      entries[0].binding = 0;
      entries[0].textureView = src_view;
      entries[1].binding = 1;
      entries[1].sampler = default_sampler();
      WGPUBindGroupDescriptor bgd = {};
      bgd.layout = bgl;
      bgd.entryCount = 2;
      bgd.entries = entries;
      WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device_, &bgd);

      WGPURenderPassColorAttachment ca = {};
      ca.view = dst_view;
      ca.loadOp = WGPULoadOp_Clear;
      ca.storeOp = WGPUStoreOp_Store;
      ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
      WGPURenderPassDescriptor rp = {};
      rp.label = {"mipgen_blit", WGPU_STRLEN};
      rp.colorAttachmentCount = 1;
      rp.colorAttachments = &ca;
      WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
      if (pass) {
        wgpuRenderPassEncoderSetPipeline(pass, pipe);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
      }
      flush_encoder();
      if (bg) {
        wgpuBindGroupRelease(bg);
      }
      wgpuTextureViewRelease(src_view);
    }
  }
  wgpuBindGroupLayoutRelease(bgl);
}

/* Resolve the texture currently bound for the TEXTURE resource at `binding`
 * within `bindings` (used to pair samplers with their texture). */
WebGPUTexture *WebGPUContext::bound_texture_for_binding(
    ShaderInterface *iface, const std::vector<WgslResourceBinding> &bindings, uint32_t binding)
{
  if (iface == nullptr) {
    return nullptr;
  }
  for (const WgslResourceBinding &b : bindings) {
    if (b.binding != binding || b.kind != WgslResourceBinding::TEXTURE) {
      continue;
    }
    const ShaderInput *in = iface->uniform_get(StringRefNull(b.res_name.c_str()));
    int slot = in ? in->binding : -1;
    return (slot >= 0 && slot < WEBGPU_MAX_TEX) ? bound_tex_[slot] : nullptr;
  }
  return nullptr;
}

WGPUBindGroupLayout WebGPUContext::make_bind_group_layout(
    const std::vector<WgslResourceBinding> &bindings, bool is_compute, ShaderInterface *iface)
{
  if (device_ == nullptr) {
    return nullptr;
  }
  const WGPUShaderStage base_vis = is_compute ?
                                       WGPUShaderStage_Compute :
                                       WGPUShaderStage(WGPUShaderStage_Vertex |
                                                       WGPUShaderStage_Fragment);
  std::vector<WGPUBindGroupLayoutEntry> entries;
  for (const WgslResourceBinding &b : bindings) {
    WGPUBindGroupLayoutEntry e = {};
    e.binding = b.binding;
    /* Writable storage is illegal in the vertex stage. */
    e.visibility = (b.writable && !is_compute) ? WGPUShaderStage_Fragment : (b.visibility ? b.visibility : base_vis);
    switch (b.kind) {
      case WgslResourceBinding::UBO:
      case WgslResourceBinding::PUSH_CONST:
        /* NOTE: push constants deliberately do NOT use hasDynamicOffset: reusing
         * one cached bind group with per-draw dynamic offsets left every draw in
         * a pass reading the FIRST offset (all icons identical, flicker on arena
         * rollover). The slice offset is baked into the bind group instead and
         * the cache key includes it — unchanged uniforms still hit the cache. */
        e.buffer.type = WGPUBufferBindingType_Uniform;
        break;
      case WgslResourceBinding::SSBO:
        e.buffer.type = b.buffer_type;
        break;
      case WgslResourceBinding::TEXTURE: {
        e.texture.sampleType = b.tex_sample;
        e.texture.viewDimension = b.view_dim;
        e.texture.multisampled = false;
        /* Depth textures sample as unfilterable-float: their WGSL type is
         * texture_2d<f32> (Tint), but a Float (filterable) layout entry rejects
         * the depth-only view at bind-group creation. Resolve the currently
         * bound texture (pipelines are created at first draw, textures already
         * bound) and downgrade the sample type for depth formats. */
        if (iface != nullptr && e.texture.sampleType == WGPUTextureSampleType_Float) {
          const ShaderInput *in = iface->uniform_get(StringRefNull(b.res_name.c_str()));
          int slot = in ? in->binding : -1;
          WebGPUTexture *t = (slot >= 0 && slot < WEBGPU_MAX_TEX) ? bound_tex_[slot] : nullptr;
          if (t) {
            switch (t->wgpu_format()) {
              case WGPUTextureFormat_Depth16Unorm:
              case WGPUTextureFormat_Depth24Plus:
              case WGPUTextureFormat_Depth24PlusStencil8:
              case WGPUTextureFormat_Depth32Float:
              case WGPUTextureFormat_Depth32FloatStencil8:
                e.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
                break;
              default:
                break;
            }
          }
        }
        break;
      }
      case WgslResourceBinding::SAMPLER:
        e.sampler.type = b.sampler_type;
        /* Samplers paired with depth textures (bound as unfilterable-float)
         * must be non-filtering; the bind group binds nearest_sampler() then. */
        if (iface != nullptr && b.binding >= 128 &&
            e.sampler.type == WGPUSamplerBindingType_Filtering)
        {
          WebGPUTexture *paired = bound_texture_for_binding(iface, bindings, b.binding - 128);
          if (paired && paired->is_depth_format()) {
            e.sampler.type = WGPUSamplerBindingType_NonFiltering;
          }
        }
        break;
      case WgslResourceBinding::STORAGE_TEXTURE:
        e.storageTexture.access = b.storage_access;
        e.storageTexture.format = b.storage_format;
        e.storageTexture.viewDimension = b.view_dim;
        break;
    }
    entries.push_back(e);
  }
  WGPUBindGroupLayoutDescriptor desc = {};
  desc.entryCount = entries.size();
  desc.entries = entries.empty() ? nullptr : entries.data();
  return wgpuDeviceCreateBindGroupLayout(device_, &desc);
}

WGPUPipelineLayout WebGPUContext::make_pipeline_layout(WGPUBindGroupLayout bgl)
{
  if (device_ == nullptr) {
    return nullptr;
  }
  WGPUPipelineLayoutDescriptor desc = {};
  desc.bindGroupLayoutCount = bgl ? 1 : 0;
  desc.bindGroupLayouts = bgl ? &bgl : nullptr;
  return wgpuDeviceCreatePipelineLayout(device_, &desc);
}

WebGPUTexture *WebGPUContext::bound_storage_image(WebGPUShader *shader, const char *name)
{
  if (shader == nullptr || shader->interface == nullptr || name == nullptr) {
    return nullptr;
  }
  const ShaderInput *in = shader->interface->uniform_get(StringRefNull(name));
  int slot = in ? in->binding : -1;
  return (slot >= 0 && slot < WEBGPU_MAX_IMAGE) ? bound_image_[slot] : nullptr;
}

WGPUBindGroup WebGPUContext::build_bind_group(WebGPUShader *shader,
                                              const std::vector<WgslResourceBinding> &bindings,
                                              WGPUBindGroupLayout bgl)
{
  if (device_ == nullptr || shader == nullptr || shader->interface == nullptr) {
    return nullptr;
  }
  ShaderInterface *iface = shader->interface;
  std::vector<WGPUBindGroupEntry> entries;
  bool incomplete = false;
  extern int g_stat_bindgroups;
  g_stat_bindgroups++;

  const bool dbg = false;
  static int s_dbg_n = 0;
  const bool dbg_log = dbg && (s_dbg_n++ < 60);

  for (const WgslResourceBinding &b : bindings) {
    WGPUBindGroupEntry e = {};
    e.binding = b.binding;
    switch (b.kind) {
      case WgslResourceBinding::PUSH_CONST: {
        WGPUBuffer buf = nullptr;
        uint64_t off = 0, size = 0;
        if (shader->push_const_slice(buf, off, size)) {
          e.buffer = buf;
          e.offset = off;
          e.size = size;
        }
        break;
      }
      case WgslResourceBinding::UBO: {
        if (b.res_name == "web_viewports") {
          float transforms[16][4];
          static_cast<WebGPUFrameBuffer *>(active_fb)->viewport_transforms(transforms);
          if (uniform_arena_alloc(sizeof(transforms), e.buffer, e.offset)) {
            e.size = sizeof(transforms);
            wgpuQueueWriteBuffer(queue_, e.buffer, e.offset, transforms, sizeof(transforms));
          }
          break;
        }
        const ShaderInput *in = iface->ubo_get(StringRefNull(b.res_name.c_str()));
        int slot = in ? in->binding : -1;
        e.buffer = (slot >= 0 && slot < WEBGPU_MAX_UBO) ? bound_ubo_[slot] : nullptr;
        e.size = WGPU_WHOLE_SIZE;
        break;
      }
      case WgslResourceBinding::SSBO: {
        const ShaderInput *in = iface->ssbo_get(StringRefNull(b.res_name.c_str()));
        int slot = in ? in->binding : -1;
        e.buffer = (slot >= 0 && slot < WEBGPU_MAX_SSBO) ? bound_ssbo_[slot] : nullptr;
        const std::string buffer_texture_prefix = "web_buffer_texture_";
        if (b.res_name.rfind(buffer_texture_prefix, 0) == 0) {
          const std::string name = b.res_name.substr(buffer_texture_prefix.size());
          const ShaderInput *sampler = iface->uniform_get(StringRefNull(name.c_str()));
          const int unit = sampler ? sampler->binding : -1;
          e.buffer = unit >= 0 && unit < WEBGPU_MAX_TEX ? bound_tex_buffer_[unit] : nullptr;
        }
        else if (b.res_name.rfind("web_atomic_sample_", 0) == 0) {
          const std::string name = b.res_name.substr(18);
          const ShaderInput *sampler = iface->uniform_get(StringRefNull(name.c_str()));
          WebGPUTexture *texture = sampler && sampler->binding >= 0 && sampler->binding < WEBGPU_MAX_TEX ? bound_tex_[sampler->binding] : nullptr;
          e.buffer = texture ? texture->atomic_buffer() : nullptr;
        }
        else if (b.res_name.rfind("web_atomic_", 0) == 0) {
          const std::string image_name = b.res_name.substr(11);
          const ShaderInput *image = iface->uniform_get(StringRefNull(image_name.c_str()));
          WebGPUTexture *texture = image && image->binding >= 0 && image->binding < WEBGPU_MAX_IMAGE ? bound_image_[image->binding] : nullptr;
          e.buffer = texture ? texture->atomic_buffer() : nullptr;
        }
        e.size = WGPU_WHOLE_SIZE;
        break;
      }
      case WgslResourceBinding::TEXTURE: {
        const ShaderInput *in = iface->uniform_get(StringRefNull(b.res_name.c_str()));
        int slot = in ? in->binding : -1;
        WebGPUTexture *t = (slot >= 0 && slot < WEBGPU_MAX_TEX) ? bound_tex_[slot] : nullptr;
        /* Depth-stencil textures must be bound through a single-aspect view. */
        if (t) t->resolve_atomics();
        e.textureView = t ? t->wgpu_sample_view() : nullptr;
        break;
      }
      case WgslResourceBinding::STORAGE_TEXTURE: {
        const ShaderInput *in = iface->uniform_get(StringRefNull(b.res_name.c_str()));
        int slot = in ? in->binding : -1;
        WebGPUTexture *t = (slot >= 0 && slot < WEBGPU_MAX_IMAGE) ? bound_image_[slot] : nullptr;
        /* Storage bindings require a single-mip view. */
        e.textureView = t ? t->wgpu_storage_view() : nullptr;
        /* The builtin generates up to two mip levels per dispatch. For its
         * final single-level dispatch Blender binds the last mip to both output
         * slots. Output 2 is unused, but WebGPU still rejects the writable alias.
         * Give only that inactive binding distinct storage. */
        if (slot == 2 && t != nullptr && t == bound_image_[1] &&
            shader->name_get().startswith("gpu_shader_2D_update_mipmaps_"))
        {
          e.textureView = dummy_storage_view(b.storage_format, b.view_dim);
        }
        /* Blender sometimes binds a dummy of a DIFFERENT format than the shader
         * declares (GL reinterprets; WebGPU rejects and the whole draw would be
         * dropped). Substitute a throwaway dummy of the declared format. */
        if (t != nullptr && t->wgpu_format() != b.storage_format) {
          e.textureView = dummy_storage_view(b.storage_format, b.view_dim);
        }
        break;
      }
      case WgslResourceBinding::SAMPLER: {
        /* The sampler at binding N+128 pairs with the texture at binding N
         * (deterministic Tint sampler mapping, see glsl_to_wgsl). Depth textures
         * bind as unfilterable-float, which WebGPU forbids pairing with a
         * filtering sampler — use the nearest sampler for those. */
        e.sampler = default_sampler();
        if (b.binding >= 128) {
          /* Honor the GPUSamplerState captured at bind time for the paired
           * texture slot (Closest interpolation, no-mip clamping, extend
           * modes). One hardcoded linear+mip sampler made byte image textures
           * sample low mips during magnification (tex_srgb_file purple mush /
           * mr_elephant muddy-dark albedo). */
          for (const WgslResourceBinding &tb : bindings) {
            if (tb.binding != b.binding - 128 || tb.kind != WgslResourceBinding::TEXTURE) {
              continue;
            }
            const ShaderInput *in_tex = iface ? iface->uniform_get(
                                                    StringRefNull(tb.res_name.c_str())) :
                                                nullptr;
            if (in_tex != nullptr) {
              e.sampler = sampler_for_state(bound_tex_state_get(in_tex->binding));
            }
            break;
          }
          WebGPUTexture *paired = bound_texture_for_binding(iface, bindings, b.binding - 128);
          if (paired && paired->is_depth_format()) {
            e.sampler = nearest_sampler();
          }
        }
        break;
      }
    }
    /* A binding the layout requires but we cannot resolve makes the whole bind
     * group invalid; skip building it (that draw is then skipped). */
    const bool resolved = e.buffer || e.textureView || e.sampler;
    if (!resolved) {
      incomplete = true;
      static int s_unresolved_logged = 0;
      if (s_unresolved_logged < 40) {
        s_unresolved_logged++;
        fprintf(stderr,
                "WGPU_BG unresolved '%s' @%u kind=%d res='%s'\n",
                shader->name_get().c_str(),
                b.binding,
                int(b.kind),
                b.res_name.c_str());
        fflush(stderr);
      }
    }
    if (dbg_log) {
      /* For texture-ish bindings also identify the bound texture (dims/format)
       * so wrong-texture resolution is visible in the log. */
      WebGPUTexture *dbg_t = nullptr;
      if (b.kind == WgslResourceBinding::TEXTURE || b.kind == WgslResourceBinding::STORAGE_TEXTURE)
      {
        const ShaderInput *in = iface->uniform_get(StringRefNull(b.res_name.c_str()));
        int slot = in ? in->binding : -1;
        if (b.kind == WgslResourceBinding::TEXTURE) {
          dbg_t = (slot >= 0 && slot < WEBGPU_MAX_TEX) ? bound_tex_[slot] : nullptr;
        }
        else {
          dbg_t = (slot >= 0 && slot < WEBGPU_MAX_IMAGE) ? bound_image_[slot] : nullptr;
        }
      }
      int dbg_slot = -2;
      if (b.kind == WgslResourceBinding::SSBO) {
        const ShaderInput *in = iface->ssbo_get(StringRefNull(b.res_name.c_str()));
        dbg_slot = in ? in->binding : -1;
      }
      else if (b.kind == WgslResourceBinding::UBO) {
        const ShaderInput *in = iface->ubo_get(StringRefNull(b.res_name.c_str()));
        dbg_slot = in ? in->binding : -1;
      }
      fprintf(stderr,
              "WGPU_BG '%s' @%u kind=%d res='%s' slot=%d -> buf=%p view=%p samp=%p tex=%p %dx%dx%d fmt=%d\n",
              shader->name_get().c_str(),
              b.binding,
              int(b.kind),
              b.res_name.c_str(),
              dbg_slot,
              (void *)e.buffer,
              (void *)e.textureView,
              (void *)e.sampler,
              (void *)dbg_t,
              dbg_t ? dbg_t->width_get() : -1,
              dbg_t ? dbg_t->height_get() : -1,
              dbg_t ? dbg_t->depth_get() : -1,
              dbg_t ? int(dbg_t->wgpu_format()) : -1);
      if (e.buffer) {
        fprintf(stderr, "WGPU_BG    buf-size=%llu\n",
                (unsigned long long)wgpuBufferGetSize(e.buffer));
      }
      fflush(stderr);
    }
    entries.push_back(e);
  }

  if (incomplete) {
    return nullptr;
  }

  /* Sampled-texture vs storage-texture aliasing on the SAME texture within one
   * bind group is invalid in WebGPU (per-subresource usage scopes) — sample a
   * snapshot copy instead (GL semantics: reads see pre-pass contents). */
  for (size_t i = 0; i < bindings.size(); i++) {
    if (bindings[i].kind != WgslResourceBinding::TEXTURE || entries[i].textureView == nullptr) {
      continue;
    }
    WebGPUTexture *tex_i = bound_texture_for_binding(iface, bindings, bindings[i].binding);
    if (tex_i == nullptr) {
      continue;
    }
    for (size_t j = 0; j < bindings.size(); j++) {
      if (bindings[j].kind != WgslResourceBinding::STORAGE_TEXTURE ||
          entries[j].textureView == nullptr)
      {
        continue;
      }
      const ShaderInput *in_j = iface->uniform_get(StringRefNull(bindings[j].res_name.c_str()));
      int slot_j = in_j ? in_j->binding : -1;
      WebGPUTexture *tex_j = (slot_j >= 0 && slot_j < WEBGPU_MAX_IMAGE) ? bound_image_[slot_j] :
                                                                          nullptr;
      if (tex_j == nullptr || tex_j->wgpu_texture() != tex_i->wgpu_texture()) {
        continue;
      }
      WGPUTextureView snap = snapshot_for_sampling(tex_i);
      if (snap) {
        static int s_snap_logged = 0;
        if (s_snap_logged < 8) {
          s_snap_logged++;
          fprintf(stderr, "WGPU_BG snapshot '%s' sampled '%s' aliases storage '%s'\n",
                  shader->name_get().c_str(), bindings[i].res_name.c_str(),
                  bindings[j].res_name.c_str());
          fflush(stderr);
        }
        entries[i].textureView = snap;
      }
      break;
    }
  }

  /* Writable buffer aliasing (same WGPUBuffer at two bindings, either writable)
   * is a WebGPU validation error at dispatch — and one invalid command POISONS
   * the whole command buffer at submit, silently dropping every valid draw
   * recorded alongside it. Skip the offending dispatch/draw instead. */
  for (size_t i = 0; i < entries.size(); i++) {
    for (size_t j = i + 1; j < entries.size(); j++) {
      if (entries[i].buffer == nullptr || entries[i].buffer != entries[j].buffer) {
        continue;
      }
      const bool i_writable = bindings[i].kind == WgslResourceBinding::SSBO &&
                              bindings[i].buffer_type == WGPUBufferBindingType_Storage;
      const bool j_writable = bindings[j].kind == WgslResourceBinding::SSBO &&
                              bindings[j].buffer_type == WGPUBufferBindingType_Storage;
      if (i_writable != j_writable) {
        /* One side read-only: bind a snapshot copy there (GL in-place-update
         * semantics: reads see pre-pass contents). */
        const size_t ro = i_writable ? j : i;
        WGPUBuffer snap = snapshot_buffer(entries[ro].buffer,
                                          wgpuBufferGetSize(entries[ro].buffer));
        if (snap) {
          static int s_alias_logged = 0;
          if (s_alias_logged < 8) {
            s_alias_logged++;
            fprintf(stderr, "WGPU_BG ALIAS-SNAP '%s' readonly '%s' copies aliased buffer\n",
                    shader->name_get().c_str(), bindings[ro].res_name.c_str());
            fflush(stderr);
          }
          entries[ro].buffer = snap;
          continue;
        }
      }
      if (i_writable || j_writable) {
        static int s_alias_logged = 0;
        if (s_alias_logged < 20) {
          s_alias_logged++;
          fprintf(stderr,
                  "WGPU_BG ALIAS-SKIP '%s' @%u('%s') and @%u('%s') -> same buffer %p\n",
                  shader->name_get().c_str(),
                  bindings[i].binding,
                  bindings[i].res_name.c_str(),
                  bindings[j].binding,
                  bindings[j].res_name.c_str(),
                  (void *)entries[i].buffer);
          fflush(stderr);
        }
        return nullptr;
      }
    }
  }

  /* Record this draw's SSBO buffers + writability for the per-pass usage
   * conflict check (after alias/snapshot resolution — snapshots replace the
   * bound buffer). See pass_buffer_usage_. */
  pending_draw_buffers_.clear();
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].buffer != nullptr && bindings[i].kind == WgslResourceBinding::SSBO) {
      const bool writable = bindings[i].buffer_type == WGPUBufferBindingType_Storage;
      pending_draw_buffers_.emplace_back(entries[i].buffer, writable);
      if (getenv("WGPU_LOG_SSBOUSE") && strstr(shader->name_get().c_str(), "MAV") != nullptr) {
        fprintf(stderr,
                "WGPU_SSBOUSE '%s' buf=%p w=%d res='%s'\n",
                shader->name_get().c_str(),
                (void *)entries[i].buffer,
                int(writable),
                bindings[i].res_name.c_str());
        fflush(stderr);
      }
    }
  }

  /* Cache: CreateBindGroup crosses the JS boundary and the UI rebinds the same
   * few resource sets thousands of times. Key = shader + resolved entries. The
   * cached group's refs keep every contained handle alive, so a matching key
   * can never alias a freed-and-reallocated handle. Keyed on the SHADER (not
   * `bgl`): GetBindGroupLayout returns a fresh wrapper each call, but WebGPU
   * only requires the bind group's layout to be GROUP-EQUIVALENT to the
   * pipeline's, and all layouts for one shader come from the same bindings. */
  uint64_t key = 1469598103934665603ull;
  auto h64 = [&key](const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) {
      key = (key ^ b[i]) * 1099511628211ull;
    }
  };
  const void *sh_ptr = shader;
  h64(&sh_ptr, sizeof(sh_ptr));
  for (const WGPUBindGroupEntry &e : entries) {
    h64(&e.binding, sizeof(e.binding));
    h64(&e.buffer, sizeof(e.buffer));
    h64(&e.offset, sizeof(e.offset));
    h64(&e.size, sizeof(e.size));
    h64(&e.textureView, sizeof(e.textureView));
    h64(&e.sampler, sizeof(e.sampler));
  }
  static const bool cache_off = getenv("WGPU_BG_CACHE_OFF") != nullptr;
  auto it = cache_off ? bind_group_cache_.end() : bind_group_cache_.find(key);
  if (it != bind_group_cache_.end()) {
    extern int g_stat_bg_hits;
    g_stat_bg_hits++;
    wgpuBindGroupAddRef(it->second.bg); /* Caller owns one ref (it releases). */
    return it->second.bg;
  }

  WGPUBindGroupDescriptor desc = {};
  desc.layout = bgl;
  desc.entryCount = entries.size();
  desc.entries = entries.empty() ? nullptr : entries.data();
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device_, &desc);
  if (bg && !cache_off) {
    if (bind_group_cache_.size() >= 2048) {
      /* Crude bound: drop everything; hot entries repopulate next frame. */
      clear_bind_group_cache();
    }
    /* Pin every resource handle (see the header comment: a recycled handle ID
     * would make a DIFFERENT future resource hash onto this cached group). */
    for (const WGPUBindGroupEntry &e : entries) {
      if (e.buffer) {
        wgpuBufferAddRef(e.buffer);
      }
      if (e.textureView) {
        wgpuTextureViewAddRef(e.textureView);
      }
      if (e.sampler) {
        wgpuSamplerAddRef(e.sampler);
      }
    }
    wgpuBindGroupAddRef(bg);
    bind_group_cache_[key] = {bg, entries};
  }
  return bg;
}

/* Decode one texel of `fmt` into out[4] (rgb default 0, a default 1). Covers the
 * formats Blender actually reads back (8-bit unorm, 16/32-bit float). */
static void webgpu_decode_texel(const uint8_t *p, WGPUTextureFormat fmt, float out[4])
{
  auto half = [](uint16_t h) -> float {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, f = h & 0x3ffu;
    float v;
    if (e == 0) { v = std::ldexp(float(f), -24); }
    else if (e == 31) { v = f ? NAN : INFINITY; }
    else { v = std::ldexp(float(f | 0x400u), int(e) - 25); }
    return s ? -v : v;
  };
  out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
  const float *f32 = reinterpret_cast<const float *>(p);
  const uint16_t *u16 = reinterpret_cast<const uint16_t *>(p);
  switch (fmt) {
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8UnormSrgb:
      for (int i = 0; i < 4; i++) { out[i] = p[i] / 255.0f; } break;
    case WGPUTextureFormat_R8Unorm: out[0] = p[0] / 255.0f; break;
    case WGPUTextureFormat_RG8Unorm: out[0] = p[0] / 255.0f; out[1] = p[1] / 255.0f; break;
    case WGPUTextureFormat_R16Float: out[0] = half(u16[0]); break;
    case WGPUTextureFormat_RG16Float: out[0] = half(u16[0]); out[1] = half(u16[1]); break;
    case WGPUTextureFormat_RGBA16Float:
      for (int i = 0; i < 4; i++) { out[i] = half(u16[i]); } break;
    case WGPUTextureFormat_R32Float: out[0] = f32[0]; break;
    case WGPUTextureFormat_RG32Float: out[0] = f32[0]; out[1] = f32[1]; break;
    case WGPUTextureFormat_RGBA32Float:
      for (int i = 0; i < 4; i++) { out[i] = f32[i]; } break;
    default:
      /* Best-effort: treat as RGBA8. */
      for (int i = 0; i < 4; i++) { out[i] = p[i] / 255.0f; } break;
  }
}

/* Block until `buf` is mapped for reading, then copy `size` bytes out. Needs
 * -sJSPI: wgpuInstanceWaitAny suspends the (pure-wasm) stack until the map
 * callback fires. Returns false on map failure or when suspension is not
 * possible (the wait would then abort — guarded by the caller's context). */
bool WebGPUContext::map_read_sync(WGPUBuffer buf, size_t size, void *dst)
{
  if (buf == nullptr || dst == nullptr || !emscripten_has_asyncify()) {
    return false;
  }
  struct MapState {
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
  } st;
  WGPUBufferMapCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_WaitAnyOnly;
  cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *ud1, void *) {
    static_cast<MapState *>(ud1)->status = status;
  };
  cb.userdata1 = &st;
  WGPUFuture fut = wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, cb);
  WGPUFutureWaitInfo wi = {fut, false};
  const WGPUWaitStatus ws = wgpuInstanceWaitAny(webgpu_instance(), 1, &wi, UINT64_MAX);
  if (ws != WGPUWaitStatus_Success || st.status != WGPUMapAsyncStatus_Success) {
    static int s_logged = 0;
    if (s_logged < 8) {
      s_logged++;
      fprintf(stderr, "WGPU_MAP sync map failed ws=%d st=%d\n", int(ws), int(st.status));
      fflush(stderr);
    }
    return false;
  }
  const void *p = wgpuBufferGetConstMappedRange(buf, 0, size);
  if (p == nullptr) {
    wgpuBufferUnmap(buf);
    return false;
  }
  std::memcpy(dst, p, size);
  wgpuBufferUnmap(buf);
  return true;
}

/* Shared texture/framebuffer readback: copy a region into reusable staging,
 * yield through JSPI until mapping completes, then convert into r_data. */
bool WebGPUContext::read_small_sync(WGPUTexture tex,
                                    WGPUTextureFormat fmt,
                                    int x,
                                    int y,
                                    int w,
                                    int h,
                                    eGPUDataFormat dst_format,
                                    int channels,
                                    void *r_data,
                                    int layer,
                                    int mip)
{
  if (r_data == nullptr || device_ == nullptr) {
    return false;
  }
  bool is_depth = false;
  bool half = false, normalized = false;
  uint32_t component_bytes = 4;
  switch (fmt) {
    case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_RG16Float:
    case WGPUTextureFormat_R16Float:
      if (dst_format != GPU_DATA_FLOAT && dst_format != GPU_DATA_HALF_FLOAT) return false;
      half = true; component_bytes = 2; break;
    case WGPUTextureFormat_RGBA32Float:
    case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_R32Float:
      if (dst_format != GPU_DATA_FLOAT) return false;
      break;
    case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RGBA32Uint:
      if (dst_format != GPU_DATA_UINT && dst_format != GPU_DATA_INT) return false;
      break;
    case WGPUTextureFormat_Depth32Float:
    case WGPUTextureFormat_Depth32FloatStencil8:
      if (channels != 1 || dst_format != GPU_DATA_FLOAT) return false;
      is_depth = true; break;
    case WGPUTextureFormat_R8Unorm:
    case WGPUTextureFormat_RG8Unorm:
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8UnormSrgb:
      if (dst_format != GPU_DATA_UBYTE && dst_format != GPU_DATA_FLOAT) return false;
      normalized = true; component_bytes = 1; break;
    default: return false;
  }
  const uint32_t bpp = webgpu_format_bytes_per_pixel(fmt);
  if (!bpp || bpp != uint32_t(channels) * component_bytes) return false;
  /* All recorded work must land before the copy (and the copy needs top-level
   * encoder access). */
  submit();
  const uint32_t bpr = align256(uint32_t(w) * bpp);
  const size_t need = size_t(bpr) * size_t(h);
  /* Keep one growable staging allocation per context. Readback is serialized
   * by the GPU-owner job; unmapping makes it available for the next pass. */
  if (!readback_buffer_ || readback_capacity_ < need) {
    if (readback_buffer_) wgpuBufferRelease(readback_buffer_);
    WGPUBufferDescriptor bd = {};
    bd.label = {"Blender readback staging", WGPU_STRLEN};
    bd.size = need;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readback_buffer_ = wgpuDeviceCreateBuffer(device_, &bd);
    readback_capacity_ = need;
  }
  WGPUBuffer scratch = readback_buffer_;
  if (!scratch) return false;
  WGPUCommandEncoder enc = ensure_encoder();
  if (enc == nullptr) {
    return false;
  }
  WGPUTexelCopyTextureInfo src = {};
  src.texture = tex;
  src.aspect = is_depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
  src.mipLevel = uint32_t(std::max(mip, 0));
  src.origin = {uint32_t(x), uint32_t(y), uint32_t(std::max(layer, 0))};
  WGPUTexelCopyBufferInfo dst = {};
  dst.buffer = scratch;
  dst.layout.bytesPerRow = bpr;
  dst.layout.rowsPerImage = uint32_t(h);
  WGPUExtent3D ext = {uint32_t(w), uint32_t(h), 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
  flush_encoder();

  std::vector<uint8_t> raw(need);
  const bool ok = map_read_sync(scratch, need, raw.data());
  if (!ok) {
    return false;
  }
  /* De-stride rows into the tightly packed caller buffer. Both sides use the
   * GL bottom-up row convention (the y passed in is already a storage row). */
  const size_t elements = size_t(w) * channels;
  const size_t output_bytes = dst_format == GPU_DATA_FLOAT ? 4 : component_bytes;
  for (int row = 0; row < h; row++) {
    const uint8_t *input = raw.data() + size_t(row) * bpr;
    uint8_t *output = static_cast<uint8_t *>(r_data) + size_t(row) * elements * output_bytes;
    if (half && dst_format == GPU_DATA_FLOAT) {
      math::half_to_float_array(reinterpret_cast<const uint16_t *>(input),
                                reinterpret_cast<float *>(output), elements);
    }
    else if (normalized && dst_format == GPU_DATA_FLOAT) {
      float *floats = reinterpret_cast<float *>(output);
      for (size_t i = 0; i < elements; i++) floats[i] = input[i] / 255.0f;
    }
    else std::memcpy(output, input, elements * component_bytes);
  }
  return true;
}

bool WebGPUContext::read_color_sync(WGPUTexture tex,
                                    WGPUTextureFormat fmt,
                                    int x,
                                    int y,
                                    int w,
                                    int h,
                                    eGPUDataFormat dst_format,
                                    int channels,
                                    void *r_data,
                                    int layer,
                                    int mip)
{
  if (device_ == nullptr || tex == nullptr || w <= 0 || h <= 0) {
    return false;
  }
  if (GHOST_web_gpu_job_active()) {
    const bool ok = read_small_sync(tex, fmt, x, y, w, h, dst_format, channels,
                                    r_data, layer, mip);
    fprintf(stderr, "WEB_READBACK %dx%d channels=%d format=%d success=%d\n",
            w, h, channels, int(fmt), int(ok));
    return ok;
  }
  /* Interactive entry points have not entered a promising Wasm call yet. */
  if (r_data) std::memset(r_data, 0, size_t(w) * h * channels *
      (dst_format == GPU_DATA_FLOAT || dst_format == GPU_DATA_UINT || dst_format == GPU_DATA_INT ? 4 :
       dst_format == GPU_DATA_HALF_FLOAT ? 2 : 1));
  return false;
}

void WebGPUContext::flush()
{
  submit();
}

void WebGPUContext::wait_for_queue()
{
  submit();
  /* Outside a promising GPU job, fence consumers are ordered uploads/draws.
   * They share this queue and WriteTexture has already copied its host bytes.
   * No mapped GPU memory or native graphics interop is exposed to CPU callers. */
  if (!queue_ || !emscripten_has_asyncify() || !GHOST_web_gpu_job_active()) return;
  WGPUQueueWorkDoneCallbackInfo callback = {};
  callback.mode = WGPUCallbackMode_WaitAnyOnly;
  callback.callback = [](WGPUQueueWorkDoneStatus value, WGPUStringView, void *, void *) {
    if (value != WGPUQueueWorkDoneStatus_Success) {
      fprintf(stderr, "Blender WebGPU error: queue fence completion failed\n");
    }
  };
  const WGPUFuture future = wgpuQueueOnSubmittedWorkDone(queue_, callback);
  WGPUFutureWaitInfo info = {future, false};
  if (wgpuInstanceWaitAny(webgpu_instance(), 1, &info, UINT64_MAX) != WGPUWaitStatus_Success)
  {
    fprintf(stderr, "Blender WebGPU error: queue fence failed\n");
  }
}

void WebGPUContext::finish()
{
  /* No synchronous GPU wait is possible without JSPI; submit and rely on the
   * browser event loop to drain the queue once wasm yields. */
  submit();
}

}  // namespace blender::gpu

/* --- JS-facing capture exports (called by the harness after "Blender quit") ---
 * The render submitted a copyTextureToBuffer into g_capture_buf; here we map it
 * (async, on the now-free browser event loop) and stage the pixels for readback. */
#ifdef __EMSCRIPTEN__
static void webgpu_capture_map_cb(WGPUMapAsyncStatus status,
                                  WGPUStringView /*msg*/,
                                  void * /*u1*/,
                                  void * /*u2*/)
{
  if (status != WGPUMapAsyncStatus_Success || g_capture_buf == nullptr) {
    g_capture_ready = -1;
    return;
  }
  const size_t size = size_t(g_capture_bpr) * g_capture_h;
  const void *p = wgpuBufferGetConstMappedRange(g_capture_buf, 0, size);
  if (p == nullptr) {
    g_capture_ready = -1;
    return;
  }
  free(g_capture_pixels);
  g_capture_pixels = static_cast<uint8_t *>(malloc(size));
  memcpy(g_capture_pixels, p, size);
  wgpuBufferUnmap(g_capture_buf);
  g_capture_ready = 1;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE int wgpu_capture_w() { return int(g_capture_w); }
EMSCRIPTEN_KEEPALIVE int wgpu_capture_h() { return int(g_capture_h); }
EMSCRIPTEN_KEEPALIVE int wgpu_capture_bpr() { return int(g_capture_bpr); }
EMSCRIPTEN_KEEPALIVE int wgpu_capture_bpp() { return int(g_capture_bpp); }
EMSCRIPTEN_KEEPALIVE int wgpu_capture_ready() { return g_capture_ready; }
EMSCRIPTEN_KEEPALIVE uint8_t *wgpu_capture_ptr() { return g_capture_pixels; }
EMSCRIPTEN_KEEPALIVE void wgpu_capture_map()
{
  if (g_capture_buf == nullptr) {
    g_capture_ready = -1;
    return;
  }
  g_capture_ready = 0;
  WGPUBufferMapCallbackInfo ci = {};
  ci.mode = WGPUCallbackMode_AllowSpontaneous;
  ci.callback = webgpu_capture_map_cb;
  wgpuBufferMapAsync(
      g_capture_buf, WGPUMapMode_Read, 0, size_t(g_capture_bpr) * g_capture_h, ci);
}
}
#endif
