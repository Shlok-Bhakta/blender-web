# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(BlenderWebPythonBindings LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "These static bindings require the pinned Wasm toolchain")
endif()
foreach(required BLENDER_WEB_BINDING_SOURCES BLENDER_WEB_PREFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
set(src "${BLENDER_WEB_BINDING_SOURCES}")
set(prefix "${BLENDER_WEB_PREFIX}")
find_package(OpenEXR CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(OpenJPEG CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(libheif CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)
find_package(MaterialX CONFIG REQUIRED)

function(web_binding target)
  add_library(${target} STATIC ${ARGN})
  target_include_directories(${target} PRIVATE
    "${prefix}/include" "${prefix}/include/python3.13"
    "${src}/pybind11/include")
  target_compile_definitions(${target} PRIVATE PY_CALL_TRAMPOLINE)
endfunction()

file(GLOB ocio_sources CONFIGURE_DEPENDS
  "${src}/opencolorio/src/bindings/python/*.cpp"
  "${src}/opencolorio/src/bindings/python/transforms/*.cpp"
  "${src}/opencolorio/src/bindings/python/apphelpers/*.cpp")
web_binding(PyOpenColorIO ${ocio_sources})
target_include_directories(PyOpenColorIO PRIVATE
  "${src}/opencolorio/src/bindings/python"
  "${src}/opencolorio/src" "${src}/opencolorio/share/docs")
target_compile_definitions(PyOpenColorIO PRIVATE OpenColorIO_SKIP_IMPORTS
  PYOCIO_NAME=PyOpenColorIO PY_VERSION_MAJOR=3 PY_VERSION_MINOR=13 PY_VERSION_PATCH=13)
target_link_libraries(PyOpenColorIO PRIVATE OpenColorIO::OpenColorIO)

file(GLOB oiio_sources CONFIGURE_DEPENDS "${src}/openimageio/src/python/*.cpp")
web_binding(PyOpenImageIO ${oiio_sources})
# The original bindings expose internal experimental FLIP declarations too.
target_compile_definitions(PyOpenImageIO PRIVATE OIIO_INTERNAL=1 PYMODULE_NAME=OpenImageIO)
target_link_libraries(PyOpenImageIO PRIVATE OpenImageIO::OpenImageIO)

web_binding(nanobind_runtime "${src}/nanobind/src/nb_combined.cpp")
web_binding(openvdb_python
  "${src}/openvdb/openvdb/openvdb/python/pyFloatGrid.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyGridBase.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyIntGrid.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyMetadata.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyOpenVDBModule.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyPointGrid.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyTransform.cc"
  "${src}/openvdb/openvdb/openvdb/python/pyVec3Grid.cc")
foreach(target nanobind_runtime openvdb_python)
  target_include_directories(${target} PRIVATE "${src}/nanobind/include"
    "${src}/nanobind/ext/robin_map/include")
  target_compile_definitions(${target} PRIVATE NB_STATIC)
  target_compile_options(${target} PRIVATE -fno-strict-aliasing)
endforeach()
# Match Blender's native recipe. AX is disabled there; all nine grid types are enabled.
target_compile_definitions(openvdb_python PRIVATE PY_OPENVDB_WRAP_ALL_GRID_TYPES)

set(materialx "${src}/materialx/source")
set(additional_libraries)
foreach(module GenOsl GenMdl Render)
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${materialx}/MaterialX${module}/*.cpp")
  if(module STREQUAL "Render")
    # MATERIALX_BUILD_OIIO is also off in the native recipe.
    list(REMOVE_ITEM sources "${materialx}/MaterialXRender/OiioImageLoader.cpp")
  endif()
  add_library(MaterialX${module} STATIC ${sources})
  target_include_directories(MaterialX${module} PRIVATE "${materialx}" "${prefix}/include")
  target_link_libraries(MaterialX${module} PUBLIC MaterialXGenShader MaterialXCore)
  list(APPEND additional_libraries MaterialX${module})
endforeach()
set(materialx_bindings)
foreach(module Core Format GenShader GenGlsl GenMsl GenOsl GenMdl Render)
  set(directory "${materialx}/PyMaterialX/PyMaterialX${module}")
  file(GLOB sources CONFIGURE_DEPENDS "${directory}/*.cpp")
  list(REMOVE_ITEM sources "${directory}/PyOiioImageLoader.cpp")
  web_binding(PyMaterialX${module} ${sources})
  target_include_directories(PyMaterialX${module} PRIVATE "${materialx}" "${materialx}/PyMaterialX")
  target_link_libraries(PyMaterialX${module} PRIVATE MaterialX${module})
  if(EXISTS "${directory}/PyUtil.cpp")
    # Shared native modules hide these identical helper names. Static archives
    # need distinct names, or one module silently receives another's bindings.
    set_source_files_properties("${directory}/PyModule.cpp" "${directory}/PyUtil.cpp"
      PROPERTIES COMPILE_DEFINITIONS "bindPyUtil=blender_web_PyMaterialX${module}_bindPyUtil")
  endif()
  list(APPEND materialx_bindings PyMaterialX${module})
endforeach()
install(TARGETS PyOpenColorIO PyOpenImageIO nanobind_runtime openvdb_python
  ${materialx_bindings} ${additional_libraries}
  ARCHIVE DESTINATION lib/blender-web-bindings)
