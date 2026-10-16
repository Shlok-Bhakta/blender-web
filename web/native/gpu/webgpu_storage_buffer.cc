/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "webgpu_context.hh"
#include "webgpu_storage_buffer.hh"

namespace blender::gpu {

static WebGPUContext *webgpu_context_get()
{
  return static_cast<WebGPUContext *>(Context::get());
}

WebGPUStorageBuf::WebGPUStorageBuf(size_t size, GPUUsageType usage, const char *name)
    : StorageBuf(size, name)
{
  usage_ = usage;
  /* WebGPU buffer sizes must be a multiple of 4 bytes. */
  alloc_size_ = (size + 3) & ~size_t(3);
}

WebGPUStorageBuf::~WebGPUStorageBuf()
{
  if (WebGPUContext *ctx = static_cast<WebGPUContext *>(Context::get())) {
    ctx->scrub_buffer(buffer_);
  }
  if (buffer_) {
    wgpuBufferRelease(buffer_);
    buffer_ = nullptr;
  }
  MEM_SAFE_DELETE_VOID(data_);
}

void WebGPUStorageBuf::ensure_buffer()
{
  if (data_ == nullptr) {
    data_ = MEM_new_zeroed(alloc_size_, "WebGPUStorageBuf");
  }
  if (buffer_ != nullptr) {
    return;
  }
  WebGPUContext *ctx = webgpu_context_get();
  if (ctx == nullptr || ctx->device() == nullptr) {
    /* No device yet (e.g. headless node). Stay CPU-only; created lazily later. */
    return;
  }
  WGPUBufferDescriptor desc = {};
  desc.size = alloc_size_;
  desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
               WGPUBufferUsage_Indirect | WGPUBufferUsage_Vertex;
  desc.mappedAtCreation = false;
  buffer_ = wgpuDeviceCreateBuffer(ctx->device(), &desc);
  /* Upload whatever the CPU shadow currently holds (fresh buffer — a queued
   * write cannot affect recorded work, no flush needed). */
  if (buffer_) {
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
  }
}

/* Copy-on-write for whole-buffer rewrites while a pass records: the recorded
 * draws keep their refs to the old buffer; a fresh one avoids both the
 * retroactive-write hazard and a full submit per update. Only valid because the
 * caller rewrites the ENTIRE buffer from the CPU shadow (any GPU-written
 * content is overwritten either way). */
bool WebGPUStorageBuf::cow_if_pass_open(WebGPUContext *ctx)
{
  if (!ctx->pass_open() || buffer_ == nullptr) {
    return false;
  }
  WGPUBufferDescriptor desc = {};
  desc.size = alloc_size_;
  desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
               WGPUBufferUsage_Indirect | WGPUBufferUsage_Vertex;
  WGPUBuffer fresh = wgpuDeviceCreateBuffer(ctx->device(), &desc);
  if (fresh == nullptr) {
    return false;
  }
  ctx->rebind_buffer(buffer_, fresh);
  wgpuBufferRelease(buffer_);
  buffer_ = fresh;
  return true;
}

void WebGPUStorageBuf::update(const void *data)
{
  if (size_in_bytes_ > (size_t(1) << 31)) {
    return;
  }
  ensure_buffer();
  if (data && data_) {
    /* NOTE: no unchanged-data early-out here — storage buffers can be
     * GPU-written (compute) after our upload, so the CPU shadow matching the
     * new data does NOT mean the GPU copy does. */
    memcpy(data_, data, size_in_bytes_);
  }
  WebGPUContext *ctx = webgpu_context_get();
  if (buffer_ && ctx && ctx->queue()) {
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
    dirty_ = false;
  }
  else {
    /* No context/device right now: upload at the next bind (else the GPU copy
     * silently stays stale — this froze the per-sample RNG data). */
    dirty_ = true;
  }
}

void WebGPUStorageBuf::bind(int slot)
{
  slot_ = slot;
  ensure_buffer();
  WebGPUContext *ctx = webgpu_context_get();
  if (dirty_ && buffer_ && data_ && ctx && ctx->queue()) {
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
    dirty_ = false;
  }
  /* Record into the context binding table; the bind group is assembled at
   * draw/dispatch time from these + the pipeline's auto layout. */
  if (ctx) {
    ctx->bind_ssbo(slot, buffer_);
  }
}

void WebGPUStorageBuf::unbind()
{
  WebGPUContext *ctx = webgpu_context_get();
  if (ctx && slot_ >= 0) {
    ctx->unbind_ssbo(slot_);
  }
  slot_ = -1;
}

void WebGPUStorageBuf::clear(uint32_t clear_value)
{
  ensure_buffer();
  if (data_) {
    /* Fill the CPU shadow with the repeated 32-bit clear value. */
    uint32_t *words = static_cast<uint32_t *>(data_);
    const size_t word_count = alloc_size_ / sizeof(uint32_t);
    for (size_t i = 0; i < word_count; i++) {
      words[i] = clear_value;
    }
  }
  WebGPUContext *ctx = webgpu_context_get();
  if (buffer_ && ctx && ctx->queue()) {
    cow_if_pass_open(ctx);
    wgpuQueueWriteBuffer(ctx->queue(), buffer_, 0, data_, alloc_size_);
  }
}

void WebGPUStorageBuf::copy_sub(VertBuf * /*src*/,
                                uint /*dst_offset*/,
                                uint /*src_offset*/,
                                uint /*copy_size*/)
{
  /* TODO: GPU-side buffer-to-buffer copy via a command encoder. Requires the
   * VertBuf WebGPU buffer handle. */
}

void WebGPUStorageBuf::read(void *data)
{
  if (data == nullptr) {
    return;
  }
  WebGPUContext *ctx = webgpu_context_get();
  if (ctx != nullptr && ctx->device() != nullptr && buffer_ != nullptr) {
    /* Real GPU readback: flush recorded work, copy into a MapRead staging
     * buffer, block on the map (JSPI / WaitAny — same path as select-id
     * picks). Falls back to the host mirror on map failure. */
    ctx->flush_if_pass_open("ssbo_read");
    const size_t asz = (size_in_bytes_ + 3) & ~size_t(3);
    WGPUBufferDescriptor bd = {};
    bd.label = {"ssbo_read_staging", WGPU_STRLEN};
    bd.size = asz;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer st = wgpuDeviceCreateBuffer(ctx->device(), &bd);
    if (st != nullptr) {
      WGPUCommandEncoder enc = ctx->ensure_encoder();
      if (enc != nullptr) {
        wgpuCommandEncoderCopyBufferToBuffer(enc, buffer_, 0, st, 0, asz);
        ctx->flush_encoder();
        std::vector<uint8_t> tmp(asz);
        if (ctx->map_read_sync(st, asz, tmp.data())) {
          memcpy(data, tmp.data(), size_in_bytes_);
          if (data_) {
            /* Keep the host mirror coherent. */
            memcpy(data_, tmp.data(), size_in_bytes_);
          }
          wgpuBufferRelease(st);
          return;
        }
      }
      wgpuBufferRelease(st);
    }
  }
  /* Fallback: last host-written contents. */
  if (data_) {
    memcpy(data, data_, size_in_bytes_);
  }
}

void WebGPUStorageBuf::async_flush_to_host()
{
  /* No-op until async readback is wired (see read()). */
}

void WebGPUStorageBuf::sync_as_indirect_buffer()
{
  ensure_buffer();
  /* The buffer is already created with WGPUBufferUsage_Indirect. */
}

}  // namespace blender::gpu
