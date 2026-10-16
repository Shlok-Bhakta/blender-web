/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU uniform buffer (UBO). Like the storage buffer: CPU shadow + a real
 * WGPUBuffer (Uniform usage) created lazily once a device exists. Device-less
 * headless mode keeps only the shadow.
 */

#pragma once

#include "gpu_uniform_buffer_private.hh"

#include <webgpu/webgpu.h>

namespace blender::gpu {

class WebGPUContext;

class WebGPUUniformBuf : public UniformBuf {
 private:
  WGPUBuffer buffer_ = nullptr;
  size_t alloc_size_ = 0;
  /* CPU shadow updated while no GPU context was active; upload on next bind. */
  bool dirty_ = false;
  /* Once bound as SSBO the GPU may write it — the unchanged-data early-out in
   * update() is then unsafe (CPU shadow can't prove the GPU copy is current). */
  bool ever_bound_as_ssbo_ = false;
  int slot_ = -1;

  void ensure_buffer();
  /* Swap in a fresh WGPUBuffer when updating while a pass records (see impl). */
  bool cow_if_pass_open(WebGPUContext *ctx);

 public:
  WebGPUUniformBuf(size_t size, const char *name);
  ~WebGPUUniformBuf() override;

  void update(const void *data) override;
  void clear_to_zero() override;
  void bind(int slot) override;
  void bind_as_ssbo(int slot) override;
  void unbind() override;

  WGPUBuffer buffer()
  {
    ensure_buffer();
    return buffer_;
  }
};

}  // namespace blender::gpu
