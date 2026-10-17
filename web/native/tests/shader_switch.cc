/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../gpu/shader_switch.hh"
#include <iostream>
int main() {
  std::string body = R"(
int FUNCTION(int x, int y) {
  int value = 0;
  switch(x) {
    case 3: {
      if (y == 9) break;
      value += 100;
      web_fallthrough_marker;
    }
    case 2:
      switch(y) {
        case 3: value += 30; web_fallthrough_marker;
        case 2: value += 20; web_fallthrough_marker;
        case 1: value += 10; break;
        default : value += 7; break;
      }
      web_fallthrough_marker;
    case 1: value += 1; break;
    default : value += 999; break;
  }
  return value;
}
)";
  std::string original = std::regex_replace(body,std::regex("web_fallthrough_marker;"),";");
  original = std::regex_replace(original,std::regex("FUNCTION"),"original");
  webgpu_lower_fallthrough(body);
  body = std::regex_replace(body,std::regex("FUNCTION"),"lowered");
  std::cout << "#include <cassert>\n#include <cstdio>\n" << original << body
      << "int main(){for(int x=-1;x<5;x++)for(int y=-1;y<11;y++)assert(original(x,y)==lowered(x,y)); puts(\"{\\\"switchCases\\\":72}\");}\n";
}
