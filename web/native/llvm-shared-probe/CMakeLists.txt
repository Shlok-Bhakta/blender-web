# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebLLVMSharedProbe LANGUAGES C CXX)
find_package(LLVM 20.1.8 EXACT REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
if(NOT EXISTS "${SDK_CRTBEGIN}")
  message(FATAL_ERROR "Provide the pinned SDK PIC crtbegin.o for real TLS initialization")
endif()
add_executable(llvm-compiler ../tests/llvm-shared.cc)
target_compile_features(llvm-compiler PRIVATE cxx_std_17)
target_include_directories(llvm-compiler SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
target_compile_options(llvm-compiler PRIVATE -pthread -msimd128 -fwasm-exceptions)
llvm_map_components_to_libnames(LLVM_PROBE_LIBS AsmParser Passes WebAssembly)
target_link_libraries(llvm-compiler PRIVATE lldWasm lldCommon ${LLVM_PROBE_LIBS})
target_link_options(llvm-compiler PRIVATE -pthread -fwasm-exceptions
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sMAXIMUM_MEMORY=2147483648
  -sMAIN_MODULE=2 -sALLOW_TABLE_GROWTH=1 -sEXPORTED_FUNCTIONS=['_main','_host_scale','_malloc','_free','_pthread_self','_emscripten_builtin_memalign','_emscripten_builtin_free'] -sMODULARIZE=1 -sEXPORT_NAME=createLLVMProbe -sEXIT_RUNTIME=0
  -sINVOKE_RUN=0 -sEXPORTED_RUNTIME_METHODS=['FS','callMain','PThread']
  -sSTACK_SIZE=8388608
  -sENVIRONMENT=web,worker,node -sPTHREAD_POOL_SIZE=12)
set_target_properties(llvm-compiler PROPERTIES SUFFIX .js)

target_link_options(llvm-compiler PRIVATE "--embed-file=${SDK_CRTBEGIN}@/crtbegin.o")
