# SPDX-License-Identifier: GPL-2.0-or-later
# Copied to a private source root so upstream's output paths stay in the build.
cmake_minimum_required(VERSION 3.24)
project(BlenderWebCyclesOSLShaders LANGUAGES NONE)
set(OSL_COMPILER "${BLENDER_WEB_PREFIX}/bin/oslc")
set(OSL_SHADER_DIR "${BLENDER_WEB_PREFIX}/share/OSL/shaders" CACHE PATH "" FORCE)
set(WITH_STRICT_BUILD_OPTIONS ON)
# The standalone check builds outputs but has no installation or IDE layout.
function(delayed_install)
endfunction()
function(cycles_set_solution_folder)
endfunction()
add_subdirectory(intern/cycles/kernel/osl/shaders)
get_target_property(shader_sources cycles_osl_shaders SOURCES)
list(FILTER shader_sources INCLUDE REGEX "\\.osl$")
file(WRITE "${CMAKE_BINARY_DIR}/shader-sources.txt" "")
foreach(shader IN LISTS shader_sources)
  get_filename_component(shader "${shader}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}/intern/cycles/kernel/osl/shaders")
  file(APPEND "${CMAKE_BINARY_DIR}/shader-sources.txt" "${shader}\n")
endforeach()
