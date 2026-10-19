/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstring>
#include <string>
#include <regex>
#include <set>
#include <vector>
#include <utility>
#include "shader_switch.hh"

/* WebGPU has no samplerBuffer. Preserve RGBA32 texels as read-only storage,
 * retaining the original float/int/uint bit interpretation without a copy. */
inline std::string webgpu_buffer_texture_declaration(const std::string &name,
                                                     const char *type,
                                                     int binding)
{
  return "layout(binding = " + std::to_string(binding) +
         ", std430) readonly buffer _web_buffer_texture_" + name + " { " + type +
         " web_buffer_texels_" + name + "[]; }; " + type + " web_buffer_fetch_" + name +
         "(int index) { return web_buffer_texels_" + name + "[index]; } int web_buffer_size_" +
         name + "() { return web_buffer_texels_" + name + ".length(); } ";
}

/* libc++'s unanchored regex search tries the expression at every source byte.
 * Generated shaders can be hundreds of KiB. Find the literal prefix first and
 * only run the expression at candidates, preserving replacement formatting. */
inline std::string webgpu_replace_prefixed(const std::string &source,
                                           const std::string &prefix,
                                           const std::regex &pattern,
                                           const char *replacement)
{
  std::string result;
  size_t copied = 0, cursor = 0;
  while ((cursor = source.find(prefix, cursor)) != std::string::npos) {
    std::match_results<std::string::const_iterator> match;
    if (std::regex_search(source.begin() + cursor, source.end(), match, pattern,
                          std::regex_constants::match_continuous)) {
      result.append(source, copied, cursor - copied);
      result += match.format(replacement);
      cursor += match.length();
      copied = cursor;
    }
    else cursor += prefix.size();
  }
  result.append(source, copied, source.size() - copied);
  return result;
}

/* WGSL storage atomics cannot also be read or copied as ordinary integers.
 * Adapt only the WebGPU shader input; retain Blender's original shader files. */
