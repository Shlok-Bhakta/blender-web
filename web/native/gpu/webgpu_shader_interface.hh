/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * WebGPU shader interface. Built directly from the ShaderCreateInfo (like the
 * Vulkan backend) rather than by GL-style runtime introspection. Assigns a
 * single flat, unique binding number per resource within bind group 0 — the
 * SAME sequential scheme WebGPUShader::resources_declare uses for the GLSL
 * `layout(binding=)` qualifiers, so the WGSL `@group(0) @binding(N)` produced by
 * Tint matches the bindings the interface reports back to the GPU module.
 */

#pragma once

#include "gpu_shader_interface.hh"

namespace blender::gpu {

class WebGPUShaderInterface : public ShaderInterface {
 public:
  WebGPUShaderInterface() = default;
  ~WebGPUShaderInterface() override = default;

  void init(const shader::ShaderCreateInfo &info);

  /* Canonical bind-group-0 binding for a resource, matching
   * WebGPUShader::resources_declare. Static so the GLSL generator can share it. */
  static int resource_binding(const shader::ShaderCreateInfo &info,
                              const shader::ShaderCreateInfo::Resource &res);

 private:
  void populate_builtins(const shader::ShaderCreateInfo &info);
};

}  // namespace blender::gpu
