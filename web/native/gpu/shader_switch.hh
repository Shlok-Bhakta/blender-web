/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cctype>
#include <regex>
#include <string>

/* Operates on preprocessed GLSL. Work from the last explicit fallthrough back
 * so nested switches and later fallthrough chains have already been lowered. */
inline void webgpu_lower_fallthrough(std::string &source)
{
  const std::string marker = "web_fallthrough_marker;";
  while (true) {
    const size_t position = source.rfind(marker);
    if (position == std::string::npos) break;
    size_t scope = std::string::npos, finish = 0;
    std::regex switches(R"(\bswitch\s*\()");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), switches);
         it != std::sregex_iterator(); ++it) {
      size_t begin = source.find('{', size_t(it->position()));
      if (begin > position) break;
      int depth = 1;
      size_t end = begin + 1;
      for (; end < source.size() && depth; end++) {
        if (source[end] == '{') depth++;
        if (source[end] == '}') depth--;
      }
      if (begin < position && position < end) { scope = begin; finish = end - 1; }
    }
    if (scope == std::string::npos) break;
    size_t body = std::string::npos, end = finish;
    int depth = 0;
    for (size_t cursor = scope + 1; cursor < finish; cursor++) {
      const char ch = source[cursor];
      if (ch == '{') { depth++; continue; }
      if (ch == '}') { depth--; continue; }
      if (depth || cursor <= position) continue;
      const bool label = source.compare(cursor, 5, "case ") == 0 ||
                         (source.compare(cursor, 7, "default") == 0 &&
                          !std::isalnum(static_cast<unsigned char>(source[cursor + 7])));
      if (!label) continue;
      if (body != std::string::npos) {
        // Adjacent empty labels share the same body.
        if (source.substr(body, cursor - body).find_first_not_of(" \t\r\n") != std::string::npos) { end = cursor; break; }
      }
      body = source.find(':', cursor) + 1;
      cursor = body - 1;
    }
    if (body == std::string::npos) break;
    const std::string replacement = "{\n" + source.substr(body, end - body) + "\nbreak;\n}";
    source.replace(position, marker.size(), replacement);
  }
}
