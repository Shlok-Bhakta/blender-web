# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(OSLOwnedWasmProof LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../osl-query-probe/cmake/OSLDependencies.cmake")
find_package(OpenImageIO CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(LLVM 20.1.8 EXACT CONFIG REQUIRED)
find_package(LLD CONFIG REQUIRED)
include("${OSL_PREFIX}/lib/blender-web-ffi.cmake")
add_executable(osl-runtime ../ghost/GHOST_WebOSLOwnedProbe.cc ../ghost/GHOST_WebJITProbe.cc ../ghost/GHOST_WebFFI.cc ../tests/osl-owned.cc)
target_compile_features(osl-runtime PRIVATE cxx_std_20)
target_compile_definitions(osl-runtime PRIVATE WITH_GHOST_WEB_OSL_PROBE=1)
target_include_directories(osl-runtime PRIVATE ../ghost "${OSL_PREFIX}/include"
  "${OSL_WORK}/osl/src/include" "${OSL_WORK}/osl-wasm/include")
target_include_directories(osl-runtime SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
target_compile_options(osl-runtime PRIVATE -pthread -msimd128 -fwasm-exceptions)
llvm_map_components_to_libnames(runtime_llvm
  AsmParser Passes WebAssembly ExecutionEngine MCJIT RuntimeDyld GlobalISel)
target_link_libraries(osl-runtime PRIVATE "${OSL_PREFIX}/lib/libffi.a"
  "${OSL_WORK}/osl-wasm/lib/liboslexec.a"
  "${OSL_WORK}/osl-wasm/lib/liboslnoise.a"
  "${OSL_WORK}/osl-wasm/lib/liboslquery.a"
  "${OSL_COMPILER_WORK}/osl-wasm/lib/liboslcomp.a"
  OpenImageIO::OpenImageIO pugixml
  "${OSL_PREFIX}/lib/libzstd.a"
  "${OSL_PREFIX}/lib/libbrotlidec-static.a"
  "${OSL_PREFIX}/lib/libbrotlicommon-static.a"
  lldWasm lldCommon ${runtime_llvm})
target_link_options(osl-runtime PRIVATE ${BLENDER_WEB_FFI_LINK_OPTIONS} -pthread -fwasm-exceptions -O0 --profiling-funcs
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sMAXIMUM_MEMORY=4294967296
  -sMAIN_MODULE=2 -sWASMFS=1 -sFORCE_FILESYSTEM=1 -sASSERTIONS=2 -sALLOW_TABLE_GROWTH=1
  "-sEXPORTED_FUNCTIONS=@${OSL_WORK}/helper-exports.json"
  -sMODULARIZE=1 -sEXPORT_NAME=createOSLRuntimeProbe -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
  -sEXPORTED_RUNTIME_METHODS=['FS','callMain','PThread'] -sSTACK_SIZE=8388608
  -sENVIRONMENT=web,worker,node -sPTHREAD_POOL_SIZE=12
  "--embed-file=${SDK_CRTBEGIN}@/crtbegin.o")
set_target_properties(osl-runtime PROPERTIES SUFFIX .js)
set_property(TARGET osl-runtime APPEND PROPERTY LINK_DEPENDS "${OSL_WORK}/helper-exports.json")
