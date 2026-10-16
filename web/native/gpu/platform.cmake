# SPDX-License-Identifier: GPL-2.0-or-later
add_compile_definitions(WITH_WEBGPU_BACKEND)
file(GLOB WEBGPU_SRC "${CMAKE_SOURCE_DIR}/intern/ghost/web/native/gpu/*.cc")
list(APPEND SRC ${WEBGPU_SRC})
list(APPEND INC "${CMAKE_SOURCE_DIR}/intern/ghost/web/native/gpu")
list(APPEND INC_SYS
  "${CMAKE_PREFIX_PATH}/include"
  "${CMAKE_BINARY_DIR}/../gpu-deps/src/dawn"
  "${CMAKE_BINARY_DIR}/../gpu-deps/build/tint"
)
add_compile_options(--use-port=emdawnwebgpu)
