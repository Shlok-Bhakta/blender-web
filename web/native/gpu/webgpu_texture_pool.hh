/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU transient-texture pool. Simple implementation: reuse a compatible free
 * texture if available, otherwise allocate a fresh one. Released textures return
 * to the free list; reset() ages and frees long-unused entries. No sub-resource
 * aliasing (unlike the Vulkan pool) — correctness first.
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "gpu_texture_pool_private.hh"

namespace blender::gpu {

class WebGPUTexturePool : public TexturePoolBase {
 private:
  static constexpr int max_unused_cycles_ = 8;

  struct FreeTexture {
    Texture *texture = nullptr;
    int unused_cycles = 0;
  };

  Vector<FreeTexture> pool_;
  /* In-use textures → remaining user count. */
  Map<Texture *, int> acquired_;

 protected:
  Texture *acquire_texture_impl(int3 extent,
                                int mip_len,
                                GPUTextureType type,
                                TextureFormat format,
                                eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL,
                                const char *name = nullptr) override;

 public:
  ~WebGPUTexturePool() override;

  void release_texture(Texture *tex) override;
  void reset(bool force_free = false) override;
  void offset_users_count(Texture *tex, int offset) override;
};

}  // namespace blender::gpu
