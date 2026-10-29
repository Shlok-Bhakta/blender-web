// SPDX-License-Identifier: GPL-2.0-or-later
// Isolated proof. This is not yet Blender's OSL execution adapter.
#include <llvm/AsmParser/Parser.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <lld/Common/Driver.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

LLD_HAS_DRIVER(wasm)

static void require(bool value, const char *message)
{
  if (!value) {
    std::fprintf(stderr, "LLVM compiler probe: %s\n", message);
    std::exit(1);
  }
}

#include "llvm-shared-check.hh"

static const char *shader_ir = R"IR(
declare float @host_scale(float) #0
define i32 @folded() {
  %answer = add i32 42, 0
  ret i32 %answer
}
define i32 @mix(i32 %x) {
  %a = mul i32 %x, 1664525
  %b = add i32 %a, 1013904223
  %c = lshr i32 %b, 13
  %d = xor i32 %b, %c
  ret i32 %d
}
define i64 @wide(i64 %x) {
  %a = mul i64 %x, 6364136223846793005
  %b = add i64 %a, 1442695040888963407
  ret i64 %b
}
define i32 @increment(ptr %counter, i32 %value) {
  %old = atomicrmw add ptr %counter, i32 %value seq_cst
  ret i32 %old
}
define void @vector4(ptr %out, ptr %in) {
  %x = load <4 x float>, ptr %in, align 16
  %scaled = fmul <4 x float> %x, <float 0.5, float 0.5, float 0.5, float 0.5>
  %result = fadd <4 x float> %scaled, <float 0.25, float 0.25, float 0.25, float 0.25>
  store <4 x float> %result, ptr %out, align 16
  ret void
}
define void @shade(ptr %out, ptr %in, i32 %count) {
entry:
  %empty = icmp eq i32 %count, 0
  br i1 %empty, label %done, label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %loop]
  %src = getelementptr float, ptr %in, i32 %i
  %x = load float, ptr %src, align 4
  %square = fmul float %x, %x
  %noise = call float @host_scale(float %square)
  %result = fadd float %noise, 0.25
  %dst = getelementptr float, ptr %out, i32 %i
  store float %result, ptr %dst, align 4
  %next = add i32 %i, 1
  %more = icmp ult i32 %next, %count
  br i1 %more, label %loop, label %done
done:
  ret void
}
attributes #0 = { "wasm-import-module"="env" "wasm-import-name"="host_scale" }
)IR";

int main()
{
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  std::string error;
  const auto *target = llvm::TargetRegistry::lookupTarget("wasm32-unknown-emscripten", error);
  require(target != nullptr, error.c_str());
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      "wasm32-unknown-emscripten", "generic", "+simd128,+bulk-memory,+atomics",
      options, llvm::Reloc::PIC_));
  require(bool(machine), "target machine");
  {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    require(!llvm::parseAssemblyString("define i32 @broken() { ret float 1.0 }",
                                      diagnostic, context), "malformed IR rejected");
    require(!diagnostic.getMessage().empty(), "malformed IR diagnostic");
  }
  const lld::DriverDef drivers[] = {{lld::Wasm, &lld::wasm::link}};
  const char *bad_args[] = {"wasm-ld", "--threads=4", "--no-entry", "/missing-object.o",
                            "-o", "/bad.wasm"};
  auto rejected = lld::lldMain(bad_args, llvm::outs(), llvm::errs(), drivers);
  require(rejected.retCode != 0 && rejected.canRunAgain, "link failure is recoverable");
  SharedJitState state;
  start_jit_workers(state);
  unsigned table_before = EM_ASM_INT({ return wasmTable.length; });
  for (int index = 0; index < 3; ++index) {
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::string source(shader_ir), marker = "add i32 42, 0";
    source.replace(source.find(marker), marker.size(), "add i32 42, " + std::to_string(index));
    auto module = llvm::parseAssemblyString(source, diagnostic, context);
    require(bool(module), "parse shader IR after failure");
    module->setTargetTriple("wasm32-unknown-emscripten");
    module->setDataLayout(machine->createDataLayout());
    require(!llvm::verifyModule(*module, &llvm::errs()), "verify shader IR");
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes(machine.get());
    passes.registerModuleAnalyses(modules);
    passes.registerCGSCCAnalyses(cgscc);
    passes.registerFunctionAnalyses(functions);
    passes.registerLoopAnalyses(loops);
    passes.crossRegisterProxies(loops, functions, cgscc, modules);
    auto pipeline = passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
    pipeline.run(*module, modules);
    require(!llvm::verifyModule(*module, &llvm::errs()), "verify optimized IR");
    std::string prefix = "/shader-" + std::to_string(index);
    std::error_code io_error;
    {
      llvm::raw_fd_ostream ir(prefix + ".ll", io_error);
      require(!io_error, "open optimized IR");
      module->print(ir, nullptr);
    }
    {
      llvm::raw_fd_ostream object(prefix + ".o", io_error);
      require(!io_error, "open Wasm object");
      llvm::legacy::PassManager emitter;
      require(!machine->addPassesToEmitFile(emitter, object, nullptr,
                                           llvm::CodeGenFileType::ObjectFile),
              "Wasm object emission supported");
      emitter.run(*module);
    }
    std::string object = prefix + ".o", binary = prefix + ".wasm";
    const char *args[] = {"wasm-ld", "--threads=4", "--shared", "--import-memory", "--shared-memory",
                         "--experimental-pic", "--allow-undefined",
                         "--export=folded", "--export=mix", "--export=wide",
                         "--export=shade", "--export=vector4", "--export=increment",
                         "--export=_emscripten_tls_init", "--export=__wasm_call_ctors",
                         "/crtbegin.o", object.c_str(),
                         "-o", binary.c_str()};
    auto result = lld::lldMain(args, llvm::outs(), llvm::errs(), drivers);
    require(result.retCode == 0 && result.canRunAgain, "link generated Wasm repeatedly");
    publish_and_test_jit(state, binary.c_str(), 42 + index);
  }
  finish_jit_workers(state);
  unsigned table_after = EM_ASM_INT({ return wasmTable.length; });
  std::printf("{\"llvmVersion\":\"%s\",\"compiledModules\":3,"
              "\"existingWorkers\":4,\"newWorkers\":3,\"atomicUpdates\":15360,"
              "\"sharedMemoryExecution\":true,\"simdExecution\":true,"
              "\"tableBefore\":%u,\"tableAfter\":%u,\"reclamationVerified\":false}\n",
              LLVM_VERSION_STRING, table_before, table_after);
}
