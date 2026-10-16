# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.22)
project(blender_web_platform LANGUAGES C CXX)
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "Configure with emcmake. See web/README.md.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(WEB_THREADS "Build a pthread-enabled Wasm module" ON)
option(WEB_SIMD "Enable baseline WebAssembly SIMD" ON)
if(NOT BLENDER_ROOT)
  get_filename_component(BLENDER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()
set(GHOST "${BLENDER_ROOT}/intern/ghost")
set(MEM "${BLENDER_ROOT}/intern/guardedalloc")

# Compile upstream platform, logging and allocator code, without replacement stubs.
# This is deliberately a GHOST test target, not the full Blender executable.
add_executable(ghost_web
  "${GHOST}/intern/GHOST_Buttons.cc"
  "${GHOST}/intern/GHOST_CallbackEventConsumer.cc"
  "${GHOST}/intern/GHOST_Context.cc"
  "${GHOST}/intern/GHOST_ContextNone.cc"
  "${GHOST}/intern/GHOST_EventManager.cc"
  "${GHOST}/intern/GHOST_ISystem.cc"
  "${GHOST}/intern/GHOST_ModifierKeys.cc"
  "${GHOST}/intern/GHOST_Rect.cc"
  "${GHOST}/intern/GHOST_System.cc"
  "${GHOST}/intern/GHOST_TimerManager.cc"
  "${GHOST}/intern/GHOST_Window.cc"
  "${GHOST}/intern/GHOST_WindowManager.cc"
  "${GHOST}/web/GHOST_SystemWeb.cc"
  "${GHOST}/web/GHOST_WindowWeb.cc"
  "${BLENDER_ROOT}/intern/clog/clog.cc"
  "${MEM}/intern/leak_detector.cc"
  "${MEM}/intern/mallocn.cc"
  "${MEM}/intern/mallocn_guarded_impl.cc"
  "${MEM}/intern/mallocn_lockfree_impl.cc"
  "${MEM}/intern/memory_usage.cc"
  platform_test.cc
)
target_include_directories(ghost_web PRIVATE
  "${GHOST}" "${MEM}" "${BLENDER_ROOT}/intern/atomic"
  "${BLENDER_ROOT}/intern/clog" "${BLENDER_ROOT}/source/blender/blenlib"
)
target_compile_definitions(ghost_web PRIVATE WITH_GHOST_WEB)
target_compile_options(ghost_web PRIVATE -O3 -flto)
target_link_options(ghost_web PRIVATE
  -O3 -flto
  -sMODULARIZE=1 -sEXPORT_NAME=createGhostWeb -sENVIRONMENT=web,worker
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sMAXIMUM_MEMORY=1073741824
  -sSTACK_SIZE=1048576 -sFORCE_FILESYSTEM=1 -lidbfs.js
  "-sEXPORTED_RUNTIME_METHODS=['ccall','FS','IDBFS','addRunDependency','removeRunDependency']"
  -sALLOW_BLOCKING_ON_MAIN_THREAD=0
)
if(WEB_THREADS)
  target_compile_definitions(ghost_web PRIVATE WITH_CLOG_PTHREADS)
  target_compile_options(ghost_web PRIVATE -pthread)
  target_link_options(ghost_web PRIVATE -pthread
    "-sPTHREAD_POOL_SIZE=Module['webWorkerCount']"
    -sPTHREAD_POOL_SIZE_STRICT=2 -sDEFAULT_PTHREAD_STACK_SIZE=524288
  )
endif()
if(WEB_SIMD)
  target_compile_options(ghost_web PRIVATE -msimd128)
  target_link_options(ghost_web PRIVATE -msimd128)
endif()
set_target_properties(ghost_web PROPERTIES SUFFIX ".js")
