/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cstdio>
#include <vector>
#include "src/tint/lang/core/ir/module.h"
#include <shaderc/shaderc.hpp>
#include "src/tint/utils/ice/ice.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"
#include "../gpu/shader_compat.hh"

struct UnwindProof {
  bool &destroyed;
  ~UnwindProof() { destroyed = true; }
};
int main()
{
  bool destroyed = false, caught = false;
  try {
    UnwindProof proof{destroyed};
    tint::internal_compiler_error_recovery = true;
    TINT_ICE() << "intentional recovery regression check";
  } catch (const tint::RecoverableCompilerError &) {
    caught = true;
  }
  assert(caught && destroyed && !tint::internal_compiler_error_recovery);

  std::string source = R"(#version 450
#define SRT_CONSTANT_use_clipping 0
#if defined(SRT_CONSTANT_use_clipping) ? (SRT_CONSTANT_use_clipping == 1) : \
 defined(USE_WORLD_CLIP_PLANES)
#error unexpected clipping
#endif
#define float4 vec4
#define NAN_FLT uintBitsToFloat(0x7fc00000u)
void main() { gl_Position = float4(NAN_FLT); }
)";
  std::string adapted;
  assert(webgpu_adapt_draw_command(source, "regression", adapted));
  shaderc::Compiler compiler;
  auto spv = compiler.CompileGlslToSpv(adapted, shaderc_vertex_shader, "regression");
  assert(spv.GetCompilationStatus() == shaderc_compilation_status_success);
  std::vector<uint32_t> words(spv.cbegin(), spv.cend());
  auto ir = tint::spirv::reader::ReadIR(words, {});
  assert(ir == tint::Success);
  auto result = tint::wgsl::writer::WgslFromIR(ir.Get(), {});
  assert(result == tint::Success && !result.Get().wgsl.empty());

  const std::string volume = R"(#version 450
layout(set=0,binding=0) uniform sampler1D flame_color;
layout(location=0) out vec4 color;
vec4 wgpu_tex1d(sampler2D s, float co) { return texture(s, vec2(co,0.5)); }
#define FLAME_COLOR(table) flame_color
#define VOLUME_COLOR(table,co) texture(FLAME_COLOR(table),co)
void main() { color = VOLUME_COLOR(smoke,0.5); }
)";
  shaderc::CompileOptions compile_options;
  auto preprocessed = compiler.PreprocessGlsl(volume, shaderc_fragment_shader, "volume-macro", compile_options);
  assert(preprocessed.GetCompilationStatus() == shaderc_compilation_status_success);
  std::string expanded(preprocessed.cbegin(), preprocessed.cend());
  webgpu_promote_1d_samplers(expanded);
  auto volume_spv = compiler.CompileGlslToSpv(expanded, shaderc_fragment_shader, "volume-macro");
  if(volume_spv.GetCompilationStatus() != shaderc_compilation_status_success) {
    std::fprintf(stderr,"%s",volume_spv.GetErrorMessage().c_str());
    return 1;
  }
  tint::spirv::reader::Options options;
  options.sampler_mappings.insert({tint::BindingPoint{0,0},tint::BindingPoint{0,128}});
  words.assign(volume_spv.cbegin(), volume_spv.cend());
  auto volume_ir = tint::spirv::reader::ReadIR(words, options);
  assert(volume_ir == tint::Success);
  auto volume_wgsl = tint::wgsl::writer::WgslFromIR(volume_ir.Get(), {});
  assert(volume_wgsl == tint::Success && volume_wgsl.Get().wgsl.find("texture_2d<f32>") != std::string::npos);
  puts("{\"shaderRecoveryUnwinds\":true,\"translationAfterRecovery\":true,\"resourceMacroSampler1D\":true}");
}
