/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "webgpu_shader_interface.hh"

#include "MEM_guardedalloc.h"

#include "BLI_vector.hh"

#include <cstdio>

namespace blender::gpu {

using namespace blender::gpu::shader;

/* Flat bind-group-0 binding index for a resource: its position in the
 * concatenation pass ++ batch ++ geometry. Identical iteration order to
 * WebGPUShader::resources_declare, so WGSL `@binding(N)` matches. The Resource
 * references passed here come straight from `info.*_resources_`, so address
 * comparison is stable. */
int WebGPUShaderInterface::resource_binding(const shader::ShaderCreateInfo &info,
                                            const shader::ShaderCreateInfo::Resource &res)
{
  int binding = 0;
  for (const ShaderCreateInfo::Resource &r : info.pass_resources_) {
    if (&r == &res) {
      return binding;
    }
    binding++;
  }
  for (const ShaderCreateInfo::Resource &r : info.batch_resources_) {
    if (&r == &res) {
      return binding;
    }
    binding++;
  }
  for (const ShaderCreateInfo::Resource &r : info.geometry_resources_) {
    if (&r == &res) {
      return binding;
    }
    binding++;
  }
  return -1;
}

/* std140 alignment + size (bytes) for a push-constant member of the given type.
 * Arrays use a 16-byte element stride (std140). Covers the types EEVEE uses. */
static void std140_layout(shader::Type type, int array_size, uint32_t &align, uint32_t &size)
{
  using shader::Type;
  uint32_t base_align = 4, base_size = 4;
  switch (type) {
    case Type::float_t:
    case Type::int_t:
    case Type::uint_t:
    case Type::bool_t:
      base_align = 4;
      base_size = 4;
      break;
    case Type::float2_t:
    case Type::int2_t:
    case Type::uint2_t:
      base_align = 8;
      base_size = 8;
      break;
    case Type::float3_t:
    case Type::int3_t:
    case Type::uint3_t:
      base_align = 16;
      base_size = 12;
      break;
    case Type::float4_t:
    case Type::int4_t:
    case Type::uint4_t:
      base_align = 16;
      base_size = 16;
      break;
    case Type::float3x3_t:
      base_align = 16;
      base_size = 48;
      break;
    case Type::float4x4_t:
      base_align = 16;
      base_size = 64;
      break;
    default:
      base_align = 16;
      base_size = 16;
      break;
  }
  if (array_size > 0) {
    /* std140: array element stride is rounded up to 16. */
    const uint32_t stride = (base_size + 15u) & ~15u;
    align = 16;
    size = stride * uint32_t(array_size);
  }
  else {
    align = base_align;
    size = base_size;
  }
}

void WebGPUShaderInterface::init(const shader::ShaderCreateInfo &info)
{
  /* --- counts (flat array order: Attributes, Ubos, Uniforms, SSBOs, Constants) --- */
  attr_len_ = info.vertex_inputs_.size();
  ubo_len_ = 0;
  /* +subpass inputs: each gets a hidden emulation sampler `gpu_subpass_img_<i>`
   * (see fragment_interface_declare) resolved at texture slot <i>. */
  uniform_len_ = info.push_constants_.size() + info.subpass_inputs_.size();
  ssbo_len_ = 0;
  constant_len_ = info.specialization_constants_.size();

  Vector<const ShaderCreateInfo::Resource *> all_resources;
  for (const ShaderCreateInfo::Resource &res : info.pass_resources_) {
    all_resources.append(&res);
  }
  for (const ShaderCreateInfo::Resource &res : info.batch_resources_) {
    all_resources.append(&res);
  }
  for (const ShaderCreateInfo::Resource &res : info.geometry_resources_) {
    all_resources.append(&res);
  }

  for (const ShaderCreateInfo::Resource *res : all_resources) {
    switch (res->bind_type) {
      case ShaderCreateInfo::Resource::BindType::IMAGE:
      case ShaderCreateInfo::Resource::BindType::SAMPLER:
        uniform_len_++;
        break;
      case ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER:
        ubo_len_++;
        break;
      case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
        ssbo_len_++;
        break;
    }
  }

  size_t names_size = info.interface_names_size_;
  names_size += info.subpass_inputs_.size() * 24; /* "gpu_subpass_img_<i>" + NUL */
  const int input_tot_len = attr_len_ + ubo_len_ + uniform_len_ + ssbo_len_ + constant_len_;
  inputs_ = MEM_new_array_zeroed<ShaderInput>(input_tot_len, __func__);
  name_buffer_ = MEM_new_array_uninitialized<char>(names_size, "name_buffer");
  uint32_t name_offset = 0;
  ShaderInput *input = inputs_;

  /* --- Attributes --- */
  for (const ShaderCreateInfo::VertIn &attr : info.vertex_inputs_) {
    copy_input_name(input, attr.name, name_buffer_, name_offset);
    input->location = input->binding = attr.index;
    if (input->location != -1) {
      enabled_attr_mask_ |= (1 << input->location);
      attr_types_[input->location] = uint8_t(attr.type);
    }
    input++;
  }

  /* binding = app-facing slot (what GPU_*_bind / GPU_shader_get_*_binding use);
   * location = the flat WGSL `@binding(N)` Tint emits (used to build bind groups).
   * The two differ because WebGPU packs all of group 0 into one unique binding
   * space while Blender's slots are per-resource-type. */

  /* --- Uniform blocks (UBOs) --- */
  for (const ShaderCreateInfo::Resource *res : all_resources) {
    if (res->bind_type == ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER) {
      copy_input_name(input, res->uniformbuf.name, name_buffer_, name_offset);
      input->binding = res->slot;
      input->location = resource_binding(info, *res);
      enabled_ubo_mask_ |= (1 << res->slot);
      input++;
    }
  }

  /* --- Samplers + Images (in `uniform` sub-array, looked up by binding) --- */
  for (const ShaderCreateInfo::Resource *res : all_resources) {
    if (res->bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER) {
      copy_input_name(input, res->sampler.name, name_buffer_, name_offset);
      input->binding = res->slot;
      input->location = resource_binding(info, *res);
      enabled_tex_mask_ |= (1ull << res->slot);
      input++;
    }
    else if (res->bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
      copy_input_name(input, res->image.name, name_buffer_, name_offset);
      input->binding = res->slot;
      input->location = resource_binding(info, *res);
      enabled_ima_mask_ |= (1 << res->slot);
      input++;
    }
  }
  /* Sub-pass input emulation samplers (fragment-only). */
  for (const ShaderCreateInfo::SubpassIn &sp : info.subpass_inputs_) {
    char sp_name[24];
    snprintf(sp_name, sizeof(sp_name), "gpu_subpass_img_%d", sp.index);
    copy_input_name(input, sp_name, name_buffer_, name_offset);
    input->binding = sp.index;
    input->location = 200 + sp.index;
    enabled_tex_mask_ |= (1ull << sp.index);
    input++;
  }
  set_image_formats_from_info(info);

  /* --- Push constants. location = std140 BYTE OFFSET into the `constants` block
   * (matching the GLSL `layout(std140) uniform constants {...}` we emit), so
   * uniform_float/int can write straight into the push-constant buffer shadow. --- */
  uint32_t pc_offset = 0;
  for (const ShaderCreateInfo::PushConst &pc : info.push_constants_) {
    uint32_t align, size;
    std140_layout(pc.type, pc.array_size, align, size);
    pc_offset = (pc_offset + align - 1) & ~(align - 1);
    copy_input_name(input, pc.name, name_buffer_, name_offset);
    input->location = int32_t(pc_offset);
    input->binding = -1;
    input++;
    pc_offset += size;
  }

  /* --- Storage buffers --- */
  for (const ShaderCreateInfo::Resource *res : all_resources) {
    if (res->bind_type == ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER) {
      copy_input_name(input, res->storagebuf.name, name_buffer_, name_offset);
      input->binding = res->slot;
      /* GPU_shader_get_ssbo_binding returns ->location (unlike UBO/samplers,
       * which return ->binding): DRW's by-NAME ssbo binds use it as the bind
       * slot, so it must be the Blender slot — NOT the flat WGSL index — or
       * name-bound buffers land in the wrong table entry (this broke light
       * culling and the shadow pipeline: wrong/aliased buffers per dispatch). */
      input->location = res->slot;
      enabled_ssbo_mask_ |= (1 << res->slot);
      input++;
    }
  }

  /* --- Specialization constants --- */
  int constant_id = 0;
  for (const SpecializationConstant &constant : info.specialization_constants_) {
    copy_input_name(input, constant.name, name_buffer_, name_offset);
    input->location = constant_id++;
    input++;
  }

  sort_inputs();

  populate_builtins(info);
}

void WebGPUShaderInterface::populate_builtins(const shader::ShaderCreateInfo & /*info*/)
{
  for (int32_t u_int = 0; u_int < GPU_NUM_UNIFORMS; u_int++) {
    GPUUniformBuiltin u = static_cast<GPUUniformBuiltin>(u_int);
    const char *name = builtin_uniform_name(u);
    const ShaderInput *uni = name ? this->uniform_get(name) : nullptr;
    builtins_[u] = (uni != nullptr) ? uni->location : -1;
  }
  for (int32_t u_int = 0; u_int < GPU_NUM_UNIFORM_BLOCKS; u_int++) {
    GPUUniformBlockBuiltin u = static_cast<GPUUniformBlockBuiltin>(u_int);
    const char *name = builtin_uniform_block_name(u);
    const ShaderInput *block = name ? this->ubo_get(name) : nullptr;
    builtin_blocks_[u] = (block != nullptr) ? block->binding : -1;
  }
}

}  // namespace blender::gpu
