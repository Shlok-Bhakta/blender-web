/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_WebJITProbe.hh"
#include "GHOST_WebOSL.hh"
#include "GHOST_WebOSLOwnedProbe.hh"
#include "GHOST_WebOSLRuntime.hh"
#include "GHOST_WebWorkerResources.hh"
#include <llvm/AsmParser/Parser.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/RTDyldMemoryManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

static void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "JIT data check: %s\n", message);
    std::abort();
  }
}

// Reproduce the native owner's join-before-retirement contract without OSL.
class OwnerMemory : public llvm::RTDyldMemoryManager {
  std::vector<uint8_t *> tokens;

 public:
  uint8_t *allocateCodeSection(uintptr_t, unsigned, unsigned, llvm::StringRef) override
  {
    require(false, "the Wasm engine owns its code allocation");
    return nullptr;
  }
  uint8_t *allocateDataSection(uintptr_t size, unsigned alignment, unsigned,
                               llvm::StringRef, bool) override
  {
    require(size == 1 && alignment == 1, "expected native ownership token");
    auto *token = static_cast<uint8_t *>(std::malloc(size));
    require(token != nullptr, "ownership token allocation");
    tokens.push_back(token);
    return token;
  }
  bool finalizeMemory(std::string *) override { return false; }
  ~OwnerMemory() override
  {
    for (auto *token : tokens) {
      GHOST_web_osl_retire_mapped_range(token, 1);
      std::free(token);
    }
  }
};

static constexpr char program[] = R"IR(
target triple = "wasm32-unknown-emscripten"
@values = global [4 x i32] [i32 11, i32 22, i32 33, i32 44], align 16
@selected = global ptr getelementptr ([4 x i32], ptr @values, i32 0, i32 2), align 4
@wide = global i64 1311768467463790320, align 8
@reader = global ptr @read_value, align 4
define i32 @read_value(i32 %index) {
  %address = getelementptr [4 x i32], ptr @values, i32 0, i32 %index
  %value = load i32, ptr %address, align 4
  ret i32 %value
}
define ptr @data_pointer() { ret ptr @values }
)IR";

