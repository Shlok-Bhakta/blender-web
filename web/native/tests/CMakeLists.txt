# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(blender_web_native_checks LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
list(APPEND CMAKE_MODULE_PATH "${BLENDER_SOURCE}/build_files/cmake/Modules")
include("${CMAKE_CURRENT_LIST_DIR}/../dependencies.cmake")
add_executable(dependencies dependencies.cc)
target_include_directories(dependencies PRIVATE ${FREETYPE_INCLUDE_DIRS})
target_link_libraries(dependencies PRIVATE TBB::tbb OpenImageIO::OpenImageIO
  OpenColorIO::OpenColorIO ${FREETYPE_LIBRARIES} ${BROTLI_LIBRARIES})
target_compile_options(dependencies PRIVATE -pthread -msimd128 -fwasm-exceptions)
target_link_options(dependencies PRIVATE -pthread -msimd128 -fwasm-exceptions
  -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8 -sEXIT_RUNTIME=1
  -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=1048576 -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=134217728 -sMAXIMUM_MEMORY=1073741824
  "SHELL:--embed-file ${BLENDER_SOURCE}/release/datafiles/fonts/Inter.woff2@/Inter.woff2"
)
