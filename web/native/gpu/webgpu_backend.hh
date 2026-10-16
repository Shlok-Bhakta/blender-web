/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU GPU backend (Emscripten / Dawn). Targets the browser's native WebGPU
 * via the emdawnwebgpu port — the path that lets EEVEE-Next run in the browser
 * (compute shaders + storage buffers, which WebGL2 lacks). WebGPU primitives
 * (compute, render-to-texture, no JSPI) were verified standalone before this
 * scaffold; see the smoke/wgpu_*.c probes and the webgpu-verified memory.
 *
 */

#pragma once

#include <cstdio>

#include "GPU_capabilities.hh"
#include "GPU_worker.hh"

#include "gpu_backend.hh"
#include "gpu_capabilities_private.hh"
#include "gpu_platform_private.hh"
#include "gpu_shader_private.hh"

#include "webgpu_batch.hh"
#include "webgpu_context.hh"
#include "webgpu_framebuffer.hh"
#include "webgpu_index_buffer.hh"
#include "webgpu_shader.hh"
#include "webgpu_storage_buffer.hh"
#include "webgpu_texture.hh"
#include "webgpu_texture_pool.hh"
#include "webgpu_uniform_buffer.hh"
#include "webgpu_vertex_buffer.hh"

#include <webgpu/webgpu.h>

#include "gpu_query.hh"

namespace blender::gpu {

/* Real WebGPU occlusion queries with ONE-ROUND-STALE results: the async resolve
 * of round N is served to round N+1 (get_occlusion_result is synchronous but
 * mapping is not). Gizmo highlight re-queries on every cursor move, so the lag
 * is one mouse event — this is what makes gizmo hover/drag functional. */
class WebGPUQueryPool : public QueryPool {
 private:
  int query_count_ = 0;

 public:
  void init(GPUQueryType /*type*/) override
  {
    query_count_ = 0;
    if (WebGPUContext *ctx = WebGPUContext::get()) {
      ctx->occlusion_pool_begin(256);
    }
  }
  void begin_query() override
  {
    if (WebGPUContext *ctx = WebGPUContext::get()) {
      ctx->occlusion_query_begin(query_count_);
    }
    query_count_++;
  }
  void end_query() override
  {
    if (WebGPUContext *ctx = WebGPUContext::get()) {
      ctx->occlusion_query_end();
    }
  }
  void get_occlusion_result(MutableSpan<uint32_t> r_values) override
  {
    for (uint32_t &v : r_values) {
      v = 0;
    }
    if (WebGPUContext *ctx = WebGPUContext::get()) {
      ctx->occlusion_pool_read(uint32_t(std::min<int64_t>(r_values.size(), query_count_)),
                               r_values.data());
    }
  }
};

class WebGPUBackend : public GPUBackend {
 private:
  WGPUInstance instance_ = nullptr;

 public:
  WebGPUBackend()
  {
    instance_ = wgpuCreateInstance(nullptr);
    GPG.init(GPU_DEVICE_ANY,
             GPU_OS_ANY,
             GPU_DRIVER_ANY,
             GPU_SUPPORT_LEVEL_SUPPORTED,
             GPU_BACKEND_WEBGPU,
             "WebGPU",
             "",
             "",
             GPU_ARCHITECTURE_IMR);
  }
  ~WebGPUBackend() override
  {
    if (instance_) {
      wgpuInstanceRelease(instance_);
    }
  }

