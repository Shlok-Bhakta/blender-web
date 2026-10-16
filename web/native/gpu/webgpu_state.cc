/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "webgpu_context.hh"
#include "webgpu_state.hh"
#include "webgpu_texture.hh"

namespace blender::gpu {

void WebGPUStateManager::texture_bind(Texture *tex, GPUSamplerState sampler, int unit)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx) {
    ctx->bind_texture(unit, static_cast<WebGPUTexture *>(tex));
    ctx->bind_texture_state(unit, sampler);
  }
}

void WebGPUStateManager::image_bind(Texture *tex, int unit)
{
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx) {
    ctx->bind_image(unit, static_cast<WebGPUTexture *>(tex));
  }
}

}  // namespace blender::gpu
