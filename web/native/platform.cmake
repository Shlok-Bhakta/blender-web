# SPDX-License-Identifier: GPL-2.0-or-later
# Included by the guarded Emscripten platform hook in Blender's root CMake file.
if(NOT EMSCRIPTEN OR NOT WITH_GHOST_WEB)
  message(FATAL_ERROR "The web platform requires Emscripten and WITH_GHOST_WEB")
endif()

set(PLATFORM_CFLAGS "-pthread -msimd128 -fwasm-exceptions -funsigned-char -fno-strict-aliasing")
set(PLATFORM_LINKFLAGS "-pthread -msimd128 -fwasm-exceptions")
add_link_options(-pthread -msimd128 -fwasm-exceptions)
add_compile_definitions(WITH_GHOST_WEB)
include_directories("${CMAKE_SOURCE_DIR}/intern/ghost/web")
set(PLATFORM_ENV_BUILD "_BLENDER_WEB_BUILD_=1")
set(PTHREADS_LIBRARIES "-pthread")
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_program(PYTHON_EXECUTABLE python3 REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/dependencies.cmake")
include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES ${FREETYPE_INCLUDE_DIRS})
check_symbol_exists(FT_CONFIG_OPTION_USE_BROTLI "freetype/config/ftconfig.h" WEB_FREETYPE_HAS_BROTLI)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT WEB_FREETYPE_HAS_BROTLI)
  message(FATAL_ERROR "Blender's fonts require FreeType with Brotli/WOFF2 support")
endif()
if(WITH_TBB)
  get_target_property(TBB_INCLUDE_DIRS TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
  set(TBB_LIBRARIES TBB::tbb)
endif()

# No desktop GL loader is linked into the WebGPU build. If the OpenGL backend
# is explicitly requested, it still has to supply its actual dependency.
if(WITH_OPENGL_BACKEND)
  find_package(Epoxy REQUIRED)
endif()
add_definitions(-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE)
include("${CMAKE_CURRENT_LIST_DIR}/generators.cmake")
