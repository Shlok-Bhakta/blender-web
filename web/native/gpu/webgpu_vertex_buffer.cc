/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "webgpu_context.hh"
#include "webgpu_vertex_buffer.hh"

namespace blender::gpu {

WebGPUVertexBuffer::~WebGPUVertexBuffer()
{
  if (WebGPUContext *ctx = static_cast<WebGPUContext *>(Context::get())) {
    ctx->scrub_buffer(buffer_);
  }
  if (buffer_) {
    wgpuBufferRelease(buffer_);
  }
  MEM_SAFE_DELETE(data_);
}

void WebGPUVertexBuffer::acquire_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  MEM_SAFE_DELETE(data_);
  data_ = MEM_new_array_uninitialized<uchar>(this->size_alloc_get(), __func__);
}

void WebGPUVertexBuffer::resize_data()
{
  if (usage_ == GPU_USAGE_DEVICE_ONLY) {
    return;
  }
  data_ = static_cast<uchar *>(
      MEM_realloc_uninitialized(data_, sizeof(uchar) * this->size_alloc_get()));
}

void WebGPUVertexBuffer::release_data()
{
  if (buffer_) {
    wgpuBufferRelease(buffer_);
    buffer_ = nullptr;
    buffer_size_ = 0;
  }
  MEM_SAFE_DELETE(data_);
}

void WebGPUVertexBuffer::ensure_buffer()
{
  const size_t needed = this->size_used_get();
  if (buffer_ && buffer_size_ >= needed) {
    return;
  }
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx == nullptr || ctx->device() == nullptr || needed == 0) {
    return;
  }
  if (buffer_) {
    wgpuBufferRelease(buffer_);
    buffer_ = nullptr;
  }
  /* WGPU requires buffer size be a multiple of 4. */
  buffer_size_ = (needed + 3) & ~size_t(3);
  WGPUBufferDescriptor desc = {};
  desc.size = buffer_size_;
  desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage |
               WGPUBufferUsage_CopySrc;
  buffer_ = wgpuDeviceCreateBuffer(ctx->device(), &desc);
}

/* Copy-on-write: updating a live buffer while a pass is recording would either
 * retroactively change what earlier draws read (queue writes execute before the
 * pass's submit) or force a full submit per update (pathological — BLF text
 * updates a VBO per run). A fresh buffer isolates the recorded draws: the pass
 * encoder holds refs to the old one. Requires the CPU shadow (full contents). */
bool WebGPUVertexBuffer::cow_if_pass_open(WebGPUContext *ctx)
{
  if (!ctx->pass_open() || buffer_ == nullptr || data_ == nullptr) {
    return false;
  }
  WGPUBufferDescriptor desc = {};
  desc.size = buffer_size_;
  desc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst | WGPUBufferUsage_Storage |
               WGPUBufferUsage_CopySrc;
  WGPUBuffer fresh = wgpuDeviceCreateBuffer(ctx->device(), &desc);
  if (fresh == nullptr) {
    return false;
  }
  ctx->rebind_buffer(buffer_, fresh); /* VBOs can be bound as SSBOs. */
  wgpuBufferRelease(buffer_);
  buffer_ = fresh;
  return true;
}

void WebGPUVertexBuffer::upload_data()
{
  ensure_buffer();
  WebGPUContext *ctx = WebGPUContext::get();
  if (buffer_ && ctx && ctx->queue() && data_) {
    const size_t used = this->size_used_get();
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, (used + 3) & ~size_t(3));
  }
}

void WebGPUVertexBuffer::update_sub(uint start, uint len, const void *data)
{
  if (data_) {
    memcpy(data_ + start, data, len);
  }
  ensure_buffer();
  WebGPUContext *ctx = WebGPUContext::get();
  if (buffer_ && ctx && ctx->queue()) {
    if (cow_if_pass_open(ctx)) {
      /* Fresh buffer: upload the FULL shadow (it includes this sub-range). */
      wgpuQueueWriteBuffer(
          ctx->queue(), buffer_, 0, data_, (this->size_used_get() + 3) & ~size_t(3));
    }
    else {
      /* No shadow to rebuild from: preserve GL ordering the expensive way. */
      ctx->flush_if_pass_open("vbo_sub");
      wgpuQueueWriteBuffer(ctx->queue(), buffer_, start, data, (len + 3) & ~size_t(3));
    }
  }
}

void WebGPUVertexBuffer::bind_as_ssbo(uint binding)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx) {
    ctx->bind_ssbo(int(binding), wgpu_buffer());
  }
}

void WebGPUVertexBuffer::read(void *data) const
{
  /* GPU->host readback needs an async map (no JSPI). Return the CPU shadow. */
  if (data_ && data) {
    memcpy(data, data_, this->size_used_get());
  }
}

}  // namespace blender::gpu
