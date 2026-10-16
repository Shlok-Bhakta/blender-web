/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU shader. SCAFFOLD STAGE: implements the gpu::Shader interface so
 * shader_alloc() returns a real object (the previous nullptr stub crashed the
 * compiler worker). The GLSL→WGSL compiler is not wired yet, so finalize()
 * currently fails gracefully (ShaderCompiler::compile then deletes the shader
 * and returns null, which the caller treats as a failed compile). This lets the
 * render pipeline proceed far enough to enumerate the shaders EEVEE needs and
 * the exact GLSL it emits — the input to the real WGSL backend.
 */

#pragma once

#include <string>
#include <vector>

#include <webgpu/webgpu.h>

#include "gpu_shader_private.hh"

namespace blender::gpu {

/* A resource binding parsed from the generated WGSL. WGSL is the ground truth for
 * which @binding the pipeline expects (Tint splits combined samplers + resolves
 * conflicts, so binding numbers don't match the source GLSL). `res_name` is the
 * Blender resource name recovered from the declaration (buffer struct type, or
 * texture/sampler var name minus the _image/_sampler suffix) and is matched
 * against the ShaderInterface to find the app binding slot. */
struct WgslResourceBinding {
  uint32_t binding;
  enum Kind { UBO, SSBO, TEXTURE, SAMPLER, STORAGE_TEXTURE, PUSH_CONST } kind;
  std::string res_name;
  /* Type detail parsed from the WGSL declaration, used to build an explicit
   * bind-group layout (auto-layout strips declared-but-unused bindings, which
   * then mismatches our complete entry set). */
  WGPUBufferBindingType buffer_type = WGPUBufferBindingType_Uniform;
  WGPUTextureSampleType tex_sample = WGPUTextureSampleType_Float;
  WGPUTextureViewDimension view_dim = WGPUTextureViewDimension_2D;
  WGPUStorageTextureAccess storage_access = WGPUStorageTextureAccess_WriteOnly;
  WGPUTextureFormat storage_format = WGPUTextureFormat_RGBA8Unorm;
  WGPUSamplerBindingType sampler_type = WGPUSamplerBindingType_Filtering;
  bool writable = false; /* read_write storage buffer / storage texture */
};

/* One @location the vertex entry point consumes (parsed from the WGSL). WebGPU
 * validation requires EVERY shader input location to be fed by the pipeline's
 * VertexState (GL fetches a default for unbound attributes), so draws pad the
 * locations the batch doesn't supply with a null buffer. */
struct WgslVertexInput {
  uint32_t location;
  WGPUVertexFormat format;
};

class WebGPUShader : public Shader {
 private:
  std::string vertex_src_;
  std::string fragment_src_;
  std::string geometry_src_;
  std::string compute_src_;

  /* Translated WGSL per stage (filled by finalize()). */
  std::string vertex_wgsl_;
  std::string fragment_wgsl_;
  std::string compute_wgsl_;

  /* Created from WGSL when a real WebGPU device is present (browser harness).
   * In degraded headless mode (no device) these stay null but the WGSL is still
   * produced + the interface built, so shader compilation reports success. */
  WGPUShaderModule vertex_module_ = nullptr;
  WGPUShaderModule fragment_module_ = nullptr;
  WGPUShaderModule compute_module_ = nullptr;

  bool valid_ = false;
  /* GLSL→WGSL translation is deferred to first use (see ensure_translated). */
  bool translate_attempted_ = false;
  /* Point-sprite expansion eligibility (set from the pre-patch GLSL). */
  bool writes_point_size_ = false;
  bool uses_vertex_id_ = false;
  bool uses_point_coord_ = false;

  /* Bindings parsed from the generated WGSL (union of vert+frag for graphics). */
  std::vector<WgslResourceBinding> render_bindings_;
  std::vector<WgslResourceBinding> compute_bindings_;
  /* Bit i set = fragment entry point writes @location(i). */
  uint32_t frag_output_mask_ = 0;
  /* Vertex input locations the vertex entry point consumes. */
  std::vector<WgslVertexInput> vertex_inputs_;

  /* Push-constant block backed by a uniform-buffer SLICE from the context's
   * transient arena (WGSL exposes push constants as a `constants` uniform
   * block). uniform_float/int write the CPU shadow; on dirty the shadow is
   * uploaded into a FRESH arena slice — earlier recorded draws keep referencing
   * their own slices, so no pass flush/submit is needed (a persistent per-shader
   * buffer would need a full submit per update to preserve GL ordering). The
   * cached slice buffer holds a ref (the arena rolls over). */
  WGPUBuffer pc_slice_buf_ = nullptr;
  uint64_t pc_slice_off_ = 0;
  uint32_t pc_slice_epoch_ = 0;
  uint8_t *push_const_data_ = nullptr;
  size_t push_const_size_ = 0;
  bool push_const_dirty_ = false;

