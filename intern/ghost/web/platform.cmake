# SPDX-License-Identifier: GPL-2.0-or-later
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "WITH_GHOST_WEB requires Emscripten")
endif()
add_definitions(-DWITH_GHOST_WEB)
list(APPEND SRC web/GHOST_SystemWeb.cc web/GHOST_WindowWeb.cc)
list(APPEND INC .)
