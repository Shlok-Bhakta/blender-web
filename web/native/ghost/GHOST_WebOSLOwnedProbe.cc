/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental owned execution and lazy worker publication. */
#if defined(__EMSCRIPTEN__) && defined(WITH_GHOST_WEB_OSL_PROBE)
#  include "GHOST_WebOSLOwnedProbe.hh"
#  include "GHOST_WebJITProbe.hh"
#  include "GHOST_WebOSL.hh"
#  include "GHOST_WebWorkerResources.hh"
#  include <algorithm>
#  include <atomic>
#  include <climits>
#  include <cstdio>
#  include <cstdlib>
#  include <dlfcn.h>
#  include <emscripten.h>
#  include <emscripten/threading.h>
#  include <fstream>
#  include <iterator>
#  include <lld/Common/Driver.h>
#  include <llvm/ExecutionEngine/ExecutionEngine.h>
#  include <llvm/ExecutionEngine/GenericValue.h>
#  include <llvm/ExecutionEngine/MCJIT.h>
#  include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#  include <llvm/IR/LegacyPassManager.h>
#  include <llvm/IR/Module.h>
#  include <llvm/IR/Verifier.h>
#  include <llvm/MC/TargetRegistry.h>
#  include <llvm/Support/DynamicLibrary.h>
#  include <llvm/Support/TargetSelect.h>
#  include <llvm/Support/raw_ostream.h>
#  include <llvm/Target/TargetMachine.h>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <string>
#  include <vector>
LLD_HAS_DRIVER(wasm)

struct OwnedProgram {
  unsigned handle;
  std::vector<char> code;
  uint32_t layout[3];
};
static std::mutex &program_mutex()
{
  static auto *m = new std::mutex;
  return *m;
}
static std::map<uintptr_t, std::shared_ptr<OwnedProgram>> &programs()
{
  static auto *p = new std::map<uintptr_t, std::shared_ptr<OwnedProgram>>;
  return *p;
}
std::atomic<unsigned> GHOST_web_osl_program_epoch{0};
thread_local unsigned GHOST_web_osl_loaded_epoch = UINT_MAX;
static thread_local bool participant = false;
static thread_local std::vector<unsigned> applied;
static void apply_programs(void *)
{
  if (emscripten_is_main_runtime_thread())
    participant = true;
  if (!participant)
    return;
  std::lock_guard<std::mutex> lock(program_mutex());
  unsigned epoch = GHOST_web_osl_program_epoch.load(std::memory_order_acquire);
  if (GHOST_web_osl_loaded_epoch == epoch)
    return;
  std::vector<unsigned> next;
  for (const auto &[token, p] : programs())
    next.push_back(p->handle);
  for (unsigned old : applied)
    if (std::find(next.begin(), next.end(), old) == next.end() && !owned_retire(old))
      llvm::report_fatal_error("Owned OSL retirement failed");
  for (const auto &[token, p] : programs())
    if (owned_create(
            p->code.data(), p->code.size(), p->handle, p->layout[0], p->layout[1], p->layout[2]) !=
        p->handle)
      llvm::report_fatal_error("Owned OSL worker attachment failed");
  applied = std::move(next);
  GHOST_web_osl_loaded_epoch = epoch;
}
static void detach_programs(void *)
{
  for (unsigned old : applied)
    if (!owned_retire(old))
      llvm::report_fatal_error("Owned OSL detach failed");
  applied.clear();
  participant = false;
  GHOST_web_osl_loaded_epoch = UINT_MAX;
}
extern "C" void GHOST_web_osl_refresh_programs()
{
  participant = true;
  GHOST_web_refresh_worker_resources();
}
extern "C" void GHOST_web_osl_retire_mapped_range(void *pointer, size_t size)
{
  const uint64_t begin = reinterpret_cast<uintptr_t>(pointer), end = begin + size;
  struct Range {
    uint64_t begin, end;
    std::vector<std::shared_ptr<OwnedProgram>> retired;
  } range{begin, end, {}};
  GHOST_web_invoke_runtime_thread(
      [](void *value) {
        auto &range = *static_cast<Range *>(value);
        std::lock_guard<std::mutex> lock(program_mutex());
        auto it = programs().lower_bound(uintptr_t(range.begin));
        while (it != programs().end() && uint64_t(it->first) < range.end) {
          range.retired.push_back(it->second);
          it = programs().erase(it);
        }
        if (!range.retired.empty())
          GHOST_web_osl_program_epoch.fetch_add(1, std::memory_order_release);
      },
      &range);
  if (!range.retired.empty())
    GHOST_web_publish_worker_resources();
}

