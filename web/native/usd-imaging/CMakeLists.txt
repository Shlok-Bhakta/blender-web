# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(blender_web_usd_imaging LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(OpenEXR CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(OpenJPEG CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(libheif CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(pxr CONFIG REQUIRED)

# These original CPU readers sit behind upstream's global GPU build gate.
# Build their original translation units without enabling graphics backends.
add_library(hioOpenVDB STATIC
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/debugCodes.cpp"
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/utils.cpp"
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/vdbAssetInterface.cpp"
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/vdbTextureData.cpp")
add_library(hioOiio STATIC
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/plugin/hioOiio/oiioImage.cpp"
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/plugin/hioOiio/metadata.cpp")
foreach(component hioOpenVDB hioOiio)
  target_include_directories(${component} PRIVATE
    "${BLENDER_WEB_PREFIX}/include" "${BLENDER_WEB_USD_SOURCE}"
    "${BLENDER_WEB_PREFIX}/include/python3.13")
  target_compile_definitions(${component} PRIVATE
    PXR_OPENVDB_SUPPORT_ENABLED PXR_OIIO_PLUGIN_ENABLED)
  target_link_libraries(${component} PRIVATE usd_m
    OpenImageIO::OpenImageIO Imath::Imath TBB::tbb)
  install(TARGETS ${component} ARCHIVE DESTINATION lib/blender-web-usd-imaging)
endforeach()
target_compile_definitions(hioOpenVDB PRIVATE OPENVDB_STATICLIB)
install(FILES
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/api.h"
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/utils.h"
  "${BLENDER_WEB_USD_SOURCE}/pxr/imaging/hioOpenVDB/vdbAssetInterface.h"
  DESTINATION include/pxr/imaging/hioOpenVDB)
