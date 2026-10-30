# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebCyclesOSLABI LANGUAGES C CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../osl-query-probe/cmake/OSLDependencies.cmake")
find_package(OpenImageIO CONFIG REQUIRED)
add_executable(cycles-osl-abi ../tests/cycles-osl-abi.cc)
add_library(cycles-osl-kernel OBJECT
  "${BLENDER_WEB_STAGED}/intern/cycles/kernel/osl/services.cpp"
  "${BLENDER_SOURCE}/intern/cycles/kernel/osl/closures.cpp"
  "${BLENDER_SOURCE}/intern/cycles/kernel/osl/globals.cpp")
add_library(cycles-osl-scene OBJECT "${BLENDER_WEB_STAGED}/intern/cycles/scene/osl.cpp")
foreach(target cycles-osl-abi cycles-osl-kernel cycles-osl-scene)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE WITH_GHOST_WEB WITH_OSL
    "CCL_NAMESPACE_BEGIN=namespace ccl {" "CCL_NAMESPACE_END=}")
  target_include_directories(${target} PRIVATE "${BLENDER_WEB_STAGED}/intern/cycles"
    "${BLENDER_SOURCE}/intern/cycles" "${BLENDER_SOURCE}/intern/atomic" "${CMAKE_CURRENT_LIST_DIR}/../..")
  target_compile_options(${target} PRIVATE -pthread -msimd128 -fwasm-exceptions
    -fno-strict-aliasing -UNDEBUG)
  target_link_libraries(${target} PRIVATE OpenImageIO::OpenImageIO_Util)
endforeach()
target_link_options(cycles-osl-abi PRIVATE -pthread -fwasm-exceptions -sENVIRONMENT=node
  -sEXIT_RUNTIME=1 -sPTHREAD_POOL_SIZE=4 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608)
set_target_properties(cycles-osl-abi PROPERTIES SUFFIX .js)
