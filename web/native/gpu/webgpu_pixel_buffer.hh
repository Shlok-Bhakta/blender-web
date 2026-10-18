/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "gpu_texture_private.hh"
#include <cstdint>
#include <vector>

namespace blender::gpu {

/* CPU renderers write their display pixels here. Queue::WriteTexture copies
 * these bytes at submission, so the same storage can then be mapped again. */
class WebGPUPixelBuffer : public PixelBuffer {
  std::vector<uint8_t> bytes_;

 public:
  explicit WebGPUPixelBuffer(size_t size) : PixelBuffer(size), bytes_(size) {}
  void *map() override { return bytes_.data(); }
  void unmap() override {}
  size_t get_size() override { return size_; }
  GPUPixelBufferNativeHandle get_native_handle() override
  {
    /* Browser WebGPU does not expose CUDA/HIP/native memory handles. */
    return {};
  }
};

}  // namespace blender::gpu
