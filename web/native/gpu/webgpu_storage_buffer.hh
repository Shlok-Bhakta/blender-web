/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU storage buffer (SSBO equivalent). Keeps a CPU shadow copy so the
 * object is usable even before a WGPUDevice exists (e.g. headless `node -b`,
 * where emscripten_webgpu_get_device() returns null); real GPU buffers are
 * created/updated lazily once a device is available. This lets the draw
 * manager / EEVEE drive the backend and surfaces the next required object,
 * while remaining correct in the browser where the device is present.
 */

#pragma once

#include "gpu_storage_buffer_private.hh"

#include <webgpu/webgpu.h>

namespace blender::gpu {

class WebGPUContext;

class WebGPUStorageBuf : public StorageBuf {
 private:
  /** Lazily-created GPU buffer; null until a device is available. */
  WGPUBuffer buffer_ = nullptr;
  GPUUsageType usage_ = GPUUsageType(-1);
  int slot_ = -1;
  /** Allocation size, rounded up to 4 bytes as WebGPU requires. */
  size_t alloc_size_ = 0;
  /* CPU shadow updated while no GPU context was active; upload on next bind. */
  bool dirty_ = false;

  /** Ensure the CPU shadow (data_) exists; create GPU buffer if a device is up. */
  void ensure_buffer();
  /* Swap in a fresh WGPUBuffer for whole-buffer rewrites mid-pass (see impl). */
  bool cow_if_pass_open(WebGPUContext *ctx);

 public:
  WebGPUStorageBuf(size_t size, GPUUsageType usage, const char *name);
  ~WebGPUStorageBuf() override;

  void update(const void *data) override;
  void bind(int slot) override;
  void unbind() override;
  void clear(uint32_t clear_value) override;
  void copy_sub(VertBuf *src, uint dst_offset, uint src_offset, uint copy_size) override;
  void read(void *data) override;
  void async_flush_to_host() override;
  void sync_as_indirect_buffer() override;

  WGPUBuffer buffer()
  {
    ensure_buffer();
    return buffer_;
  }
};

}  // namespace blender::gpu
