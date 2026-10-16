# SPDX-License-Identifier: GPL-2.0-or-later
# Execute the target-architecture generators on the build host using Node.
# In particular, makesdna must see Wasm's pointer sizes and struct alignment.
function(blender_web_configure_generators)
  foreach(generator datatoc makesdna makesrna shader_tool)
    if(TARGET ${generator})
      target_link_options(${generator} PRIVATE
        -sENVIRONMENT=node -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=8388608 -sEXIT_RUNTIME=1
      )
      set_property(TARGET ${generator} PROPERTY CXX_LINKER_LAUNCHER
        "${PYTHON_EXECUTABLE};${CMAKE_CURRENT_FUNCTION_LIST_DIR}/executable.py;${CMAKE_CROSSCOMPILING_EMULATOR}"
      )
    endif()
  endforeach()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL blender_web_configure_generators)
