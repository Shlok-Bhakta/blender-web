# SPDX-License-Identifier: GPL-2.0-or-later
function(blender_web_add_gltf_bridge target sources)
  add_library(${target} STATIC ${sources})
endfunction()
