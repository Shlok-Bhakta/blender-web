# SPDX-License-Identifier: GPL-2.0-or-later
option(BLENDER_WEB_JSPI "Enable GPU render waits using JavaScript Promise Integration" ON)
set(BLENDER_WEB_LINK_OPTIMIZATION "3" CACHE STRING "Final Wasm optimization level")
option(BLENDER_WEB_DEBUG_NAMES "Retain C++ function names in the downloadable Wasm" OFF)
option(BLENDER_WEB_BINARYEN_INLINING "Run the additional whole-module Wasm inliner" OFF)
function(blender_web_configure_application)
  if(NOT TARGET blender)
    return()
  endif()
  set(gpu_deps "${CMAKE_BINARY_DIR}/../gpu-deps/build")
  file(GLOB tint_libs "${gpu_deps}/tint/src/tint/libtint*.a")
  file(GLOB absl_libs "${gpu_deps}/tint/third_party/abseil/absl/*/libabsl*.a")
  target_link_libraries(bf_gpu PUBLIC
    -Wl,--whole-archive ${tint_libs}
    "${gpu_deps}/spirv-tools/source/opt/libSPIRV-Tools-opt.a"
    "${gpu_deps}/spirv-tools/source/libSPIRV-Tools.a"
    -Wl,--no-whole-archive
    "${CMAKE_PREFIX_PATH}/lib/libshaderc_combined.a"
    -Wl,--start-group ${absl_libs} -Wl,--end-group
  )
  include("${CMAKE_PREFIX_PATH}/lib/blender-web-numpy/libraries.cmake")
  target_include_directories(blender PRIVATE "${CMAKE_PREFIX_PATH}/include" ${PYTHON_INCLUDE_DIRS})
  target_link_libraries(blender PRIVATE
    -Wl,--start-group ${BLENDER_WEB_NUMPY_LIBRARIES} -Wl,--end-group)
  if(WITH_USD)
    include("${CMAKE_PREFIX_PATH}/lib/blender-web-usd/libraries.cmake")
    target_link_libraries(blender PRIVATE
      -Wl,--start-group ${BLENDER_WEB_USD_PYTHON_LIBRARIES} -Wl,--end-group)
  endif()
  set_property(TARGET blender APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_BINARY_DIR}/../assets.sha256"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shader-cache.js")
  if(BLENDER_WEB_JSPI)
    target_link_options(blender PRIVATE -sJSPI=1
      "-sJSPI_EXPORTS=['main','blender_web_run_gpu_jobs']")
  endif()
  if(NOT BLENDER_WEB_BINARYEN_INLINING)
    # USD's static bindings make Binaryen 125's extra inliner exceed host RAM.
    # Original C++ kernels still receive Clang's full release optimization.
    target_link_options(blender PRIVATE
      "-sBINARYEN_EXTRA_PASSES=--skip-pass=inlining-optimizing")
  endif()
  if(BLENDER_WEB_DEBUG_NAMES)
    target_link_options(blender PRIVATE -g2)
  else()
    # Preserve readable JS without shipping hundreds of MiB of C++ names.
    target_link_options(blender PRIVATE -g1)
  endif()
  target_link_options(blender PRIVATE
    --use-port=emdawnwebgpu -sUSE_BZIP2=1
    "-O${BLENDER_WEB_LINK_OPTIMIZATION}"
    "--pre-js=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shader-cache.js"
    -sPROXY_TO_PTHREAD=1 -sOFFSCREENCANVAS_SUPPORT=1
    "-sPTHREAD_POOL_SIZE=Math.max(4, Math.min(64, (navigator.hardwareConcurrency || 4) + 2))" -sPTHREAD_POOL_SIZE_STRICT=0
    -sDEFAULT_PTHREAD_STACK_SIZE=4194304 -sSTACK_SIZE=16777216
    -sWASMFS=1 -sFORCE_FILESYSTEM=1
    -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=536870912 -sMAXIMUM_MEMORY=4294967296
    -sASSERTIONS=1
    "-sEXPORTED_RUNTIME_METHODS=['FS','callMain','ccall','cwrap','ENV']"
    "--preload-file=${CMAKE_BINARY_DIR}/../assets/5.2@/5.2"
    "--preload-file=${CMAKE_BINARY_DIR}/../assets/python3.13@/5.2/python/lib/python3.13"
  )
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL blender_web_configure_application)