  void init_resources() override
  {
    /* Force synchronous, main-thread shader compilation: this leaves
     * ShaderCompiler::compilation_worker_ null so async_compilation() compiles
     * inline on the calling thread. Avoids a GPUWorker background thread racing
     * the main thread (which corrupted the heap), and sidesteps WebGPU device
     * sharing across worker threads — neither is wired for wasm yet. */
    GCaps.use_main_context_workaround = true;
    /* WebGPU has no snorm 10_10_10_2 vertex format; make mesh extraction emit
     * I16 (Snorm16x4) normals instead (same workaround as old AMD GL drivers). */
    GCaps.use_hq_normals_workaround = true;

    /* WebGPU device capabilities. The device is created later (in the context),
     * so we cannot query it here; use values matching Dawn's common per-stage
     * limits, which the browser then re-validates at pipeline creation. Without
     * this, every limit is 0 and the GPU module rejects any shader that uses a
     * sampler ("too many samplers"), a storage buffer, etc. */
    GCaps.max_texture_size = 8192;
    GCaps.max_texture_3d_size = 2048;
    GCaps.max_texture_layers = 256;
    GCaps.max_textures = 32;        /* Blender slot space (bind tables are 32-wide) */
    GCaps.max_images = 16;          /* Blender slot space */
    GCaps.max_work_group_count[0] = 65535;
    GCaps.max_work_group_count[1] = 65535;
    GCaps.max_work_group_count[2] = 65535;
    GCaps.max_work_group_size[0] = 256;
    GCaps.max_work_group_size[1] = 256;
    GCaps.max_work_group_size[2] = 64;
    GCaps.max_uniforms_vert = 1024;
    GCaps.max_uniforms_frag = 1024;
    GCaps.max_batch_indices = 1 << 24;
    GCaps.max_batch_vertices = 1 << 24;
    GCaps.max_vertex_attribs = 16;
    GCaps.max_varying_floats = 60;  /* maxInterStageShaderComponents */
    GCaps.max_shader_storage_buffer_bindings = 8;
    GCaps.max_compute_shader_storage_blocks = 8;
    GCaps.max_uniform_buffer_size = 65536;
    GCaps.max_storage_buffer_size = size_t(1) << 27; /* 128 MB */
    GCaps.storage_buffer_alignment = 256;
    GCaps.max_parallel_compilations = 0; /* synchronous main-thread compilation */
    GCaps.mem_stats_support = false;
    GCaps.geometry_shader_support = false;
    /* The generic ShaderCompiler drives shader compilation; without it
     * get_compiler() returns the uninitialized compiler_ pointer and any
     * compilation aborts. */
    compiler_ = MEM_new<ShaderCompiler>(
        __func__, 1, GPUWorker::ContextType::Main);
  }
  void delete_resources() override
  {
    if (compiler_) {
      MEM_delete(compiler_);
      compiler_ = nullptr;
    }
  }

