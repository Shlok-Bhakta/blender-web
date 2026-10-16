/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cmath>

#include "gpu_backend.hh"

#include "webgpu_texture.hh"
#include "webgpu_texture_pool.hh"

namespace blender::gpu {

WebGPUTexturePool::~WebGPUTexturePool()
{
  for (FreeTexture &ft : pool_) {
    delete ft.texture;
  }
  pool_.clear();
  for (auto item : acquired_.keys()) {
    delete item;
  }
  acquired_.clear();
}

Texture *WebGPUTexturePool::acquire_texture_impl(int3 extent,
                                                 int mip_len,
                                                 GPUTextureType type,
                                                 TextureFormat format,
                                                 eGPUTextureUsage usage,
                                                 const char *name)
{
  /* Clamp mip count to the maximum supported by the largest dimension. */
  const int max_dim = std::max({extent.x, extent.y, extent.z});
  const int mip_len_max = 1 + int(std::floor(std::log2(float(std::max(max_dim, 1)))));
  mip_len = std::min(mip_len, mip_len_max);

  /* Reuse a compatible free texture. */
  for (int64_t i : pool_.index_range()) {
    Texture *t = pool_[i].texture;
    if (t->format_get() == format && t->width_get() == extent.x &&
        t->height_get() == extent.y && t->depth_get() == extent.z &&
        t->mip_count() == mip_len &&
        /* The wgpu-side usage is fixed at creation; usage_set() below only
         * updates bookkeeping. Reusing a texture whose created usage lacks a
         * requested bit (e.g. RenderAttachment for DoF's scatter target)
         * fails render-pass validation and kills the entire command buffer,
         * dropping every later pass in the frame. */
        static_cast<WebGPUTexture *>(t)->wgpu_usage_covers(usage))
    {
      pool_.remove_and_reorder(i);
      t->usage_set(usage | GPU_TEXTURE_USAGE_FORMAT_VIEW);
      acquired_.add(t, 0);
      if (name) {
        /* Re-label on every acquisition: pool textures otherwise keep their
         * FIRST user's name forever, which breaks name-matched debug probes
         * and mislabels captures. */
        static_cast<WebGPUTexture *>(t)->debug_rename(name);
      }
      /* DEBUG: ENV.WGPU_POOL_CLEAR=1 — zero reused textures so reuse behaves
       * like a fresh (spec-zeroed) allocation. Bisects read-before-write bugs
       * on pooled textures: pool assignment order varies run-to-run, so an
       * unwritten read inherits bistable content. */
      if (getenv("WGPU_POOL_CLEAR")) {
        static_cast<WebGPUTexture *>(t)->clear(double4(0.0));
      }
      return t;
    }
  }

  /* Allocate a fresh one. */
  Texture *texture = GPUBackend::get()->texture_alloc(name ? name : "TexFromPool");
  texture->usage_set(usage | GPU_TEXTURE_USAGE_FORMAT_VIEW);
  switch (type) {
    case GPU_TEXTURE_1D:
    case GPU_TEXTURE_1D_ARRAY:
      texture->init_1D(extent.x, extent.y, mip_len, format);
      break;
    case GPU_TEXTURE_2D:
    case GPU_TEXTURE_2D_ARRAY:
      texture->init_2D(extent.x, extent.y, extent.z, mip_len, format);
      break;
    case GPU_TEXTURE_3D:
      texture->init_3D(extent.x, extent.y, extent.z, mip_len, format);
      break;
    case GPU_TEXTURE_CUBE:
    case GPU_TEXTURE_CUBE_ARRAY:
      texture->init_cubemap(extent.x, extent.y, mip_len, format);
      break;
    default:
      break;
  }
  acquired_.add(texture, 0);
  return texture;
}

void WebGPUTexturePool::offset_users_count(Texture *tex, int offset)
{
  /* VALIDATION COUNTER ONLY — upstream (gpu_texture_pool.cc) semantics.
   * TextureFromPool::retain() calls this with -1 meaning "keep me alive into
   * the next cycle"; a texture only returns to the free list via an explicit
   * release_texture(). The previous refcount-style implementation FREED the
   * texture when the count hit zero, so retain() (DoF stabilize history,
   * raytrace denoise histories) put live textures back in the pool — two
   * users then shared one texture, and two writable bindings of the same
   * subresource kill the whole command buffer on WebGPU. */
  int *users = acquired_.lookup_ptr(tex);
  if (users == nullptr) {
    return;
  }
  *users += offset;
}

void WebGPUTexturePool::release_texture(Texture *tex)
{
  if (acquired_.remove(tex)) {
    pool_.append({tex, 0});
  }
}

void WebGPUTexturePool::reset(bool force_free)
{
  for (int64_t i = pool_.size() - 1; i >= 0; i--) {
    pool_[i].unused_cycles++;
    if (force_free || pool_[i].unused_cycles > max_unused_cycles_) {
      delete pool_[i].texture;
      pool_.remove_and_reorder(i);
    }
  }
}

}  // namespace blender::gpu