static void check_round(unsigned round)
{
  std::vector<std::unique_ptr<llvm::LLVMContext>> contexts;
  std::vector<std::unique_ptr<llvm::ExecutionEngine>> engines;
  std::vector<int *> arrays;
  std::vector<unsigned> handles;
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    const unsigned layout = (iteration * 3 + round * 5) % 8;
    const unsigned padding_bytes = 4096 * (layout + 1);
    std::string ir = std::string(program) + "\n@padding = global [" +
                     std::to_string(padding_bytes) + " x i8] zeroinitializer, align 64\n";
    for (unsigned i = 0; i <= layout * 2; ++i)
      ir += "define i32 @layout_" + std::to_string(i) + "() { ret i32 " +
            std::to_string(layout) + " }\n";
    auto context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic diagnostic;
    auto module = llvm::parseAssemblyString(ir, diagnostic, *context);
    require(bool(module), "valid LLVM fixture");
    std::string error;
    llvm::EngineBuilder builder(std::move(module));
    builder.setErrorStr(&error).setEngineKind(llvm::EngineKind::JIT);
    builder.setMCJITMemoryManager(std::make_unique<OwnerMemory>());
    std::unique_ptr<llvm::ExecutionEngine> engine(builder.create());
    require(bool(engine), error.c_str());
    auto *values = reinterpret_cast<int *>(uintptr_t(engine->getGlobalValueAddress("values")));
    require(values != nullptr, "global lookup must return a data address");
    require(values[0] == 11 && values[3] == 44, "initialized array at its relocated address");
    auto *padding = reinterpret_cast<unsigned char *>(uintptr_t(engine->getGlobalValueAddress("padding")));
    require(padding && !(uintptr_t(padding) % 64), "variable-size data alignment");
    require(padding[0] == 0 && padding[padding_bytes - 1] == 0, "fresh zeroed data allocation");
    padding[0] = padding[padding_bytes - 1] = layout + 1;
    auto *selected = reinterpret_cast<int **>(uintptr_t(engine->getGlobalValueAddress("selected")));
    require(selected && *selected == values + 2, "data pointer relocation");
    auto *wide = reinterpret_cast<uint64_t *>(uintptr_t(engine->getGlobalValueAddress("wide")));
    require(wide && *wide == UINT64_C(0x123456789abcdef0), "full-width initialized global");
    using Reader = int (*)(int);
    auto reader = reinterpret_cast<Reader>(uintptr_t(engine->getFunctionAddress("read_value")));
    auto *reader_address = reinterpret_cast<Reader *>(uintptr_t(engine->getGlobalValueAddress("reader")));
    require(reader && reader_address && *reader_address == reader, "function pointer relocation");
    using DataPointer = int *(*)();
    auto data_pointer = reinterpret_cast<DataPointer>(uintptr_t(engine->getFunctionAddress("data_pointer")));
    GHOST_web_osl_refresh_programs();
    require(data_pointer && data_pointer() == values, "generated code and data lookup agree");
    require(!engine->getGlobalValueAddress("read_value"), "function is not a data symbol");
    require(!engine->getFunctionAddress("values"), "data is not a function symbol");
    require(!engine->getGlobalValueAddress("absent"), "missing data symbol");
    values[2] = 1000 + iteration;
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) {
      workers.emplace_back([=] {
        GHOST_web_osl_refresh_programs();
        require(reader(2) == int(1000 + iteration), "worker sees shared data through generated code");
        require(data_pointer() == values && **selected == reader(2), "worker sees relocated pointers");
      });
    }
    for (auto &worker : workers) worker.join();
    arrays.push_back(values);
    handles.push_back(EM_ASM_INT({
      for (const [handle, p] of Module.ownedState.programs)
        if (p.memoryBase + p.instance.exports.values.value === $0) return handle;
      return 0;
    }, values));
    require(handles.back() != 0, "native owner descriptor");
    engines.push_back(std::move(engine));
    contexts.push_back(std::move(context));
    for (unsigned i = 0; i <= iteration; ++i)
      require(arrays[i][2] == int(1000 + i), "simultaneous owners have independent global data");
  }
  const unsigned order[] = {3, 0, 6, 2, 7, 4, 1, 5};
  unsigned remaining = 8;
  for (unsigned index : order) {
    engines[index].reset();
    GHOST_web_osl_refresh_programs();
    require(GHOST_web_osl_live_programs() == --remaining, "native owner retires program");
    require(!owned_data_symbol(handles[index], "values"), "retired data symbol is unavailable");
    require(!owned_symbol(handles[index], "read_value"), "retired function symbol is unavailable");
    for (unsigned i = 0; i < engines.size(); ++i)
      if (engines[i])
        require(arrays[i][2] == int(1000 + i), "out-of-order retirement preserves live data");
  }
  require(owned_clean(), "all mapped data and table blocks retired");
}

int main()
{
  std::vector<std::thread> initializers;
  for (unsigned i = 0; i < 4; ++i)
    initializers.emplace_back([] {
      for (unsigned repeat = 0; repeat < 3; ++repeat) GHOST_web_osl_initialize_runtime();
    });
  for (auto &initializer : initializers) initializer.join();
  GHOST_web_osl_initialize_runtime();
  for (unsigned round = 0; round < 4; ++round) check_round(round);
  unsigned metrics[3] = {};
  GHOST_web_invoke_runtime_thread([](void *value) {
    EM_ASM({
      const blocks = Module.ownedState.blocks;
      new Uint32Array(wasmMemory.buffer, $0 >>> 0, 3).set([
        blocks.length, blocks.reduce((sum, block) => sum + block.capacity, 0),
        Math.max(...blocks.map(block => block.capacity))]);
    }, value);
  }, metrics);
  const auto [blocks, capacity, largest] = metrics;
  require(blocks > 0 && blocks <= 8 * 8 && capacity <= 8 * 8 * largest,
          "bounded capacity for eight layouts and eight live owners");
  std::printf("{\"programs\":32,\"workerReads\":128,\"globalAddresses\":true,"
              "\"dataRelocations\":true,\"functionRelocations\":true,\"nativeOwnerRetirement\":true,"
              "\"independentOwners\":true,\"outOfOrderRetirement\":true,\"variableLayouts\":8,"
              "\"concurrentInitialization\":true,\"dataAlignment\":64,"
              "\"ownedBlocks\":%u,\"ownedTableCapacity\":%u,\"largestBlock\":%u}\n",
              blocks, capacity, largest);
  return 0;
}
