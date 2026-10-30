# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebBindingCheck LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(OpenEXR CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(OpenJPEG CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(libheif CONFIG QUIET)
find_package(OpenImageIO CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)
find_package(MaterialX CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
set(prefix "${BLENDER_WEB_PREFIX}")
include("${prefix}/lib/blender-web-numpy/libraries.cmake")
include("${prefix}/lib/blender-web-bindings/libraries.cmake")
add_executable(check "${CMAKE_CURRENT_LIST_DIR}/../../tests/python-bindings.cc")
target_include_directories(check PRIVATE "${prefix}/include" "${prefix}/include/python3.13")
target_link_libraries(check PRIVATE
  ${BLENDER_WEB_PYTHON_BINDING_LIBRARIES} ${BLENDER_WEB_NUMPY_LIBRARIES}
  "${prefix}/lib/libpython3.13.a" OpenImageIO::OpenImageIO OpenColorIO::OpenColorIO
  MaterialXGenGlsl MaterialXGenMsl MaterialXGenShader MaterialXFormat MaterialXCore
  "${prefix}/lib/libopenvdb.a" "${prefix}/lib/libblosc.a" TBB::tbb
  Imath::Imath Freetype::Freetype "${prefix}/lib/libbrotlidec-static.a"
  "${prefix}/lib/libbrotlicommon-static.a" "${prefix}/lib/libzstd.a" ZLIB::ZLIB)
target_compile_options(check PRIVATE -pthread -msimd128 -fwasm-exceptions)
target_link_options(check PRIVATE -O2 --profiling-funcs -pthread -msimd128 -fwasm-exceptions
  -sUSE_BZIP2=1 -sENVIRONMENT=node -sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=8
  -sPTHREAD_POOL_SIZE_STRICT=0 -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1
  -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=8388608 -sDEFAULT_PTHREAD_STACK_SIZE=8388608
  -sWASMFS=1 -sFORCE_FILESYSTEM=1
  "--preload-file=${BLENDER_WEB_ASSETS}/python3.13@/python/lib/python3.13"
  "--preload-file=${CMAKE_CURRENT_LIST_DIR}/../../tests/python-bindings.py@/python-bindings.py")
set_property(TARGET check APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/../../tests/python-bindings.py")
foreach(package PyOpenColorIO OpenImageIO MaterialX oslquery)
  target_link_options(check PRIVATE
    "--preload-file=${prefix}/lib/python3.13/site-packages/${package}@/python/lib/python3.13/site-packages/${package}")
endforeach()
if(BLENDER_WEB_OSL_FIXTURE)
  target_compile_definitions(check PRIVATE BLENDER_WEB_BINDING_TEST_SCRIPT="/osl-python.py")
  target_link_options(check PRIVATE
    "--preload-file=${CMAKE_CURRENT_LIST_DIR}/../../tests/osl-python.py@/osl-python.py"
    "--preload-file=${BLENDER_WEB_OSL_FIXTURE}@/fixtures/web_query.oso")
  set_property(TARGET check APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../tests/osl-python.py" "${BLENDER_WEB_OSL_FIXTURE}")
endif()