extern "C" unsigned GHOST_web_osl_live_programs()
{
  std::lock_guard<std::mutex> lock(program_mutex());
  return programs().size();
}
class WebOSLEngine : public llvm::ExecutionEngine {
  std::unique_ptr<llvm::TargetMachine> machine;
  std::shared_ptr<llvm::LegacyJITSymbolResolver> resolver;
  unsigned handle = 0;
  std::shared_ptr<llvm::MCJITMemoryManager> memory;
  static inline std::atomic<unsigned> serial{0};

 public:
  static void install()
  {
    MCJITCtor = create;
  }
  static llvm::ExecutionEngine *create(std::unique_ptr<llvm::Module> module,
                                       std::string *error,
                                       std::shared_ptr<llvm::MCJITMemoryManager> memory,
                                       std::shared_ptr<llvm::LegacyJITSymbolResolver> resolver,
                                       std::unique_ptr<llvm::TargetMachine> original)
  {
    std::string why;
    auto *target = llvm::TargetRegistry::lookupTarget("wasm32-unknown-emscripten", why);
    if (!target) {
      if (error)
        *error = why;
      return nullptr;
    }
    auto machine = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine("wasm32-unknown-emscripten",
                                    "generic",
                                    "+simd128,+bulk-memory,+atomics,+exception-handling",
                                    original->Options,
                                    llvm::Reloc::PIC_));
    return new WebOSLEngine(
        std::move(module), std::move(machine), std::move(resolver), std::move(memory));
  }
  WebOSLEngine(std::unique_ptr<llvm::Module> module,
               std::unique_ptr<llvm::TargetMachine> target,
               std::shared_ptr<llvm::LegacyJITSymbolResolver> symbols,
               std::shared_ptr<llvm::MCJITMemoryManager> mm)
      : ExecutionEngine(target->createDataLayout(), std::move(module)),
        machine(std::move(target)),
        resolver(std::move(symbols)),
        memory(std::move(mm))
  {
  }
  llvm::TargetMachine *getTargetMachine() override
  {
    return machine.get();
  }
  llvm::GenericValue runFunction(llvm::Function *, llvm::ArrayRef<llvm::GenericValue>) override
  {
    llvm::report_fatal_error("OSL proof requires typed function entry");
  }
  void *getPointerToNamedFunction(llvm::StringRef name, bool abort = true) override
  {
    if (auto *pointer = getPointerToGlobalIfAvailable(name))
      return pointer;
    if (resolver) {
      auto symbol = resolver->findSymbol(name.str());
      if (symbol) {
        auto address = symbol.getAddress();
        if (address)
          return reinterpret_cast<void *>(uintptr_t(*address));
        llvm::consumeError(address.takeError());
      }
    }
    if (LazyFunctionCreator)
      if (auto *p = LazyFunctionCreator(name.str()))
        return p;
    if (auto *p = dlsym(RTLD_DEFAULT, name.str().c_str()))
      return p;
    if (abort)
      llvm::report_fatal_error(llvm::Twine("Missing OSL function ") + name);
    return nullptr;
  }
  void finalizeObject() override
  {
    if (handle)
      return;
    static std::mutex compiler_mutex;
    std::lock_guard<std::mutex> compiler_lock(compiler_mutex);
    auto &module = *Modules.front();
    module.setTargetTriple("wasm32-unknown-emscripten");
    module.setDataLayout(machine->createDataLayout());
    if (llvm::verifyModule(module, &llvm::errs()))
      llvm::report_fatal_error("OSL generated invalid LLVM IR");
    for (auto &function : module) {
      if (function.isDeclaration() && !function.isIntrinsic() && !function.use_empty()) {
        auto *pointer = getPointerToNamedFunction(function.getName(), false);
        if (pointer) {
          EM_ASM(
              { wasmImports[UTF8ToString($0)] = getWasmTableEntry($1); },
              function.getName().str().c_str(),
              pointer);
        }
      }
    }
    for (auto &global : module.globals()) {
      if (global.isDeclaration() && !global.use_empty()) {
        auto *pointer = getPointerToGlobalIfAvailable(&global);
        if (!pointer)
          pointer = llvm::sys::DynamicLibrary::SearchForAddressOfSymbol(global.getName().str());
        if (pointer)
          EM_ASM({ wasmImports[UTF8ToString($0)] = $1; }, global.getName().str().c_str(), pointer);
      }
    }
    std::string prefix = "/osl-program-" + std::to_string(serial.fetch_add(1));
    std::error_code error;

    std::string object = prefix + ".o", binary = prefix + ".wasm";
    {
      llvm::raw_fd_ostream out(object, error);
      if (error)
        llvm::report_fatal_error(llvm::Twine(error.message()));
      llvm::legacy::PassManager emitter;
      if (machine->addPassesToEmitFile(emitter, out, nullptr, llvm::CodeGenFileType::ObjectFile))
        llvm::report_fatal_error("OSL Wasm emission unavailable");
      emitter.run(module);
    }
    const lld::DriverDef drivers[] = {{lld::Wasm, &lld::wasm::link}};
    const char *args[] = {"wasm-ld",
                          "--threads=4",
                          "--shared",
                          "--import-memory",
                          "--shared-memory",
                          "--experimental-pic",
                          "--allow-undefined",
                          "--export-dynamic",
                          "--export=_emscripten_tls_init",
                          "--export=__wasm_call_ctors",
                          "/crtbegin.o",
                          object.c_str(),
                          "-o",
                          binary.c_str()};
    auto linked = lld::lldMain(args, llvm::outs(), llvm::errs(), drivers);
    if (linked.retCode || !linked.canRunAgain)
      llvm::report_fatal_error("OSL Wasm link failed");
    auto program = std::make_shared<OwnedProgram>();
    std::ifstream input(binary, std::ios::binary);
    program->code.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    input.close();
    std::remove(object.c_str());
    std::remove(binary.c_str());
    GHOST_web_invoke_runtime_thread(
        [](void *value) {
          auto &p = *static_cast<OwnedProgram *>(value);
          p.handle = owned_create(p.code.data(), p.code.size(), 0, 0, 0, 0);
          if (!p.handle) {
            EM_ASM({ console.error(Module.ownedLastError); });
            llvm::report_fatal_error("Owned OSL load failed");
          }
          owned_describe(p.handle, p.layout);
        },
        program.get());
    handle = program->handle;
    auto *token = memory->allocateDataSection(1, 1, 0, "GHOST Web OSL owner", false);
    if (!token)
      llvm::report_fatal_error("Original OSL memory owner allocation failed");
    struct Publication {
      uintptr_t token;
      std::shared_ptr<OwnedProgram> program;
    } publication{reinterpret_cast<uintptr_t>(token), program};
    GHOST_web_invoke_runtime_thread(
        [](void *value) {
          auto &p = *static_cast<Publication *>(value);
          std::lock_guard<std::mutex> lock(program_mutex());
          programs().emplace(p.token, p.program);
          GHOST_web_osl_program_epoch.fetch_add(1, std::memory_order_release);
        },
        &publication);
    GHOST_web_publish_worker_resources();
  }
  uint64_t getFunctionAddress(const std::string &name) override
  {
    finalizeObject();
    struct Query {
      unsigned handle;
      const char *name;
      uintptr_t result;
    } query{handle, name.c_str(), 0};
    GHOST_web_invoke_runtime_thread(
        [](void *value) {
          auto &q = *static_cast<Query *>(value);
          q.result = owned_symbol(q.handle, q.name);
        },
        &query);
    return query.result;
  }
  uint64_t getGlobalValueAddress(const std::string &name) override
  {
    finalizeObject();
    struct Query {
      unsigned handle;
      const char *name;
      uintptr_t result;
    } query{handle, name.c_str(), 0};
    GHOST_web_invoke_runtime_thread(
        [](void *value) {
          auto &q = *static_cast<Query *>(value);
          q.result = owned_symbol(q.handle, q.name);
        },
        &query);
    return query.result;
  }
  void *getPointerToFunction(llvm::Function *function) override
  {
    return reinterpret_cast<void *>(uintptr_t(getFunctionAddress(function->getName().str())));
  }
};
void GHOST_web_osl_install_probe_engine()
{
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    GHOST_web_register_worker_resource({apply_programs, detach_programs, nullptr});
  });
  WebOSLEngine::install();
}

#endif
