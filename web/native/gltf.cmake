# SPDX-License-Identifier: GPL-2.0-or-later
function(blender_web_add_gltf_bridge target sources)
  add_library(${target} STATIC ${sources})
  if(target STREQUAL "bf_intern_draco_bridge")
    target_compile_options(${target} PRIVATE
      "SHELL:-include \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ghost/GHOST_WebDracoTypes.hh\"")
  endif()
endfunction()
