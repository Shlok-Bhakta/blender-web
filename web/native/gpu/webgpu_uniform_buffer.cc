/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "webgpu_context.hh"
#include "webgpu_uniform_buffer.hh"

namespace blender::gpu {

static WebGPUContext *ctx_get()
{
  return static_cast<WebGPUContext *>(Context::get());
}

WebGPUUniformBuf::WebGPUUniformBuf(size_t size, const char *name) : UniformBuf(size, name)
{
  alloc_size_ = (size + 15) & ~size_t(15); /* WebGPU UBOs align to 16 bytes. */
}

WebGPUUniformBuf::~WebGPUUniformBuf()
{
  if (WebGPUContext *ctx = static_cast<WebGPUContext *>(Context::get())) {
    ctx->scrub_buffer(buffer_);
  }
  if (buffer_) {
    wgpuBufferRelease(buffer_);
  }
  MEM_SAFE_DELETE_VOID(data_);
}

void WebGPUUniformBuf::ensure_buffer()
{
  if (data_ == nullptr) {
    data_ = MEM_new_zeroed(alloc_size_, "WebGPUUniformBuf");
  }
  if (buffer_ != nullptr) {
    return;
  }
  WebGPUContext *ctx = ctx_get();
  if (ctx == nullptr || ctx->device() == nullptr) {
    return;
  }
  WGPUBufferDescriptor desc = {};
  desc.size = alloc_size_;
  /* Include Storage usage too: Blender binds some UniformBuf objects as SSBOs
   * (bind_as_ssbo), and the matching WGSL binding is then a storage buffer. A
   * buffer may carry both Uniform and Storage usage. */
  desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  desc.mappedAtCreation = false;
  buffer_ = wgpuDeviceCreateBuffer(ctx->device(), &desc);
  if (buffer_) {
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
  }
}

/* Copy-on-write when a pass is recording: a queued write to the live buffer
 * would retroactively change what the already-recorded draws read (their bind
 * groups reference it), and a pass flush here means one full submit per widget
 * draw (the UI updates a UBO per widget — pathological). A fresh buffer
 * isolates old draws (they hold refs via their bind groups). */
bool WebGPUUniformBuf::cow_if_pass_open(WebGPUContext *ctx)
{
  if (!ctx->pass_open() || buffer_ == nullptr) {
    return false;
  }
  WGPUBufferDescriptor desc = {};
  desc.size = alloc_size_;
  desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  WGPUBuffer fresh = wgpuDeviceCreateBuffer(ctx->device(), &desc);
  if (fresh == nullptr) {
    return false;
  }
  ctx->rebind_buffer(buffer_, fresh);
  wgpuBufferRelease(buffer_);
  buffer_ = fresh;
  return true;
}

void WebGPUUniformBuf::update(const void *data)
{
  ensure_buffer();
  if (data && data_) {
    /* Unchanged (widgets re-upload identical params every draw): the GPU copy
     * is already current — skip the write AND the copy-on-write churn. Only
     * safe while the GPU never writes this buffer (bound_as_ssbo_ tracks it). */
    if (!dirty_ && !ever_bound_as_ssbo_ && memcmp(data_, data, size_in_bytes_) == 0) {
      return;
    }
    memcpy(data_, data, size_in_bytes_);
  }
  WebGPUContext *ctx = ctx_get();
  if (buffer_ && ctx && ctx->queue()) {
    /* Copy-on-write when a pass is recording: a queued write to the live buffer
     * would retroactively change what the already-recorded draws read (their
     * bind groups reference it), and a pass flush here means one full submit
     * per widget draw (the UI updates a UBO per widget — pathological). A fresh
     * buffer isolates old draws (they hold refs via their bind groups). */
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
    dirty_ = false;
  }
  else {
    /* No context/device right now (e.g. update during scene sync): remember to
     * upload at the next bind — otherwise the GPU copy silently stays stale. */
    dirty_ = true;
  }
}

void WebGPUUniformBuf::clear_to_zero()
{
  ensure_buffer();
  if (data_) {
    memset(data_, 0, alloc_size_);
  }
  WebGPUContext *ctx = ctx_get();
  if (buffer_ && ctx && ctx->queue()) {
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
  }
}

void WebGPUUniformBuf::bind(int slot)
{
  slot_ = slot;
  ensure_buffer();
  WebGPUContext *ctx = ctx_get();
  if (dirty_ && buffer_ && data_ && ctx && ctx->queue()) {
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
    dirty_ = false;
  }
  if (ctx) {
    ctx->bind_ubo(slot, buffer_);
  }
}

void WebGPUUniformBuf::bind_as_ssbo(int slot)
{
  ever_bound_as_ssbo_ = true;
  slot_ = slot;
  ensure_buffer();
  WebGPUContext *ctx = ctx_get();
  if (dirty_ && buffer_ && data_ && ctx && ctx->queue()) {
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
    dirty_ = false;
  }
  if (ctx) {
    ctx->bind_ssbo(slot, buffer_);
  }
}

void WebGPUUniformBuf::unbind()
{
  WebGPUContext *ctx = ctx_get();
  if (ctx && slot_ >= 0) {
    ctx->unbind_ubo(slot_);
  }
  slot_ = -1;
}

}  // namespace blender::gpu