  void compute_dispatch(int groups_x_len, int groups_y_len, int groups_z_len) override
  {
    WebGPUContext *ctx = WebGPUContext::get();
    {
      static int s_disp_entry = 0;
      if (s_disp_entry < 8) {
        WebGPUShader *es = ctx ? static_cast<WebGPUShader *>(ctx->shader) : nullptr;
        fprintf(stderr,
                "WGPU_DISPATCH entry #%d ctx=%p dev=%p shader=%p cmod=%p\n",
                s_disp_entry++,
                (void *)ctx,
                ctx ? (void *)ctx->device() : nullptr,
                (void *)es,
                es ? (void *)es->compute_module() : nullptr);
        fflush(stderr);
      }
    }
    if (ctx == nullptr || ctx->device() == nullptr) {
      return;
    }
    WebGPUShader *sh = static_cast<WebGPUShader *>(ctx->shader);
    if (sh == nullptr || !sh->is_valid() || sh->compute_module() == nullptr ||
        sh->interface == nullptr)
    {
      static int s_skip_logged = 0;
      if (sh && s_skip_logged < 200) {
        s_skip_logged++;
        fprintf(stderr,
                "WGPU_DISPATCH skipped '%s' valid=%d cmod=%p iface=%p\n",
                sh->name_get().c_str(),
                int(sh->is_valid()),
                (void *)sh->compute_module(),
                (void *)sh->interface);
        fflush(stderr);
      }
      return;
    }
    const uint64_t key = uint64_t(uintptr_t(sh)) ^ sh->spec_hash();
    WGPUComputePipeline pipe = ctx->compute_pipeline_get(key);
    if (pipe == nullptr) {
      WGPUBindGroupLayout bgl_explicit = ctx->make_bind_group_layout(
          sh->compute_bindings(), true, sh->interface);
      WGPUPipelineLayout pipe_layout = ctx->make_pipeline_layout(bgl_explicit);
      WGPUComputePipelineDescriptor d = {};
      /* Label = shader name so Dawn validation errors are attributable. */
      d.label = {sh->name_get().c_str(), WGPU_STRLEN};
      d.layout = pipe_layout;
      d.compute.module = sh->compute_module();
      d.compute.entryPoint = {"main", WGPU_STRLEN};
      std::vector<WGPUConstantEntry> spec_consts;
      sh->spec_entries(sh->compute_wgsl(), spec_consts);
      d.compute.constantCount = spec_consts.size();
      d.compute.constants = spec_consts.empty() ? nullptr : spec_consts.data();
      pipe = wgpuDeviceCreateComputePipeline(ctx->device(), &d);
      if (pipe_layout) {
        wgpuPipelineLayoutRelease(pipe_layout);
      }
      if (bgl_explicit) {
        wgpuBindGroupLayoutRelease(bgl_explicit);
      }
      if (pipe == nullptr) {
        return;
      }
      ctx->compute_pipeline_put(key, pipe);
    }
    /* Build the bind group BEFORE opening the compute pass: assembly can record
     * copies/uploads (snapshot_for_sampling, lazy uploads) that must land
     * outside any pass. Compute also cannot be recorded inside a render pass. */
    if (getenv("WGPU_LOG_RP") && ctx->render_pass() != nullptr) {
      fprintf(stderr, "WGPU_SPLIT compute '%s'\n", sh->name_get().c_str());
      fflush(stderr);
    }
    ctx->render_pass_end();
    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroup bg = ctx->build_bind_group(sh, sh->compute_bindings(), bgl);
    WGPUCommandEncoder enc = ctx->ensure_encoder();
    if (enc == nullptr) {
      wgpuBindGroupLayoutRelease(bgl);
      return;
    }
    WGPUComputePassEncoder cpass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    static int s_dispatch_log = 0;
    if (s_dispatch_log < 16) {
      fprintf(stderr,
              "WGPU_DISPATCH #%d shader='%s' groups=%d,%d,%d bg=%p\n",
              s_dispatch_log++,
              sh->name_get().c_str(),
              groups_x_len,
              groups_y_len,
              groups_z_len,
              (void *)bg);
      fflush(stderr);
    }
    /* A dispatch without its bind group is a validation error that POISONS the
     * whole command buffer at submit (dropping all sibling passes) — skip it. */
    if (bg) {
      wgpuComputePassEncoderSetPipeline(cpass, pipe);
      wgpuComputePassEncoderSetBindGroup(cpass, 0, bg, 0, nullptr);
      wgpuComputePassEncoderDispatchWorkgroups(
          cpass, uint32_t(groups_x_len), uint32_t(groups_y_len), uint32_t(groups_z_len));
    }
    wgpuComputePassEncoderEnd(cpass);
    wgpuComputePassEncoderRelease(cpass);
    if (bg) {
      wgpuBindGroupRelease(bg);
    }
    wgpuBindGroupLayoutRelease(bgl);
    /* Isolate this dispatch in its own command buffer (see flush_encoder). */
    ctx->flush_encoder();
    debug_sum_ssbos(ctx, sh);
  }
  /* DEBUG: ENV.WGPU_SUM_SHADER=<substr> — after each matching dispatch, copy
   * bound SSBO slots (and WGPU_SUM_TEX=<name substr> texture layers) into
   * staging buffers and checksum them via ASYNC maps. Blocking maps need
   * JSPI/Asyncify which this build lacks — async callbacks fire in queue
   * completion order, so the seq tags keep prints attributable. Unlike
   * WGPU_CAP_SSBO this does NOT suppress film output, so runs stay scoreable. */
  struct DebugSumCtx {
    WGPUBuffer st;
    size_t size;
    char tag[160];
  };
  static void debug_sum_async(WebGPUContext *ctx, const char *tag, WGPUBuffer st, size_t size)
  {
    DebugSumCtx *mc = new DebugSumCtx{st, size, {}};
    snprintf(mc->tag, sizeof(mc->tag), "%s", tag);
    WGPUBufferMapCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.userdata1 = mc;
    cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *ud1, void *) {
      DebugSumCtx *mc = static_cast<DebugSumCtx *>(ud1);
      if (status == WGPUMapAsyncStatus_Success) {
        const uint32_t *v = static_cast<const uint32_t *>(
            wgpuBufferGetConstMappedRange(mc->st, 0, mc->size));
        if (v) {
          uint32_t sum = 5381, nz = 0, nonfinite = 0;
          for (size_t i = 0; i < mc->size / 4; i++) {
            sum = sum * 33 + v[i];
            nz += (v[i] != 0u);
            /* Interpreted as f32: exponent all-ones = Inf/NaN. Also catches
             * f16 pairs loosely via the high half (good enough for triage). */
            nonfinite += ((v[i] & 0x7f800000u) == 0x7f800000u);
            nonfinite += ((v[i] & 0x7c000000u) == 0x7c000000u); /* f16 hi */
            nonfinite += ((v[i] & 0x00007c00u) == 0x00007c00u); /* f16 lo */
          }
          /* f16 max magnitude (treat words as 2x half): decode exponent/mantissa. */
          float h16max = 0.0f;
          for (size_t i = 0; i < mc->size / 4; i++) {
            for (int hw = 0; hw < 2; hw++) {
              const uint32_t h = (v[i] >> (hw * 16)) & 0xffffu;
              const int e = int((h >> 10) & 0x1f);
              const int m = int(h & 0x3ff);
              if (e == 0x1f) {
                continue; /* inf/nan counted above */
              }
              const float val = (e == 0) ? (m / 1024.0f) * 6.1e-5f :
                                           (1.0f + m / 1024.0f) * exp2f(float(e - 15));
              h16max = std::max(h16max, val);
            }
          }
          fprintf(stderr, "WGPU_ASUM %s sum=%08x nz=%u nf=%u h16max=%.3g\n",
                  mc->tag, sum, nz, nonfinite, h16max);
          /* ENV.WGPU_DUMP_SSBO=<substr of tag>: hex-dump the first words to
           * locate WHICH field diverges between runs. */
          const char *dump = getenv("WGPU_DUMP_SSBO");
          if (dump && strstr(mc->tag, dump)) {
            const size_t n = std::min<size_t>(mc->size / 4, 128);
            for (size_t i = 0; i < n; i += 8) {
              fprintf(stderr,
                      "WGPU_DUMP %s +%03zu %08x %08x %08x %08x %08x %08x %08x %08x\n",
                      mc->tag, i,
                      v[i], v[i + 1], v[i + 2], v[i + 3],
                      v[i + 4], v[i + 5], v[i + 6], v[i + 7]);
            }
          }
          fflush(stderr);
        }
        wgpuBufferUnmap(mc->st);
      }
      else {
        fprintf(stderr, "WGPU_ASUM %s MAP_FAILED st=%d\n", mc->tag, int(status));
        fflush(stderr);
      }
      wgpuBufferRelease(mc->st);
      delete mc;
    };
    wgpuBufferMapAsync(st, WGPUMapMode_Read, 0, size, cb);
    (void)ctx;
  }
  static void debug_sum_ssbos(WebGPUContext *ctx, WebGPUShader *sh)
  {
    const char *pat = getenv("WGPU_SUM_SHADER");
    const char *shader_name = sh->name_get().c_str();
    if (pat == nullptr || strstr(shader_name, pat) == nullptr) {
      return;
    }
    static int s_seq = 0;
    const int seq = s_seq++;
    char tag[160];
    /* Only the SSBOs this shader DECLARES — the raw context bind table holds
     * stale slots from earlier work, which fingerprint as phantom divergence. */
    const ShaderInterface *iface = sh->interface;
    const ShaderInput *ssbos = iface->inputs_ + iface->attr_len_ + iface->ubo_len_ +
                               iface->uniform_len_;
    for (uint i = 0; i < iface->ssbo_len_; i++) {
      const ShaderInput *si = ssbos + i;
      WGPUBuffer b = ctx->ssbo_at(si->location);
      if (b == nullptr) {
        continue;
      }
      const size_t sz = std::min<size_t>(size_t(wgpuBufferGetSize(b)), 262144);
      const size_t asz = (sz + 3) & ~size_t(3);
      WGPUBufferDescriptor bd = {};
      bd.size = asz;
      bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
      WGPUBuffer st = wgpuDeviceCreateBuffer(ctx->device(), &bd);
      if (st == nullptr) {
        continue;
      }
      WGPUCommandEncoder e2 = ctx->ensure_encoder();
      if (e2 == nullptr) {
        wgpuBufferRelease(st);
        continue;
      }
      wgpuCommandEncoderCopyBufferToBuffer(e2, b, 0, st, 0, asz);
      ctx->flush_encoder();
      snprintf(tag,
               sizeof(tag),
               "#%d '%s' %s",
               seq,
               shader_name,
               iface->input_name_get(si));
      debug_sum_async(ctx, tag, st, asz);
    }
    if (getenv("WGPU_SUM_UBO")) {
      /* Same as the SSBO capture but for the shader's declared UBOs — uniform
       * inputs (dof_buf, view matrices) are invisible to texture probes yet
       * fully determine per-pixel branches like DoF's focus classification. */
      const ShaderInput *ubos = iface->inputs_ + iface->attr_len_;
      for (uint i = 0; i < iface->ubo_len_; i++) {
        const ShaderInput *ui = ubos + i;
        /* UBOs: ->binding is the app slot (bound_ubo_ index); ->location is the
         * flat WGSL binding (bind-group space). SSBOs have both == slot. */
        WGPUBuffer b = ctx->ubo_at(ui->binding);
        if (b == nullptr) {
          continue;
        }
        if (!(wgpuBufferGetUsage(b) & WGPUBufferUsage_CopySrc)) {
          /* Copying a non-CopySrc buffer fails validation and INVALIDATES the
           * whole command buffer — every later pass in it would be dropped,
           * turning the probe itself into the bug being chased. */
          fprintf(stderr, "WGPU_ASUM #%d ubo '%s' NO_COPY_SRC\n", seq, iface->input_name_get(ui));
          continue;
        }
        const size_t sz = std::min<size_t>(size_t(wgpuBufferGetSize(b)), 4096);
        const size_t asz = (sz + 3) & ~size_t(3);
        WGPUBufferDescriptor bd = {};
        bd.size = asz;
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        WGPUBuffer st = wgpuDeviceCreateBuffer(ctx->device(), &bd);
        if (st == nullptr) {
          continue;
        }
        WGPUCommandEncoder e2 = ctx->ensure_encoder();
        if (e2 == nullptr) {
          wgpuBufferRelease(st);
          continue;
        }
        wgpuCommandEncoderCopyBufferToBuffer(e2, b, 0, st, 0, asz);
        ctx->flush_encoder();
        snprintf(tag, sizeof(tag), "#%d ubo '%s'", seq, iface->input_name_get(ui));
        debug_sum_async(ctx, tag, st, asz);
      }
    }
    if (getenv("WGPU_SUM_BOUND")) {
      /* Report THE SHADER'S declared texture bindings (raw bind-table slots
       * include stale leftovers from other shaders — the classic trap). */
      const ShaderInterface *bif = sh->interface;
      for (uint i = 0; i < bif->uniform_len_; i++) {
        const ShaderInput *in = bif->inputs_ + bif->attr_len_ + bif->ubo_len_ + i;
        if (in->binding < 0 || in->binding >= 32) {
          continue; /* Not a texture sampler slot. */
        }
        const char *bname = bif->input_name_get(in);
        const size_t blen = strlen(bname);
        /* Images and samplers share the uniform subarray; the slot spaces are
         * separate, so the '_img' suffix alone misroutes names like
         * out_radiance_mip0. Use the interface masks; suffix only breaks ties
         * when a slot is enabled in both spaces. */
        const bool suffix_img = blen > 4 && strcmp(bname + blen - 4, "_img") == 0;
        const bool in_ima = in->binding < 8 && (bif->enabled_ima_mask_ >> in->binding) & 1;
        const bool in_tex = (bif->enabled_tex_mask_ >> in->binding) & 1;
        const bool is_img = in_ima && (!in_tex || suffix_img);
        WebGPUTexture *bt = is_img ? ctx->bound_image_get(in->binding) :
                                     ctx->bound_tex_get(in->binding);
        fprintf(stderr, "WGPU_BOUND #%d '%s' %s=%d -> %s@%p wgpu=%p %dx%dx%d fmt=%d view=%d mip=%d layer=%d\n",
                seq, bname, is_img ? "img" : "tex", in->binding,
                bt ? bt->debug_name().c_str() : "NULL", (void *)bt,
                bt ? (void *)bt->wgpu_texture() : nullptr,
                bt ? bt->width_get() : 0, bt ? bt->height_get() : 0,
                bt ? std::max(bt->depth_get(), 1) : 0,
                (bt && bt->wgpu_texture()) ? int(wgpuTextureGetFormat(bt->wgpu_texture())) : -1,
                bt ? int(bt->is_view()) : 0, bt ? bt->view_mip() : 0,
                bt ? bt->view_layer() : 0);
      }
      fflush(stderr);
    }
    const char *tpat = getenv("WGPU_SUM_TEX");
    const char *tfmt = getenv("WGPU_SUM_TEX_FMT"); /* Match by WGPUTextureFormat
        int + depth>1 instead of name (DRW Texture members are often unnamed). */
    if (tpat || tfmt) {
      int tex_matches = 0;
      /* Dispatch-level sampling gate (was inside the loop, capping captures
       * to ONE texture per run). */
      static int s_tex_nth = 0;
      const char *nth_env = getenv("WGPU_SUM_TEX_NTH");
      const int nth = nth_env ? atoi(nth_env) : 8;
      const bool skip_this_dispatch = (s_tex_nth++ % std::max(nth, 1)) != 0;
      extern std::vector<WebGPUTexture *> g_wgpu_live_textures;
      for (WebGPUTexture *t : g_wgpu_live_textures) {
        if (t->wgpu_texture() == nullptr) {
          continue;
        }
        if (tfmt) {
          if (int(wgpuTextureGetFormat(t->wgpu_texture())) != atoi(tfmt)) {
            continue;
          }
        }
        else if (strstr(t->debug_name().c_str(), tpat) == nullptr) {
          continue;
        }
        /* Big textures (shadow atlas = 512MB): sample every Nth match, first
         * WGPU_SUM_TEX_LAYERS layers only, one async staging buffer per layer. */
        if (skip_this_dispatch) {
          break;
        }
        const uint32_t w = uint32_t(std::max(t->width_get(), 1));
        const uint32_t h = uint32_t(std::max(t->height_get(), 1));
        const uint32_t layers = uint32_t(std::max(t->depth_get(), 1));
        const char *lay_env = getenv("WGPU_SUM_TEX_LAYERS");
        const uint32_t max_layers = std::min<uint32_t>(layers, lay_env ? atoi(lay_env) : 4);
        extern uint32_t webgpu_format_bytes_per_pixel(WGPUTextureFormat f);
        const uint32_t bpp = webgpu_format_bytes_per_pixel(
            wgpuTextureGetFormat(t->wgpu_texture()));
        const uint32_t arow = ((w * bpp) + 255u) & ~255u;
        const uint64_t lsz = uint64_t(arow) * h;
        if (lsz > (64u << 20)) {
          break;
        }
        for (uint32_t l = 0; l < max_layers; l++) {
          WGPUBufferDescriptor bd = {};
          bd.size = lsz;
          bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
          WGPUBuffer st = wgpuDeviceCreateBuffer(ctx->device(), &bd);
          if (st == nullptr) {
            break;
          }
          WGPUCommandEncoder e2 = ctx->ensure_encoder();
          if (e2 == nullptr) {
            wgpuBufferRelease(st);
            break;
          }
          WGPUTexelCopyTextureInfo src = {};
          src.texture = t->wgpu_texture();
          src.origin = {0, 0, l};
          WGPUTexelCopyBufferInfo dst = {};
          dst.buffer = st;
          dst.layout.bytesPerRow = arow;
          dst.layout.rowsPerImage = h;
          WGPUExtent3D ext = {w, h, 1};
          wgpuCommandEncoderCopyTextureToBuffer(e2, &src, &dst, &ext);
          ctx->flush_encoder();
          snprintf(tag,
                   sizeof(tag),
                   "#%d tex%d '%s'@%p %ux%u l%u",
                   seq,
                   tex_matches,
                   t->debug_name().c_str(),
                   (void *)t,
                   w,
                   h,
                   l);
          debug_sum_async(ctx, tag, st, size_t(lsz));
        }
        const char *cap_env = getenv("WGPU_SUM_TEX_MAX");
        if (++tex_matches >= (cap_env ? atoi(cap_env) : 8)) {
          break; /* Cap per-dispatch texture captures. */
        }
      }
    }
  }
  void compute_dispatch_indirect(StorageBuf *indirect_buf) override
  {
    /* Same as compute_dispatch, but the workgroup counts come from a GPU buffer
     * (e.g. eevee_shadow_page_clear sized by the rendermap pass). */
    WebGPUContext *ctx = WebGPUContext::get();
    if (ctx == nullptr || ctx->device() == nullptr || indirect_buf == nullptr) {
      return;
    }
    WebGPUShader *sh = static_cast<WebGPUShader *>(ctx->shader);
    if (sh == nullptr || !sh->is_valid() || sh->compute_module() == nullptr ||
        sh->interface == nullptr)
    {
      return;
    }
    const uint64_t key = uint64_t(uintptr_t(sh)) ^ sh->spec_hash();
    WGPUComputePipeline pipe = ctx->compute_pipeline_get(key);
    if (pipe == nullptr) {
      WGPUBindGroupLayout bgl_explicit = ctx->make_bind_group_layout(
          sh->compute_bindings(), true, sh->interface);
      WGPUPipelineLayout pipe_layout = ctx->make_pipeline_layout(bgl_explicit);
      WGPUComputePipelineDescriptor d = {};
      d.label = {sh->name_get().c_str(), WGPU_STRLEN};
      d.layout = pipe_layout;
      d.compute.module = sh->compute_module();
      d.compute.entryPoint = {"main", WGPU_STRLEN};
      std::vector<WGPUConstantEntry> spec_consts;
      sh->spec_entries(sh->compute_wgsl(), spec_consts);
      d.compute.constantCount = spec_consts.size();
      d.compute.constants = spec_consts.empty() ? nullptr : spec_consts.data();
      pipe = wgpuDeviceCreateComputePipeline(ctx->device(), &d);
      if (pipe_layout) {
        wgpuPipelineLayoutRelease(pipe_layout);
      }
      if (bgl_explicit) {
        wgpuBindGroupLayoutRelease(bgl_explicit);
      }
      if (pipe == nullptr) {
        return;
      }
      ctx->compute_pipeline_put(key, pipe);
    }
    WGPUBuffer ind = static_cast<WebGPUStorageBuf *>(indirect_buf)->buffer();
    if (ind == nullptr) {
      return;
    }
    ctx->render_pass_end();
    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroup bg = ctx->build_bind_group(sh, sh->compute_bindings(), bgl);
    WGPUCommandEncoder enc = ctx->ensure_encoder();
    if (enc == nullptr) {
      wgpuBindGroupLayoutRelease(bgl);
      return;
    }
    WGPUComputePassEncoder cpass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
    if (bg) {
      wgpuComputePassEncoderSetPipeline(cpass, pipe);
      wgpuComputePassEncoderSetBindGroup(cpass, 0, bg, 0, nullptr);
      wgpuComputePassEncoderDispatchWorkgroupsIndirect(cpass, ind, 0);
    }
    wgpuComputePassEncoderEnd(cpass);
    wgpuComputePassEncoderRelease(cpass);
    if (bg) {
      wgpuBindGroupRelease(bg);
    }
    wgpuBindGroupLayoutRelease(bgl);
    ctx->flush_encoder();
    debug_sum_ssbos(ctx, sh);
  }

