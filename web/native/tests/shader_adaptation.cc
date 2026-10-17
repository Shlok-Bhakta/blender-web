/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../gpu/shader_compat.hh"
#include <cassert>
#include <cstdio>

int main()
{
  const std::string padding = "\n" + std::string(100000, 'x') + "\n";
  struct Case { const char *source, *expected; bool vertex; };
  const Case cases[] = {
    {"#define ATTR_FALLTHROUGH [[fallthrough]]\n",
     "#define ATTR_FALLTHROUGH web_fallthrough_marker\n", false},
    {"#define GBUFFER_LAYER_MAX 6\n#define GBUFFER_LAYER_MAX 7\n",
     "#undef GBUFFER_LAYER_MAX\n#define GBUFFER_LAYER_MAX 6\n#undef GBUFFER_LAYER_MAX\n#define GBUFFER_LAYER_MAX 7\n", false},
    {" \t# define GBUFFER_LAYER_MAX 8\n#define GBUFFER_LAYER_MAX_SUFFIX 9\n",
     "#undef GBUFFER_LAYER_MAX\n \t# define GBUFFER_LAYER_MAX 8\n#define GBUFFER_LAYER_MAX_SUFFIX 9\n", false},
    {"float4(NAN_FLT)", "float4(2.0, 2.0, 2.0, 1.0)", false},
    {"layout(binding=1) restrict buffer Example {int x;};",
     "layout(binding=1) readonly buffer Example {int x;};", true},
    {"layout(binding=1) restrict buffer Example {int x;};",
     "layout(binding=1) restrict buffer Example {int x;};", false},
    {"buffer _web_atomic_atlas {int x;}; imageAtomicAdd(atlas, p, 1); imageLoad(atlas, p);",
     "buffer _web_atomic_atlas {int x;}; web_add_atlas( p, 1); web_load_atlas( p);", false},
    {"buffer _web_atomic_sample_atlas {int x;}; texelFetch(atlas, p, 0);",
     "buffer _web_atomic_sample_atlas {int x;}; web_fetch_atlas( p, 0);", false},
    {"#if defined(SRT_CONSTANT_use_clipping) ? (SRT_CONSTANT_use_clipping == 1) : \\\n defined(USE_WORLD_CLIP_PLANES)\n#endif",
     "#if ((defined(SRT_CONSTANT_use_clipping) && (SRT_CONSTANT_use_clipping == 1)) || (!defined(SRT_CONSTANT_use_clipping) && defined(USE_WORLD_CLIP_PLANES)))\n#endif", false},
  };
  int checks=0;
  for (const auto &test : cases) {
    for (bool padded : {false, true}) {
      std::string source = padded ? padding + test.source + padding : test.source;
      std::string expected = padded ? padding + test.expected + padding : test.expected;
      std::string adapted;
      webgpu_adapt_draw_command(source,"regression",adapted,test.vertex);
      assert(adapted == expected);
      checks++;
    }
  }
  printf("{\"shaderAdaptationCases\":%d}\n", checks);
}
