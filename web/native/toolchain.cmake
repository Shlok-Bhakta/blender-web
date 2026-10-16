# SPDX-License-Identifier: GPL-2.0-or-later
find_program(WEB_EMCC emcc REQUIRED)
get_filename_component(WEB_EMSCRIPTEN_ROOT "${WEB_EMCC}" DIRECTORY)
# The SDK reports x86 to accommodate old configure scripts. Modern dependencies
# otherwise add x86 assembly and -mfpmath=sse, neither of which targets Wasm.
set(EMSCRIPTEN_SYSTEM_PROCESSOR wasm32)
include("${WEB_EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
