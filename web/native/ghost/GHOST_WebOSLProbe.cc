/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Finite execution proof only. SDK side modules remain allocated. */
#if defined(__EMSCRIPTEN__) && defined(WITH_GHOST_WEB_OSL_PROBE)
#include "GHOST_WebOSL.hh"
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <lld/Common/Driver.h>
#include <emscripten.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
LLD_HAS_DRIVER(wasm)

class WebOSLEngine : public llvm::ExecutionEngine {
  std::unique_ptr<llvm::TargetMachine> machine;
  std::shared_ptr<llvm::LegacyJITSymbolResolver> resolver;
  void *handle = nullptr;
  static inline std::atomic<unsigned> serial{0};
public:
  static void install() { MCJITCtor = create; }
  static llvm::ExecutionEngine *create(std::unique_ptr<llvm::Module> module, std::string *error,
      std::shared_ptr<llvm::MCJITMemoryManager> memory,
      std::shared_ptr<llvm::LegacyJITSymbolResolver> resolver,
      std::unique_ptr<llvm::TargetMachine> original) {
    std::string why;
    auto *target = llvm::TargetRegistry::lookupTarget("wasm32-unknown-emscripten", why);
    if (!target) { if (error) *error = why; return nullptr; }
    auto machine = std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
      "wasm32-unknown-emscripten", "generic", "+simd128,+bulk-memory,+atomics,+exception-handling",
      original->Options, llvm::Reloc::PIC_));
    return new WebOSLEngine(std::move(module), std::move(machine), std::move(resolver));
  }
  WebOSLEngine(std::unique_ptr<llvm::Module> module, std::unique_ptr<llvm::TargetMachine> target,
      std::shared_ptr<llvm::LegacyJITSymbolResolver> symbols)
    : ExecutionEngine(target->createDataLayout(), std::move(module)),
      machine(std::move(target)), resolver(std::move(symbols)) {}
  llvm::TargetMachine *getTargetMachine() override { return machine.get(); }
  llvm::GenericValue runFunction(llvm::Function *, llvm::ArrayRef<llvm::GenericValue>) override {
    llvm::report_fatal_error("OSL proof requires typed function entry");
  }
  void *getPointerToNamedFunction(llvm::StringRef name, bool abort = true) override {
    if (auto *pointer = getPointerToGlobalIfAvailable(name)) return pointer;
    if (resolver) {
      auto symbol = resolver->findSymbol(name.str());
      if (symbol) {
        auto address = symbol.getAddress();
        if (address) return reinterpret_cast<void *>(uintptr_t(*address));
        llvm::consumeError(address.takeError());
      }
    }
    if (LazyFunctionCreator) if (auto *p = LazyFunctionCreator(name.str())) return p;
    if (auto *p = dlsym(RTLD_DEFAULT, name.str().c_str())) return p;
    if (abort) llvm::report_fatal_error(llvm::Twine("Missing OSL function ") + name);
    return nullptr;
  }
  void finalizeObject() override {
    if (handle) return;
    auto &module = *Modules.front();
    module.setTargetTriple("wasm32-unknown-emscripten");
    module.setDataLayout(machine->createDataLayout());
    if (llvm::verifyModule(module, &llvm::errs()))
      llvm::report_fatal_error("OSL generated invalid LLVM IR");
    for (auto &function : module) {
      if (function.isDeclaration() && !function.isIntrinsic() && !function.use_empty()) {
        auto *pointer = getPointerToNamedFunction(function.getName(), false);
        if (pointer) {
          EM_ASM({ wasmImports[UTF8ToString($0)] = getWasmTableEntry($1); },
                  function.getName().str().c_str(), pointer);
        }
      }
    }
    for (auto &global : module.globals()) {
      if (global.isDeclaration() && !global.use_empty()) {
        auto *pointer = getPointerToGlobalIfAvailable(&global);
        if (!pointer) pointer = llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(global.getName().str());
        if (pointer) EM_ASM({ wasmImports[UTF8ToString($0)] = $1; }, global.getName().str().c_str(), pointer);
      }
    }
    std::string prefix = "/osl-program-" + std::to_string(serial.fetch_add(1));
    std::error_code error;
    { llvm::raw_fd_ostream ir(prefix+".ll", error); module.print(ir, nullptr); }
    std::string object = prefix + ".o", binary = prefix + ".wasm";
    { llvm::raw_fd_ostream out(object, error); if(error) llvm::report_fatal_error(llvm::Twine(error.message()));
      llvm::legacy::PassManager emitter;
      if (machine->addPassesToEmitFile(emitter, out, nullptr, llvm::CodeGenFileType::ObjectFile))
        llvm::report_fatal_error("OSL Wasm emission unavailable");
      emitter.run(module); }
    const lld::DriverDef drivers[] = {{lld::Wasm, &lld::wasm::link}};
    const char *args[] = {"wasm-ld", "--threads=4", "--shared", "--import-memory", "--shared-memory",
      "--experimental-pic", "--allow-undefined", "--export-dynamic", "--export=_emscripten_tls_init",
      "--export=__wasm_call_ctors", "/crtbegin.o", object.c_str(), "-o", binary.c_str()};
    auto linked = lld::lldMain(args, llvm::outs(), llvm::errs(), drivers);
    if (linked.retCode || !linked.canRunAgain) llvm::report_fatal_error("OSL Wasm link failed");
    handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) llvm::report_fatal_error(llvm::Twine("OSL Wasm load failed: ") + dlerror());
  }
  uint64_t getFunctionAddress(const std::string &name) override {
    finalizeObject(); return uintptr_t(dlsym(handle, name.c_str()));
  }
  uint64_t getGlobalValueAddress(const std::string &name) override {
    finalizeObject(); return uintptr_t(dlsym(handle, name.c_str()));
  }
  void *getPointerToFunction(llvm::Function *function) override {
    return reinterpret_cast<void *>(uintptr_t(getFunctionAddress(function->getName().str())));
  }
};
void GHOST_web_osl_install_probe_engine() {
  llvm::InitializeAllTargetInfos(); llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs(); llvm::InitializeAllAsmPrinters();
  WebOSLEngine::install();
}

#endif
