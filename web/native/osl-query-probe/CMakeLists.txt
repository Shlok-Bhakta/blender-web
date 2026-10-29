# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(OSLQueryWasmCheck LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OSLDependencies.cmake")
find_package(OpenImageIO CONFIG REQUIRED)
add_executable(osl-query ../tests/osl-query.cc)
target_compile_features(osl-query PRIVATE cxx_std_17)
target_include_directories(osl-query PRIVATE
  "${OSL_WORK}/osl/src/include" "${OSL_WORK}/osl-wasm/include"
  "${BLENDER_SOURCE}/extern/json/include")
target_compile_options(osl-query PRIVATE -pthread -msimd128 -fwasm-exceptions)
target_link_libraries(osl-query PRIVATE
  "${OSL_WORK}/osl-wasm/lib/liboslquery.a" OpenImageIO::OpenImageIO_Util)
target_link_options(osl-query PRIVATE -pthread -fwasm-exceptions
  -sMODULARIZE=1 -sEXPORT_NAME=createOSLQueryProbe -sEXIT_RUNTIME=0 -sINVOKE_RUN=0
  -sEXPORTED_RUNTIME_METHODS=['callMain','PThread'] -sPTHREAD_POOL_SIZE=8
  -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608
  "--embed-file=${OSL_WORK}/query-fixtures@/fixtures")
set_target_properties(osl-query PROPERTIES SUFFIX .js)

file(GLOB fixture_files CONFIGURE_DEPENDS "${OSL_WORK}/query-fixtures/*")
set_property(TARGET osl-query APPEND PROPERTY LINK_DEPENDS ${fixture_files})
