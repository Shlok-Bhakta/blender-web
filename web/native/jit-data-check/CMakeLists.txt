# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebJITDataCheck LANGUAGES C CXX)
find_package(LLVM 20.1.8 EXACT CONFIG REQUIRED)
find_package(LLD CONFIG REQUIRED)
include("${BLENDER_WEB_PREFIX}/lib/blender-web-ffi.cmake")
add_executable(jit-data ../ghost/GHOST_WebOSLOwnedProbe.cc ../ghost/GHOST_WebJITProbe.cc
  ../ghost/GHOST_WebFFI.cc ../tests/jit-data.cc)
target_compile_features(jit-data PRIVATE cxx_std_20)
target_compile_definitions(jit-data PRIVATE WITH_GHOST_WEB_OSL_PROBE=1)
target_include_directories(jit-data PRIVATE ../ghost "${BLENDER_WEB_PREFIX}/include")
target_include_directories(jit-data SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
target_compile_options(jit-data PRIVATE -pthread -msimd128 -fwasm-exceptions)
llvm_map_components_to_libnames(jit_libraries
  AsmParser Passes WebAssembly ExecutionEngine MCJIT RuntimeDyld GlobalISel)
target_link_libraries(jit-data PRIVATE "${BLENDER_WEB_PREFIX}/lib/libffi.a"
  lldWasm lldCommon ${jit_libraries})
target_link_options(jit-data PRIVATE ${BLENDER_WEB_FFI_LINK_OPTIONS}
  -pthread -fwasm-exceptions -O0 --profiling-funcs
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sMAXIMUM_MEMORY=4294967296
  -sMAIN_MODULE=2 -sWASMFS=1 -sFORCE_FILESYSTEM=1 -sASSERTIONS=2 -sALLOW_TABLE_GROWTH=1
  -sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_pthread_self']
  -sMODULARIZE=1 -sEXPORT_NAME=createJITDataCheck -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
  -sEXPORTED_RUNTIME_METHODS=['FS','callMain','PThread'] -sSTACK_SIZE=8388608
  -sENVIRONMENT=web,worker,node -sPTHREAD_POOL_SIZE=8 -sPROXY_TO_PTHREAD=1
  -sDEFAULT_PTHREAD_STACK_SIZE=8388608
  "--embed-file=${SDK_CRTBEGIN}@/crtbegin.o")
set_target_properties(jit-data PROPERTIES SUFFIX .js)