  /* Specialization-constant values from the most recent bind() (defaults until
   * a pass calls specialize_constant). Index i corresponds to constant_id=i in
   * the generated GLSL preamble. */
  shader::SpecializationConstants spec_state_;
  /* Stable storage for WGPUConstantEntry keys ("0", "1", ...). */
  std::vector<std::string> spec_keys_;

 public:
  WebGPUShader(const char *name);
  ~WebGPUShader() override;

  const std::vector<WgslResourceBinding> &render_bindings() const { return render_bindings_; }
  const std::vector<WgslResourceBinding> &compute_bindings() const { return compute_bindings_; }
  uint32_t fragment_output_mask() const { return frag_output_mask_; }
  const std::vector<WgslVertexInput> &vertex_inputs() const { return vertex_inputs_; }
  /* Point draws of this shader can expand to per-instance quads. */
  /* gl_VertexID remaps to the instance index under expansion (see the patch
   * define), so vertex-id-using point shaders expand fine. */
  bool can_expand_points() const
  {
    return writes_point_size_;
  }
  /* Upload-on-dirty push-constant slice; false when the shader has none. */
  bool push_const_slice(WGPUBuffer &r_buf, uint64_t &r_off, uint64_t &r_size);
  /* Run the deferred GLSL→SPIR-V→WGSL translation + WGSL metadata parsing.
   * Called at first draw/dispatch (module accessors call it too). */
  void ensure_translated();

  const std::string &vertex_wgsl() const { return vertex_wgsl_; }
  const std::string &fragment_wgsl() const { return fragment_wgsl_; }
  const std::string &compute_wgsl() const { return compute_wgsl_; }
  /* Lazy: create the WGPUShaderModule on first use from the retained WGSL. Under
   * PROXY_TO_PTHREAD the shader finalizes on a thread without the device context,
   * so we defer module creation to draw/dispatch time (on the render worker where
   * the device is valid). Non-const for that reason. */
  WGPUShaderModule vertex_module();
  WGPUShaderModule fragment_module();
  WGPUShaderModule compute_module();
  bool is_valid() const { return valid_; }

  void init(const shader::ShaderCreateInfo &info, bool is_codegen_only) override;
  const shader::ShaderCreateInfo &patch_create_info(
      const shader::ShaderCreateInfo &original_info) override
  {
    return original_info;
  }

  void vertex_shader_from_glsl(const shader::ShaderCreateInfo &info,
                               MutableSpan<StringRefNull> sources) override;
  void geometry_shader_from_glsl(const shader::ShaderCreateInfo &info,
                                 MutableSpan<StringRefNull> sources) override;
  void fragment_shader_from_glsl(const shader::ShaderCreateInfo &info,
                                 MutableSpan<StringRefNull> sources) override;
  void compute_shader_from_glsl(const shader::ShaderCreateInfo &info,
                                MutableSpan<StringRefNull> sources) override;
  bool finalize(const shader::ShaderCreateInfo *info = nullptr) override;
  void warm_cache(int /*limit*/) override {}

  void bind(const shader::SpecializationConstants *constants_state) override
  {
    if (constants_state != nullptr) {
      spec_state_ = *constants_state;
    }
    else if (constants) {
      spec_state_ = *constants;
    }
  }
  void unbind() override {}

  /* WGSL `override` specialization: spec constants survive GLSL→SPIR-V→WGSL as
   * `@id(N) override` declarations; the per-submit values (from bind()) are fed
   * to pipeline creation as WGPUConstantEntry overrides. Entries are emitted
   * only for ids actually present in the stage's WGSL — Dawn rejects constant
   * keys that don't match any override (a stage that never uses a constant has
   * no override for it). */
  uint64_t spec_hash() const;
  void spec_entries(const std::string &wgsl, std::vector<WGPUConstantEntry> &out);

  /* location is the std140 byte offset into the push-constant block (assigned by
   * WebGPUShaderInterface). Writes into the CPU shadow + marks it dirty for the
   * next push_const_buffer() upload. */
  void uniform_float(int location, int comp_len, int array_size, const float *data) override;
  void uniform_int(int location, int comp_len, int array_size, const int *data) override;

  /* Resource/interface declarations: inject the Vulkan-GLSL preamble (uniforms,
   * attributes, interface blocks with set/binding/location) ahead of the GLSL
   * body. Implemented in webgpu_shader.cc. */
  std::string resources_declare(const shader::ShaderCreateInfo &info) const override;
  std::string vertex_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string fragment_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string geometry_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string geometry_layout_declare(const shader::ShaderCreateInfo &info) const override;
  std::string compute_layout_declare(const shader::ShaderCreateInfo &info) const override;
};

}  // namespace blender::gpu
