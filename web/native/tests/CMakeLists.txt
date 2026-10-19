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

find_package(Fftw3 REQUIRED)
find_library(FFTW_DOUBLE_THREADS NAMES fftw3_threads REQUIRED)
add_executable(fftw-check fftw.cc)
target_include_directories(fftw-check PRIVATE ${FFTW3_INCLUDE_DIRS})
target_compile_options(fftw-check PRIVATE -pthread -msimd128 -fwasm-exceptions)
target_link_libraries(fftw-check PRIVATE
  ${FFTW_DOUBLE_THREADS} ${FFTW3_LIBRARY_THREADS_F} ${FFTW3_LIBRARY_D} ${FFTW3_LIBRARY_F})
target_link_options(fftw-check PRIVATE -pthread -msimd128 -fwasm-exceptions
  -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8 -sEXIT_RUNTIME=1
  -sSTACK_SIZE=8388608 -sALLOW_MEMORY_GROWTH=1)

find_package(Alembic REQUIRED)
add_executable(alembic-check alembic.cc)
target_include_directories(alembic-check PRIVATE ${ALEMBIC_INCLUDE_DIRS})
target_link_libraries(alembic-check PRIVATE ${ALEMBIC_LIBRARIES} Imath::Imath)
target_compile_options(alembic-check PRIVATE -pthread -msimd128 -fwasm-exceptions)
target_link_options(alembic-check PRIVATE -pthread -msimd128 -fwasm-exceptions
  -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=1
  -sSTACK_SIZE=8388608 -sALLOW_MEMORY_GROWTH=1 -sWASMFS=1 -sFORCE_FILESYSTEM=1
  "SHELL:--embed-file ${CMAKE_CURRENT_LIST_DIR}/../../tests/fixtures/ocean-alembic/desktop-ocean.abc@/desktop-ocean.abc")
