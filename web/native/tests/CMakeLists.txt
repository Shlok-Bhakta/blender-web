# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(blender_web_native_checks LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
list(APPEND CMAKE_MODULE_PATH "${BLENDER_SOURCE}/build_files/cmake/Modules")
include("${CMAKE_CURRENT_LIST_DIR}/../dependencies.cmake")
if(WITH_LIBMV)
  add_executable(tracking-check tracking.cc)
  target_link_libraries(tracking-check PRIVATE Ceres::ceres)
  target_compile_options(tracking-check PRIVATE -pthread -msimd128 -fwasm-exceptions -UNDEBUG)
  target_link_options(tracking-check PRIVATE -O1 -pthread -msimd128 -fwasm-exceptions
    -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8 -sPTHREAD_POOL_SIZE_STRICT=0
    -sEXIT_RUNTIME=1 -sSTACK_SIZE=8388608 -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=1)
endif()
if(WITH_HARFBUZZ AND WITH_FRIBIDI)
  add_executable(text-check text.cc)
  target_include_directories(text-check PRIVATE ${FREETYPE_INCLUDE_DIRS}
    ${LIBHARFBUZZ_INCLUDE_DIRS} ${LIBFRIBIDI_INCLUDE_DIRS}
    "${BLENDER_SOURCE}/source/blender/blenlib" "${BLENDER_SOURCE}/intern/guardedalloc")
  target_link_libraries(text-check PRIVATE ${LIBHARFBUZZ_LIBRARIES}
    ${LIBFRIBIDI_LIBRARIES} ${FREETYPE_LIBRARIES} ${BROTLI_LIBRARIES})
  target_compile_options(text-check PRIVATE -pthread -msimd128 -fwasm-exceptions -UNDEBUG)
  target_link_options(text-check PRIVATE -O1 -pthread -msimd128 -fwasm-exceptions
    -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1)
endif()
if(WITH_CODEC_FFMPEG AND WITH_CODEC_SNDFILE)
  add_executable(media-check media.cc)
  target_include_directories(media-check PRIVATE ${FFMPEG_INCLUDE_DIRS} ${LIBSNDFILE_INCLUDE_DIRS} ${RUBBERBAND_INCLUDE_DIRS})
  target_link_libraries(media-check PRIVATE
    -Wl,--start-group ${FFMPEG_LIBRARIES} ${LIBSNDFILE_LIBRARIES} ${RUBBERBAND_LIBRARIES} -Wl,--end-group)
  target_compile_options(media-check PRIVATE -pthread -msimd128 -fwasm-exceptions -UNDEBUG)
  target_link_options(media-check PRIVATE -O1 -pthread -msimd128 -fwasm-exceptions
    -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=16 -sPTHREAD_POOL_SIZE_STRICT=0 -sEXIT_RUNTIME=1
    -sSTACK_SIZE=8388608 -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=1 -sWASMFS=1 -sFORCE_FILESYSTEM=1
    "SHELL:--embed-file ${CMAKE_CURRENT_LIST_DIR}/../../tests/fixtures/media/reference.mkv@/reference.mkv")
endif()
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

if(WITH_OPENVDB)
  add_executable(openvdb-check openvdb.cc)
  target_include_directories(openvdb-check PRIVATE ${OPENVDB_INCLUDE_DIRS} ${NANOVDB_INCLUDE_DIRS})
  target_compile_definitions(openvdb-check PRIVATE OPENVDB_STATICLIB NANOVDB_USE_TBB NANOVDB_USE_OPENVDB)
  target_compile_options(openvdb-check PRIVATE -pthread -msimd128 -fwasm-exceptions)
  target_link_libraries(openvdb-check PRIVATE ${OPENVDB_LIBRARIES})
  target_link_options(openvdb-check PRIVATE -pthread -msimd128 -fwasm-exceptions
    -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=1
    -sSTACK_SIZE=8388608 -sALLOW_MEMORY_GROWTH=1 -sNODERAWFS=1)
endif()

if(WITH_USD AND EXISTS "${CMAKE_CURRENT_BINARY_DIR}/../assets/python3.13")
  include("${CMAKE_PREFIX_PATH}/lib/blender-web-usd/libraries.cmake")
  add_executable(usd-python-check usd_python.cc)
  target_include_directories(usd-python-check PRIVATE
    "${CMAKE_PREFIX_PATH}/include" "${CMAKE_PREFIX_PATH}/include/python3.13")
  target_compile_options(usd-python-check PRIVATE -pthread -msimd128 -fwasm-exceptions)
  target_link_libraries(usd-python-check PRIVATE
    -Wl,--start-group ${BLENDER_WEB_USD_PYTHON_LIBRARIES}
    ${USD_LIBRARIES} "${CMAKE_PREFIX_PATH}/lib/libpython3.13.a"
    "${CMAKE_PREFIX_PATH}/lib/libz.a" -Wl,--end-group)
  target_link_options(usd-python-check PRIVATE -pthread -msimd128 -fwasm-exceptions
    -sUSE_BZIP2=1 -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8
    -sEXIT_RUNTIME=1 -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=4194304
    -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=1 -sWASMFS=1 -sFORCE_FILESYSTEM=1
    "SHELL:--preload-file ${CMAKE_CURRENT_BINARY_DIR}/../assets/python3.13@/python/lib/python3.13")
endif()

if(WITH_MATERIALX)
  find_package(PugiXML REQUIRED)
  add_executable(materialx-check materialx.cc)
  target_include_directories(materialx-check PRIVATE ${PUGIXML_INCLUDE_DIRS})
  target_compile_options(materialx-check PRIVATE -pthread -msimd128 -fwasm-exceptions)
  target_link_libraries(materialx-check PRIVATE
    -Wl,--whole-archive MaterialXFormat ${PUGIXML_LIBRARIES} -Wl,--no-whole-archive MaterialXCore)
  target_link_options(materialx-check PRIVATE -pthread -msimd128 -fwasm-exceptions
    -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608
    -sWASMFS=1 -sFORCE_FILESYSTEM=1
    "SHELL:--embed-file ${CMAKE_PREFIX_PATH}/libraries@/libraries")
endif()

if(WITH_CYCLES_EMBREE)
  add_executable(embree-check embree.cc)
  target_link_libraries(embree-check PRIVATE ${EMBREE_LIBRARIES} TBB::tbb)
  target_compile_options(embree-check PRIVATE -pthread -msimd128 -fwasm-exceptions -UNDEBUG)
  target_link_options(embree-check PRIVATE -O2 -pthread -msimd128 -fwasm-exceptions
    -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8 -sPTHREAD_POOL_SIZE_STRICT=0
    -sEXIT_RUNTIME=1 -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=4194304
    -sINITIAL_MEMORY=134217728 -sALLOW_MEMORY_GROWTH=1)
endif()
