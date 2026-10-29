# Use target CMake exports instead of executing a target llvm-config.
find_package(LLVM 20.1.8 EXACT CONFIG REQUIRED)
find_package(LLD CONFIG REQUIRED)
find_package(Clang CONFIG REQUIRED)
set(LLVM_VERSION "${LLVM_PACKAGE_VERSION}")
set(LLVM_INCLUDES ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
set(LLVM_DIRECTORY "${LLVM_BINARY_DIR}")
set(LLVM_LIB_DIR "${LLVM_BINARY_DIR}/lib")
set(LLVM_TARGETS WebAssembly)
set(LLVM_SHARED_MODE static)
set(LLVM_MCJIT_LIBRARY LLVMMCJIT)
llvm_map_components_to_libnames(LLVM_LIBRARIES
  Core Passes WebAssembly AsmParser BitReader BitWriter IRReader Linker
  ExecutionEngine MCJIT RuntimeDyld)
set(CLANG_LIBRARIES clangFrontend clangDriver clangSerialization clangParse
  clangSema clangAnalysis clangAST clangASTMatchers clangEdit clangLex
  clangSupport clangAPINotes clangBasic)
set(LLVM_FOUND TRUE)
