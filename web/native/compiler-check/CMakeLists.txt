# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebInstalledCompilerCheck LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(LLVM 20.1.8 EXACT REQUIRED CONFIG)
find_package(LLD REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
add_executable(llvm-compiler ../tests/llvm-compiler.cc)
target_include_directories(llvm-compiler SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
target_compile_options(llvm-compiler PRIVATE -pthread -msimd128 -fwasm-exceptions)
llvm_map_components_to_libnames(compiler_libraries AsmParser Passes WebAssembly)
target_link_libraries(llvm-compiler PRIVATE lldWasm lldCommon ${compiler_libraries})
target_link_options(llvm-compiler PRIVATE -pthread -fwasm-exceptions
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 -sMAXIMUM_MEMORY=2147483648
  -sMODULARIZE=1 -sEXPORT_NAME=createLLVMProbe -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
  -sEXPORTED_RUNTIME_METHODS=['FS','callMain','PThread'] -sSTACK_SIZE=8388608
  -sENVIRONMENT=web,worker,node -sPTHREAD_POOL_SIZE=8)
set_target_properties(llvm-compiler PROPERTIES SUFFIX .js)

if(BLENDER_WEB_OSL_FIXTURES)
  include("${CMAKE_CURRENT_LIST_DIR}/../osl-query-probe/cmake/OSLDependencies.cmake")
  find_package(OSL 1.15.3.0 EXACT CONFIG REQUIRED)
  add_executable(osl-query ../tests/osl-query.cc)
  target_include_directories(osl-query PRIVATE "${BLENDER_SOURCE}/extern/json/include")
  target_compile_options(osl-query PRIVATE -pthread -msimd128 -fwasm-exceptions)
  target_link_libraries(osl-query PRIVATE OSL::oslquery)
  target_link_options(osl-query PRIVATE -pthread -fwasm-exceptions
    -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sPTHREAD_POOL_SIZE=4
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608
    "--embed-file=${BLENDER_WEB_OSL_FIXTURES}@/fixtures")
  set_target_properties(osl-query PROPERTIES SUFFIX .js)
  file(GLOB fixture_files CONFIGURE_DEPENDS "${BLENDER_WEB_OSL_FIXTURES}/*")
  set_property(TARGET osl-query APPEND PROPERTY LINK_DEPENDS ${fixture_files})
endif()