inline bool webgpu_adapt_draw_command(const std::string &source,
                                      const char *name,
                                      std::string &adapted, bool vertex = false, bool raw = false)
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
  adapted = webgpu_replace_prefixed(adapted, "defined(SRT_CONSTANT_use_clipping)",
      std::regex(R"(defined\(SRT_CONSTANT_use_clipping\)\s*\?\s*\(SRT_CONSTANT_use_clipping == 1\)\s*:\s*(?:\\\s*)?defined\(USE_WORLD_CLIP_PLANES\))"),
      "((defined(SRT_CONSTANT_use_clipping) && (SRT_CONSTANT_use_clipping == 1)) || "
      "(!defined(SRT_CONSTANT_use_clipping) && defined(USE_WORLD_CLIP_PLANES)))");
  /* WGSL forbids NaN literals. These vertices are deliberately discarded, so
   * place them outside the homogeneous clip volume instead. */
  replace("float4(NAN_FLT)", "float4(2.0, 2.0, 2.0, 1.0)");
  adapted = webgpu_replace_prefixed(adapted, "#",
      std::regex(R"((#[ \t]*define[ \t]+ATTR_FALLTHROUGH)[^\n]*)"), "$1 web_fallthrough_marker");
  if (adapted.find("GBUFFER_LAYER_MAX") != std::string::npos) {
    size_t line = 0;
    const std::regex directive(R"([ \t]*#[ \t]*define[ \t]+GBUFFER_LAYER_MAX\b)");
    while (line < adapted.size()) {
      size_t first = adapted.find_first_not_of(" \t", line);
      if (first != std::string::npos && adapted[first] == '#' &&
          std::regex_search(adapted.cbegin() + line, adapted.cend(), directive,
                            std::regex_constants::match_continuous)) {
        const std::string undef = "#undef GBUFFER_LAYER_MAX\n";
        adapted.insert(line, undef);
        line += undef.size();
      }
      line = adapted.find('\n', line);
      if (line == std::string::npos) break;
      line++;
    }
  }
  if (raw) return true;
  const std::regex buffer_texture(R"(buffer\s+_web_buffer_texture_(\w+)\s*\{)");
  std::vector<std::string> buffer_names;
  for (size_t cursor = 0; (cursor = adapted.find("buffer", cursor)) != std::string::npos; cursor += 6) {
    std::match_results<std::string::const_iterator> match;
    if (std::regex_search(adapted.cbegin() + cursor, adapted.cend(), match, buffer_texture,
                          std::regex_constants::match_continuous)) buffer_names.push_back(match[1]);
  }
  for (const std::string &resource : buffer_names) {
    const std::string fetch = "web_buffer_fetch_" + resource + "(";
    adapted = webgpu_replace_prefixed(adapted, "texelFetch",
        std::regex("texelFetch\\s*\\(\\s*" + resource + "\\s*,"), fetch.c_str());
    const std::string size = "web_buffer_size_" + resource + "()";
    adapted = webgpu_replace_prefixed(adapted, "textureSize",
        std::regex("textureSize\\s*\\(\\s*" + resource + "\\s*\\)"), size.c_str());
  }
  for (const char *texture : {"object_id_tx", "object_id_buffer", "outline_id_tx"}) {
    replace(std::string("texture(") + texture + ",", std::string("wgpu_uint_texture(") + texture + ",");
    replace(std::string("textureLod(") + texture + ",", std::string("wgpu_uint_texture_lod(") + texture + ",");
  }
  // Lower atomic-image operations by resource identity, rather than by render
  // engine or shader name. Sampling uses the same buffer, avoiding atlas copies.
  std::regex atomic_resource(R"(buffer\s+_web_atomic_(\w+)\s*\{)");
  std::vector<std::string> atomic_names;
  for (size_t cursor = 0; (cursor = adapted.find("buffer", cursor)) != std::string::npos; cursor += 6) {
    std::match_results<std::string::const_iterator> match;
    if (std::regex_search(adapted.cbegin() + cursor, adapted.cend(), match, atomic_resource,
                          std::regex_constants::match_continuous)) atomic_names.push_back(match[1]);
  }
  for (const auto &resource : atomic_names) {
    if (resource.rfind("sample_", 0) == 0) {
      const std::string name = resource.substr(7);
      replace("texelFetch(" + name + ",", "web_fetch_" + name + "(");
    }
    else {
      for (const auto &operation : {std::pair{"imageAtomicMin", "min"}, {"imageAtomicAdd", "add"}})
        replace(std::string(operation.first) + "(" + resource + ",", std::string("web_") + operation.second + "_" + resource + "(");
      replace("imageStore(" + resource + ",", "web_store_" + resource + "(");
      replace("imageLoad(" + resource + ",", "web_load_" + resource + "(");
    }
  }
  if (vertex) {
    adapted = webgpu_replace_prefixed(adapted, "layout(",
        std::regex(R"((layout\([^)]*\)\s*)(?:(?:readonly|writeonly|restrict)\s+)*buffer\s)"),
        "$1readonly buffer ");
  }
  webgpu_lower_fallthrough(adapted);
  /* A tilemap is identical for the whole workgroup. Load it once, then
   * broadcast through shared memory so WGSL can prove barrier uniformity. */
  if (std::strcmp(name, "eevee_shadow_tilemap_init") == 0 ||
      std::strcmp(name, "eevee_shadow_page_mask") == 0 ||
      std::strcmp(name, "eevee_shadow_tilemap_finalize") == 0) {
    std::regex load(R"(ShadowTileMapData\s+(\w+)\s*=\s*([^;\n]+)\[tilemap_index\];)");
    adapted = std::regex_replace(adapted, load,
        "if (gl_LocalInvocationIndex == 0u) web_uniform_tilemap = $2[tilemap_index]; "
        "barrier(); ShadowTileMapData $1 = web_uniform_tilemap; barrier();");
    // The type definition comes from Blender's shared shader header.
    std::smatch definition;
    if (std::regex_search(adapted, definition, std::regex(R"(struct\s+ShadowTileMapData\s*\{)"))) {
      size_t end = adapted.find("};", size_t(definition.position()));
      adapted.insert(end + 2, "\nshared ShadowTileMapData web_uniform_tilemap;\n");
    }
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

/* Rewrite 1D sampler usage for the 1D->2D texture promotion (see the texture
 * descriptor in webgpu_texture.cc). Collects every identifier declared with a
 * sampler1D/sampler1DArray type (uniforms AND function parameters), rewrites
 * texture/textureLod/texelFetch/textureSize calls whose first argument is such
 * a name to the wgpu_*1d helper overloads (defined in webgpu_glsl_patch), then
 * retypes the declarations to sampler2D/sampler2DArray. */
inline void webgpu_promote_1d_samplers(std::string &src)
{
  if (src.find("sampler1D") == std::string::npos) {
    return;
  }
  auto is_ident = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };
  /* Pass 1: collect names typed sampler1D / sampler1DArray. */
  std::set<std::string> plain, arr;
  for (size_t pos = src.find("sampler1D"); pos != std::string::npos;
       pos = src.find("sampler1D", pos + 1))
  {
    if (pos > 0 && is_ident(src[pos - 1])) {
      continue; /* e.g. usampler1D (unhandled; none in practice) or mid-word. */
    }
    size_t p = pos + strlen("sampler1D");
    const bool is_array = (src.compare(p, 5, "Array") == 0);
    if (is_array) {
      p += 5;
    }
    if (is_ident(src[p])) {
      continue; /* Longer identifier, not this type. */
    }
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\n')) {
      p++;
    }
    size_t e = p;
    while (e < src.size() && is_ident(src[e])) {
      e++;
    }
    if (e > p) {
      (is_array ? arr : plain).insert(src.substr(p, e - p));
    }
  }
  /* Pass 2: rewrite call sites (before retyping, order irrelevant). */
  static const struct {
    const char *fn;
    const char *plain_repl;
    const char *arr_repl;
  } kFns[] = {
      {"texture", "wgpu_tex1d", "wgpu_tex1darr"},
      {"textureLod", "wgpu_texlod1d", "wgpu_texlod1darr"},
      {"texelFetch", "wgpu_fetch1d", "wgpu_fetch1darr"},
      {"textureSize", "wgpu_size1d", "wgpu_size1darr"},
  };
  for (const auto &f : kFns) {
    const size_t fn_len = strlen(f.fn);
    std::string out;
    out.reserve(src.size());
    size_t last = 0;
    for (size_t pos = src.find(f.fn); pos != std::string::npos; pos = src.find(f.fn, pos + 1)) {
      if ((pos > 0 && is_ident(src[pos - 1])) || is_ident(src[pos + fn_len])) {
        continue; /* textureLod while scanning texture, textureSize, ... */
      }
      size_t p = pos + fn_len;
      while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) {
        p++;
      }
      if (p >= src.size() || src[p] != '(') {
        continue;
      }
      p++;
      while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) {
        p++;
      }
      size_t e = p;
      while (e < src.size() && is_ident(src[e])) {
        e++;
      }
      size_t c = e;
      while (c < src.size() && (src[c] == ' ' || src[c] == '\t')) {
        c++;
      }
      if (c >= src.size() || src[c] != ',' || e == p) {
        continue;
      }
      const std::string name = src.substr(p, e - p);
      const bool in_plain = plain.count(name) != 0;
      const bool in_arr = !in_plain && arr.count(name) != 0;
      if (!in_plain && !in_arr) {
        continue;
      }
      out.append(src, last, pos - last);
      out.append(in_plain ? f.plain_repl : f.arr_repl);
      last = pos + fn_len;
    }
    if (last != 0) {
      out.append(src, last, std::string::npos);
      src = std::move(out);
    }
  }
  /* Pass 3: retype the declarations. */
  auto replace_all_str = [](std::string &s, const char *from, const char *to) {
    const size_t n = strlen(from);
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + 1)) {
      s.replace(pos, n, to);
    }
  };
  replace_all_str(src, "sampler1DArray", "sampler2DArray");
  replace_all_str(src, "sampler1D", "sampler2D");
}