  Context *context_alloc(GHOST_IWindow *ghost_window, GHOST_IContext *ghost_context) override
  {
    return new WebGPUContext(ghost_window, ghost_context);
  }

  Batch *batch_alloc() override { return new WebGPUBatch; }
  FrameBuffer *framebuffer_alloc(const char *name) override { return new WebGPUFrameBuffer(name); }
  VertBuf *vertbuf_alloc() override { return new WebGPUVertexBuffer; }
  Shader *shader_alloc(const char *name) override { return new WebGPUShader(name); }
  Texture *texture_alloc(const char *name) override { return new WebGPUTexture(name); }
  TexturePool *texturepool_alloc() override { return new WebGPUTexturePool(); }
  UniformBuf *uniformbuf_alloc(size_t size, const char *name) override
  {
    return new WebGPUUniformBuf(size, name);
  }
  StorageBuf *storagebuf_alloc(size_t size, GPUUsageType usage, const char *name) override
  {
    return new WebGPUStorageBuf(size, usage, name);
  }

  IndexBuf *indexbuf_alloc() override { return new WebGPUIndexBuf; }

  /* TODO: still stubs — implement as the render path starts exercising them. */
  Fence *fence_alloc() override { return nullptr; }
  PixelBuffer *pixelbuf_alloc(size_t /*size*/) override { return nullptr; }
  /* Occlusion queries: WebGPU supports them, but reading results back is async
   * (ResolveQuerySet → buffer map) while get_occlusion_result() is synchronous —
   * same no-JSPI wall as all readbacks. Return a counting stub with zero
   * samples: gizmo hover/click (gpu_select_sample_query) finds nothing instead
   * of DEREFERENCING NULL and killing the main loop ("memory access out of
   * bounds" on the first gizmo-area drag). */
  QueryPool *querypool_alloc() override { return new WebGPUQueryPool; }

  void shader_cache_dir_clear_old() override {}
  void render_begin() override {}
  void render_end() override
  {
    /* Flush any recorded commands so the final frame (and the debug capture copy)
     * actually executes on the GPU. */
    if (WebGPUContext *ctx = WebGPUContext::get()) {
      ctx->submit();
    }
  }
  void render_step(bool /*force_resource_release*/) override
  {
    /* EEVEE calls GPU_render_step() after each sample; WebGPU needs an explicit
     * submit for recorded draws/dispatches to run (it doesn't call GPU_flush for
     * our backend). Submit here so work is paced per sample, not buffered into one
     * giant command buffer. */
    if (WebGPUContext *ctx = WebGPUContext::get()) {
      ctx->submit();
    }
  }

  WGPUInstance instance() const { return instance_; }
};

}  // namespace blender::gpu
