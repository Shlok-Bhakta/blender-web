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
  puts("{\"shaderRecoveryUnwinds\":true,\"translationAfterRecovery\":true}");
}
