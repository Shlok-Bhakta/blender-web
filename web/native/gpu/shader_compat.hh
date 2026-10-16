/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstring>
#include <string>
#include <regex>

/* WGSL storage atomics cannot also be read or copied as ordinary integers.
 * Adapt only the WebGPU shader input; retain Blender's original shader files. */
inline bool webgpu_adapt_draw_command(const std::string &source,
                                      const char *name,
                                      std::string &adapted)
{
  adapted = source;
  auto replace = [&](const std::string &from, const std::string &to) {
    size_t position = 0;
    while ((position = adapted.find(from, position)) != std::string::npos) {
      adapted.replace(position, from.size(), to);
      position += to.size();
    }
  };
  /* GLSL preprocessor expressions do not support C's conditional operator. */
  adapted = std::regex_replace(adapted,
      std::regex(R"(defined\(SRT_CONSTANT_use_clipping\)\s*\?\s*\(SRT_CONSTANT_use_clipping == 1\)\s*:\s*(?:\\\s*)?defined\(USE_WORLD_CLIP_PLANES\))"),
      "((defined(SRT_CONSTANT_use_clipping) && (SRT_CONSTANT_use_clipping == 1)) || "
      "(!defined(SRT_CONSTANT_use_clipping) && defined(USE_WORLD_CLIP_PLANES)))");
  /* WGSL forbids NaN literals. These vertices are deliberately discarded, so
   * place them outside the homogeneous clip volume instead. */
  replace("float4(NAN_FLT)", "float4(2.0, 2.0, 2.0, 1.0)");
  for (const char *texture : {"object_id_tx", "object_id_buffer", "outline_id_tx"}) {
    replace(std::string("texture(") + texture + ",", std::string("wgpu_uint_texture(") + texture + ",");
    replace(std::string("textureLod(") + texture + ",", std::string("wgpu_uint_texture_lod(") + texture + ",");
  }
  if (std::strcmp(name, "draw_command_generate") != 0) return adapted != source;
  replace("DrawGroup group = group_buf[group_id];",
          "DrawGroup group;\n"
          "group.next = group_buf[group_id].next;\n"
          "group.start = group_buf[group_id].start;\n"
          "group.len = group_buf[group_id].len;\n"
          "group.front_facing_len = group_buf[group_id].front_facing_len;\n"
          "group.vertex_len = group_buf[group_id].vertex_len;\n"
          "group.vertex_first = group_buf[group_id].vertex_first;\n"
          "group.base_index = group_buf[group_id].base_index;");
  for (const char *field : {"front_facing_counter", "back_facing_counter", "total_counter"}) {
    const std::string expression = std::string("group_buf[group_id].") + field;
    replace(expression + " = 0u;", "atomicExchange(" + expression + ", 0u);");
    replace("= " + expression + ";", "= atomicAdd(" + expression + ", 0u);");
  }
  return true;
}
