# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebCyclesOSLABI LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../osl-query-probe/cmake/OSLDependencies.cmake")
find_package(OpenImageIO CONFIG REQUIRED)
add_executable(cycles-osl-abi ../tests/cycles-osl-abi.cc)
target_compile_features(cycles-osl-abi PRIVATE cxx_std_20)
target_compile_definitions(cycles-osl-abi PRIVATE WITH_GHOST_WEB
  "CCL_NAMESPACE_BEGIN=namespace ccl {" "CCL_NAMESPACE_END=}")
target_include_directories(cycles-osl-abi PRIVATE "${BLENDER_WEB_STAGED}/intern/cycles"
  "${BLENDER_SOURCE}/intern/cycles" "${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_options(cycles-osl-abi PRIVATE -pthread -msimd128 -fwasm-exceptions
  -fno-strict-aliasing -UNDEBUG)
target_link_libraries(cycles-osl-abi PRIVATE OpenImageIO::OpenImageIO_Util)
target_link_options(cycles-osl-abi PRIVATE -pthread -fwasm-exceptions -sENVIRONMENT=node
  -sEXIT_RUNTIME=1 -sPTHREAD_POOL_SIZE=4 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608)
set_target_properties(cycles-osl-abi PROPERTIES SUFFIX .js)
