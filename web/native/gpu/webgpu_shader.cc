/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <set>

#ifdef __EMSCRIPTEN__
#  include <emscripten/em_js.h>
#  include <emscripten/emscripten.h>
#endif

#include <shaderc/shaderc.hpp>

#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/utils/ice/ice.h"

#include "gpu_shader_dependency_private.hh"

#include "GPU_context.hh"

#include "webgpu_context.hh"
#include "webgpu_shader.hh"
#include "shader_compat.hh"
#include "webgpu_shader_interface.hh"

namespace blender::gpu {

/* Create a WGPUShaderModule from WGSL on the active device, or null if there is
 * no device (degraded headless mode) or the source is empty. */
static WGPUShaderModule webgpu_module_from_wgsl(const std::string &wgsl)
{
  if (wgsl.empty()) {
    return nullptr;
  }
  Context *ctx = Context::get();
  if (ctx == nullptr) {
    return nullptr;
  }
  WGPUDevice device = static_cast<WebGPUContext *>(ctx)->device();
  if (device == nullptr) {
    return nullptr;
  }
  WGPUShaderSourceWGSL src = {};
  src.chain.sType = WGPUSType_ShaderSourceWGSL;
  src.code = {wgsl.c_str(), wgsl.size()};
  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &src.chain;
  return wgpuDeviceCreateShaderModule(device, &desc);
}

using namespace shader;

/* --- GLSL declaration helpers (verbatim from vk_shader.cc) --- */
static const char *to_string(const Interpolation &interp)
{
  switch (interp) {
    case Interpolation::SMOOTH:
      return "smooth";
    case Interpolation::FLAT:
      return "flat";
    case Interpolation::NO_PERSPECTIVE:
      return "noperspective";
    default:
      return "unknown";
  }
}

static const char *to_string(const Type &type)
{
  switch (type) {
    case Type::float_t:
      return "float";
    case Type::float2_t:
      return "vec2";
    case Type::float3_t:
      return "vec3";
    case Type::float4_t:
      return "vec4";
    case Type::float3x3_t:
      return "mat3";
    case Type::float4x4_t:
      return "mat4";
    case Type::uint_t:
      return "uint";
    case Type::uint2_t:
      return "uvec2";
    case Type::uint3_t:
      return "uvec3";
    case Type::uint4_t:
      return "uvec4";
    case Type::int_t:
      return "int";
    case Type::int2_t:
      return "ivec2";
    case Type::int3_t:
      return "ivec3";
    case Type::int4_t:
      return "ivec4";
    case Type::bool_t:
      return "bool";
    default:
      return "unknown";
  }
}

static const char *to_string(const TextureFormat &type)
{
  switch (type) {
    case TextureFormat::UINT_8_8_8_8:
      return "rgba8ui";
    case TextureFormat::SINT_8_8_8_8:
      return "rgba8i";
    case TextureFormat::UNORM_8_8_8_8:
      return "rgba8";
    case TextureFormat::UINT_32_32_32_32:
      return "rgba32ui";
    case TextureFormat::SINT_32_32_32_32:
      return "rgba32i";
    case TextureFormat::SFLOAT_32_32_32_32:
      return "rgba32f";
    case TextureFormat::UINT_16_16_16_16:
      return "rgba16ui";
    case TextureFormat::SINT_16_16_16_16:
      return "rgba16i";
    case TextureFormat::SFLOAT_16_16_16_16:
      return "rgba16f";
    case TextureFormat::UNORM_16_16_16_16:
      return "rgba16";
    case TextureFormat::UINT_8_8:
      return "rg8ui";
    case TextureFormat::SINT_8_8:
      return "rg8i";
    case TextureFormat::UNORM_8_8:
      return "rg8";
    case TextureFormat::UINT_32_32:
      return "rg32ui";
    case TextureFormat::SINT_32_32:
      return "rg32i";
    case TextureFormat::SFLOAT_32_32:
      return "rg32f";
    case TextureFormat::UINT_16_16:
      return "rg16ui";
    case TextureFormat::SINT_16_16:
      return "rg16i";
    case TextureFormat::SFLOAT_16_16:
      return "rg16f";
    case TextureFormat::UNORM_16_16:
      return "rg16";
    case TextureFormat::UINT_8:
      return "r8ui";
    case TextureFormat::SINT_8:
      return "r8i";
    case TextureFormat::UNORM_8:
      return "r8";
    case TextureFormat::UINT_32:
      return "r32ui";
    case TextureFormat::SINT_32:
      return "r32i";
    case TextureFormat::SFLOAT_32:
      return "r32f";
    case TextureFormat::UINT_16:
      return "r16ui";
    case TextureFormat::SINT_16:
      return "r16i";
    case TextureFormat::SFLOAT_16:
      return "r16f";
    case TextureFormat::UNORM_16:
      return "r16";
    case TextureFormat::UFLOAT_11_11_10:
      return "r11f_g11f_b10f";
    case TextureFormat::UNORM_10_10_10_2:
      return "rgb10_a2";
    default:
      return "unknown";
  }
}

static void print_image_type(std::ostream &os,
                             const ImageType &type,
                             const ShaderCreateInfo::Resource::BindType bind_type)
{
  switch (type) {
    case ImageType::IntBuffer:
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Int3D:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicInt3D:
      os << "i";
      break;
    case ImageType::UintBuffer:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Uint3D:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint2DArray:
    case ImageType::AtomicUint3D:
      os << "u";
      break;
    default:
      break;
  }

  if (bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
    os << "image";
  }
  else {
    os << "sampler";
  }

  switch (type) {
    case ImageType::FloatBuffer:
    case ImageType::IntBuffer:
    case ImageType::UintBuffer:
      os << "Buffer";
      break;
    case ImageType::Float1D:
    case ImageType::Float1DArray:
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
      os << "1D";
      break;
    case ImageType::Float2D:
    case ImageType::Float2DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Shadow2D:
    case ImageType::Shadow2DArray:
    case ImageType::Depth2D:
    case ImageType::Depth2DArray:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint2DArray:
      os << "2D";
      break;
    case ImageType::Float3D:
    case ImageType::Int3D:
    case ImageType::AtomicInt3D:
    case ImageType::Uint3D:
    case ImageType::AtomicUint3D:
      os << "3D";
      break;
    case ImageType::FloatCube:
    case ImageType::FloatCubeArray:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
    case ImageType::ShadowCube:
    case ImageType::ShadowCubeArray:
    case ImageType::DepthCube:
    case ImageType::DepthCubeArray:
      os << "Cube";
      break;
    default:
      break;
  }

  switch (type) {
    case ImageType::Float1DArray:
    case ImageType::Float2DArray:
    case ImageType::FloatCubeArray:
    case ImageType::Int1DArray:
    case ImageType::Int2DArray:
    case ImageType::IntCubeArray:
    case ImageType::Uint1DArray:
    case ImageType::Uint2DArray:
    case ImageType::UintCubeArray:
    case ImageType::Shadow2DArray:
    case ImageType::ShadowCubeArray:
    case ImageType::Depth2DArray:
    case ImageType::DepthCubeArray:
    case ImageType::AtomicUint2DArray:
      os << "Array";
      break;
    default:
      break;
  }

  switch (type) {
    case ImageType::Shadow2D:
    case ImageType::Shadow2DArray:
    case ImageType::ShadowCube:
    case ImageType::ShadowCubeArray:
      os << "Shadow";
      break;
    default:
      break;
  }
  os << " ";
}

static std::ostream &print_qualifier(std::ostream &os, const Qualifier &qualifiers)
{
  /* NOTE: 'restrict' omitted — it emits SPIR-V Restrict decoration (19) which
   * Tint's SPIR-V reader does not handle (TINT_UNIMPLEMENTED). It is only an
   * aliasing hint, safe to drop for the WGSL path. */
  if (!flag_is_set(qualifiers, Qualifier::read)) {
    os << "writeonly ";
  }
  if (!flag_is_set(qualifiers, Qualifier::write)) {
    os << "readonly ";
  }
  return os;
}

inline int get_location_count(const Type &type)
{
  if (type == shader::Type::float4x4_t) {
    return 4;
  }
  if (type == shader::Type::float3x3_t) {
    return 3;
  }
  return 1;
}

static void print_interface_as_attributes(std::ostream &os,
                                          const std::string &prefix,
                                          const StageInterfaceInfo &iface,
                                          int &location)
{
  for (const StageInterfaceInfo::InOut &inout : iface.inouts) {
    os << "layout(location=" << location << ") " << prefix << " " << to_string(inout.interp) << " "
       << to_string(inout.type) << " " << inout.name << ";\n";
    location += get_location_count(inout.type);
  }
}

static void print_interface_as_struct(std::ostream &os,
                                      const std::string &prefix,
                                      const StageInterfaceInfo &iface,
                                      int &location,
                                      const StringRefNull &suffix)
{
  std::string struct_name = prefix + iface.name;
  Interpolation qualifier = iface.inouts[0].interp;

  os << "struct " << struct_name << " {\n";
  for (const StageInterfaceInfo::InOut &inout : iface.inouts) {
    os << "  " << to_string(inout.type) << " " << inout.name << ";\n";
  }
  os << "};\n";
  os << "layout(location=" << location << ") " << prefix << " " << to_string(qualifier) << " "
     << struct_name << " " << iface.instance_name << suffix << ";\n";

  for (const StageInterfaceInfo::InOut &inout : iface.inouts) {
    location += get_location_count(inout.type);
  }
}

static void print_interface(std::ostream &os,
                            const std::string &prefix,
                            const StageInterfaceInfo &iface,
                            int &location,
                            const StringRefNull &suffix = "")
{
  if (iface.instance_name.is_empty()) {
    print_interface_as_attributes(os, prefix, iface, location);
  }
  else {
    print_interface_as_struct(os, prefix, iface, location, suffix);
  }
}

/* Build the per-stage version patch: #version + extensions + compat macros,
 * by resolving Blender's compat GLSL with our extension header injected. Mirrors
 * VKDevice::glsl_*_patch_get. Targets Vulkan-flavoured GLSL (set/binding/location)
 * which shaderc compiles to SPIR-V suitable for Tint. */
static std::string webgpu_glsl_patch(const char *stage_define)
{
  std::stringstream ss;
  ss << "#version 450\n";
  ss << stage_define;
  /* NOTE: do NOT enable GL_ARB_shader_draw_parameters — it emits the
   * SPV_KHR_shader_draw_parameters SPIR-V extension which Tint's reader rejects.
   * WebGPU has no base-instance, so gpu_BaseInstance is 0. */
  ss << "uvec4 wgpu_uint_texture_lod(usampler2D t, vec2 uv, float lod) {"
        " ivec2 sz = textureSize(t, int(lod));"
        " return texelFetch(t, clamp(ivec2(floor(uv * vec2(sz))), ivec2(0), sz - 1), int(lod)); }\n";
  ss << "uvec4 wgpu_uint_texture(usampler2D t, vec2 uv) { return wgpu_uint_texture_lod(t, uv, 0.0); }\n";
  ss << "#define gpu_BaseInstance 0\n";
  ss << "#define GPU_WEBGPU\n";
  ss << "#define GPU_ARB_clip_control\n";
  ss << "#define GPU_ARB_derivative_control\n";
  /* Under point->quad expansion (spec constant 242, pipeline-time) the vertex
   * index is the QUAD CORNER and the instance index is the POINT index —
   * shaders that fetch per-point data with gl_VertexID (overlay_extra_point
   * SSBO indexing) keep working because the constant folds at compile. */
  ss << "#define gl_VertexID ((gpu_point_expand > 0.5) ? gl_InstanceIndex : gl_VertexIndex)\n";
  ss << "#define gpu_InstanceIndex (gl_InstanceIndex)\n";
  ss << "#define gl_InstanceID gl_InstanceIndex\n";
  /* WebGPU has no multi-viewport / gl_ViewportIndex. EEVEE's shadow geometry
   * shaders ASSIGN a viewport index; route it into a mutable private variable so
   * the write compiles. The viewport transform itself is emulated in the shader
   * source (see the GPU_WEBGPU block in eevee_geom_*.bsl.hh — all shadow
   * viewports anchor at (0,0), so it reduces to an NDC scale). */
  ss << "int gpu_viewport_index_var = 0;\n";
  ss << "#define gpu_ViewportIndex gpu_viewport_index_var\n";
  /* DEBUG bisect toggle: ENV.WGPU_PLAIN_STORE=1 restores the pre-min-emulation
   * blind shadow-atlas store (eevee_surf_shadow.bsl.hh). */
  if (getenv("WGPU_PLAIN_STORE")) {
    ss << "#define WGPU_PLAIN_STORE_TEST\n";
  }
  /* Tint's SPIR-V reader does not implement OpIsNan/OpIsInf — replace isnan/isinf
   * with manual expressions so glslang never emits those instructions. Use
   * type-overloaded helper functions (not a bare macro) so that VECTOR arguments
   * yield a bvec — a scalar `!(v==v)` would collapse to a single bool and break
   * `any(isnan(vec2))` (the EEVEE velocity/film shaders). `not(equal(v,v))`
   * yields true per-NaN-component via OpFOrdEqual (false for NaN) + negate. */
  ss << "bool gpu_isnan(float x) { return !(x == x); }\n";
  ss << "bvec2 gpu_isnan(vec2 v) { return not(equal(v, v)); }\n";
  ss << "bvec3 gpu_isnan(vec3 v) { return not(equal(v, v)); }\n";
  ss << "bvec4 gpu_isnan(vec4 v) { return not(equal(v, v)); }\n";
  ss << "bool gpu_isinf(float x) { return abs(x) > 3.402823466e38; }\n";
  ss << "bvec2 gpu_isinf(vec2 v) { return greaterThan(abs(v), vec2(3.402823466e38)); }\n";
  ss << "bvec3 gpu_isinf(vec3 v) { return greaterThan(abs(v), vec3(3.402823466e38)); }\n";
  ss << "bvec4 gpu_isinf(vec4 v) { return greaterThan(abs(v), vec4(3.402823466e38)); }\n";
  ss << "#define isnan(x) gpu_isnan(x)\n";
  ss << "#define isinf(x) gpu_isinf(x)\n";
  /* WebGPU points are always 1px: WGSL has no PointSize output (Tint rejects
   * non-constant stores to it) and no PointCoord input. Route gl_PointSize
   * into a dead private variable so point shaders COMPILE (vertices render as
   * 1px dots — proper point-sprite emulation would expand points to quads),
   * and pin gl_PointCoord to the point center so radius-discard round-point
   * fragments keep their center texel. */
  ss << "float gpu_point_size_var = 1.0;\n";
  ss << "#define gl_PointSize gpu_point_size_var\n";
  ss << "#ifdef GPU_VERTEX_SHADER\n";
  ss << "layout(constant_id = 240) const float gpu_viewport_w = 1024.0;\n";
  ss << "layout(constant_id = 241) const float gpu_viewport_h = 1024.0;\n";
  ss << "layout(constant_id = 242) const float gpu_point_expand = 0.0;\n";
  ss << "#endif\n";
  /* When the fragment stage actually reads gl_PointCoord AND the draw goes
   * through point->quad expansion, ensure_translated() defines
   * GPU_POINTCOORD_VARYING in both stages: the expansion wrapper then feeds
   * REAL per-corner UVs through location 15 (round points, keyframe shapes).
   * Otherwise gl_PointCoord stays pinned to the point center. */
  ss << "#if defined(GPU_POINTCOORD_VARYING) && defined(GPU_FRAGMENT_SHADER)\n";
  ss << "layout(location = 15) in vec2 gpu_pointcoord_var;\n";
  ss << "#define gl_PointCoord gpu_pointcoord_var\n";
  ss << "#else\n";
  ss << "#define gl_PointCoord vec2(0.5)\n";
  ss << "#endif\n";
  /* 1D textures are PROMOTED to 2D height-1 textures (WGSL has no 1D array
   * textures, and Tint mis-reads plain-1D textureDimensions). Call sites are
   * rewritten to these helpers by promote_1d_samplers(). */
  ss << "vec4 wgpu_tex1d(sampler2D s, float x) { return texture(s, vec2(x, 0.5)); }\n";
  ss << "vec4 wgpu_texlod1d(sampler2D s, float x, float lod) {"
        " return textureLod(s, vec2(x, 0.5), lod); }\n";
  ss << "vec4 wgpu_fetch1d(sampler2D s, int x, int lod) {"
        " return texelFetch(s, ivec2(x, 0), lod); }\n";
  ss << "int wgpu_size1d(sampler2D s, int lod) { return textureSize(s, lod).x; }\n";
  ss << "vec4 wgpu_tex1darr(sampler2DArray s, vec2 co) {"
        " return texture(s, vec3(co.x, 0.5, co.y)); }\n";
  ss << "vec4 wgpu_texlod1darr(sampler2DArray s, vec2 co, float lod) {"
        " return textureLod(s, vec3(co.x, 0.5, co.y), lod); }\n";
  ss << "vec4 wgpu_fetch1darr(sampler2DArray s, ivec2 co, int lod) {"
        " return texelFetch(s, ivec3(co.x, 0, co.y), lod); }\n";
  ss << "ivec2 wgpu_size1darr(sampler2DArray s, int lod) {"
        " return textureSize(s, lod).xz; }\n";

  GeneratedSourceList sources;
  GeneratedSource ext;
  ext.filename = "gpu_shader_glsl_extension.glsl";
  ext.content = ss.str();
  sources.append(std::move(ext));

  Vector<StringRefNull> resolved = gpu_shader_dependency_get_resolved_source(
      "gpu_shader_compat_glsl.glsl", sources);
  std::string out;
  for (StringRefNull s : resolved) {
    out.append(s.c_str(), s.size());
  }
  return out;
}

/* GLSL -> SPIR-V (shaderc) -> WGSL (Tint). Returns true + sets r_wgsl on success.
 * SPIR-V is targeted at Vulkan 1.0 (Tint rejects the SPIR-V 1.5 that vulkan_1.2
 * would emit). This is the validated toolchain (see smoke/wgsl_probe.cc). */
#ifdef __EMSCRIPTEN__
/* ---- Session-persistent WGSL translation cache ----
 * GLSL -> SPIR-V -> WGSL costs 200-600 ms PER SHADER on the main thread; the
 * deferred-translation scheme (translate at first draw) turns that into
 * interaction hitches the first time any shader variant is used. The page
 * (blender-gui.html) preloads previously translated WGSL from IndexedDB into
 * globalThis.__WGSL_CACHE__ before main() runs; lookups here are synchronous.
 * Keys: "v1:<stage>:<fnv1a64 of the joined GLSL>". The version salt must be
 * bumped whenever the translator or the GLSL patching changes output. */
EM_JS(int, wgpu_wgsl_cache_query, (const char *key), {
  const m = globalThis.__WGSL_CACHE__;
  if (!m) {
    return -1;
  }
  const v = m.get(UTF8ToString(key));
  if (v === undefined) {
    return -1;
  }
  globalThis.__WGSL_CACHE_HIT__ = v;
  return lengthBytesUTF8(v);
});
EM_JS(void, wgpu_wgsl_cache_fetch, (char *buf, int buf_len), {
  stringToUTF8(globalThis.__WGSL_CACHE_HIT__, buf, buf_len);
  globalThis.__WGSL_CACHE_HIT__ = undefined;
});
EM_JS(void, wgpu_wgsl_cache_put, (const char *key, const char *val), {
  let m = globalThis.__WGSL_CACHE__;
  if (!m) {
    m = globalThis.__WGSL_CACHE__ = new Map();
  }
  const k = UTF8ToString(key);
  const v = UTF8ToString(val);
  m.set(k, v);
  if (globalThis.__WGSL_CACHE_PUT__) {
    globalThis.__WGSL_CACHE_PUT__(k, v);
  }
});
EM_JS_DEPS(wgpu_wgsl_cache, "$UTF8ToString,$lengthBytesUTF8,$stringToUTF8");

static std::string wgsl_cache_key(const char *stage, const std::string &glsl)
{
  uint64_t h = 1469598103934665603ull;
  for (const char c : glsl) {
    h = (h ^ uint8_t(c)) * 1099511628211ull;
  }
  char key[64];
  snprintf(key, sizeof(key), "v2:%s:%016llx:%zu", stage, (unsigned long long)h, glsl.size());
  return key;
}

/* Returns true and fills r_wgsl on a cache hit. */
static bool wgsl_cache_get(const char *stage, const std::string &glsl, std::string &r_wgsl)
{
  const std::string key = wgsl_cache_key(stage, glsl);
  const int len = wgpu_wgsl_cache_query(key.c_str());
  if (len < 0) {
    return false;
  }
  /* +1: stringToUTF8 needs room for the NUL it writes. */
  std::vector<char> tmp(size_t(len) + 1);
  wgpu_wgsl_cache_fetch(tmp.data(), len + 1);
  r_wgsl.assign(tmp.data(), size_t(len));
  return true;
}

static void wgsl_cache_store(const char *stage, const std::string &glsl, const std::string &wgsl)
{
  if (wgsl.empty()) {
    return;
  }
  wgpu_wgsl_cache_put(wgsl_cache_key(stage, glsl).c_str(), wgsl.c_str());
}
#endif /* __EMSCRIPTEN__ */

static bool glsl_to_wgsl(const std::string &glsl,
                         shaderc_shader_kind kind,
                         const char *name,
                         std::string &r_wgsl)
{
  if (const char *gpat = getenv("WGPU_DUMP_GLSL_STDERR")) {
    if (strstr(name, gpat) != nullptr) {
      fprintf(stderr, "GLSL_DUMP %s kind=%d\n%s\nGLSL_DUMP_END\n", name, int(kind), glsl.c_str());
      fflush(stderr);
    }
  }
  std::string adapted;
  const std::string &compiler_source = webgpu_adapt_draw_command(glsl, name, adapted) ? adapted : glsl;
  shaderc::Compiler compiler;
  shaderc::CompileOptions opts;
  opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
  shaderc::SpvCompilationResult spv = compiler.CompileGlslToSpv(compiler_source, kind, name, opts);
  if (spv.GetCompilationStatus() != shaderc_compilation_status_success) {
    fprintf(stderr, "WGPU_SH shaderc FAIL %s: %s\n", name, spv.GetErrorMessage().c_str());
    if (getenv("WGPU_DUMP_GLSL")) {
      char path[256];
      snprintf(path, sizeof(path), "/tmp/wgpu_glsl_%s_%d.glsl", name, int(kind));
      FILE *f = fopen(path, "w");
      if (f) { fwrite(glsl.data(), 1, glsl.size(), f); fclose(f); }
    }
    fflush(stderr);
    return false;
  }
  std::vector<uint32_t> words(spv.cbegin(), spv.cend());
  /* Unsupported shaders must unwind compiler-owned C++ objects. Longjmp
   * across these frames corrupts ownership under Wasm exceptions. */
  struct RecoveryScope {
    RecoveryScope() { tint::internal_compiler_error_recovery = true; }
    ~RecoveryScope() { tint::internal_compiler_error_recovery = false; }
  } recovery;
  try {
  /* Tint splits SPIR-V combined image-samplers into a WGSL texture + sampler.
   * With no explicit mapping it resolves the sampler's binding by incrementing
   * until unique — a PER-MODULE outcome, so the same sampler can land on
   * different slots in the vertex vs fragment WGSL (and a slot can mean
   * "texture" in one stage and "sampler" in the other, which is impossible to
   * express in one bind group layout). Pin samplers deterministically instead:
   * the sampler for the combined resource at @binding(N) always goes to
   * @binding(N + 128). Blender's binding slots stay well below 128 and the
   * device's maxBindingsPerBindGroup (>=1000) accommodates the offset. */
  tint::spirv::reader::Options ropts;
  for (uint32_t i = 0; i < 128; i++) {
    ropts.sampler_mappings.insert({tint::BindingPoint{0, i}, tint::BindingPoint{0, i + 128}});
  }
  auto ir = tint::spirv::reader::ReadIR(words, ropts);
  if (ir != tint::Success) {
    tint::internal_compiler_error_recovery = false;
    fprintf(stderr, "WGPU_SH tint-read FAIL %s: %s\n", name, ir.Failure().reason.c_str());
    fflush(stderr);
    return false;
  }
  /* Permissive WGSL emission: EEVEE uses derivatives in non-uniform control flow
   * (fwidth), has provably-unreachable branches after early-outs, and relies on
   * extensions/features gated off by default (dual-source blending, read-write
   * storage textures). Allow them all here so the WGSL is produced; the browser
   * device must in turn expose the matching WebGPU features at module use. */
  tint::wgsl::writer::Options wopts;
  wopts.allow_non_uniform_derivatives = true;
  wopts.allow_non_uniform_subgroup_operations = true;
  wopts.disable_unreachable_code_warning = true;
  wopts.allowed_features = tint::wgsl::AllowedFeatures::Everything();
  auto out = tint::wgsl::writer::WgslFromIR(ir.Get(), wopts);
  tint::internal_compiler_error_recovery = false;
  if (out != tint::Success) {
    fprintf(stderr, "WGPU_SH tint-write FAIL %s: %s\n", name, out.Failure().reason.c_str());
    fflush(stderr);
    return false;
  }
  r_wgsl = out.Get().wgsl;
  if (getenv("WGPU_DUMP_WGSL")) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/wgpu_wgsl_%s_%d.wgsl", name, int(kind));
    FILE *f = fopen(path, "w");
    if (f) { fwrite(r_wgsl.data(), 1, r_wgsl.size(), f); fclose(f); }
  }
  return true;
  } catch (const tint::RecoverableCompilerError &) {
    fprintf(stderr, "WGPU_SH unsupported Tint shader '%s'\n", name);
    return false;
  }
}

/* Recover the Blender resource name from a WGSL buffer struct type name:
 * strip a leading '_' (block prefix), a trailing '_atomic' (Tint's atomics
 * transform clones structs with atomic members under that suffix), and a
 * trailing '_<digits>' (Tint dedup). */
static std::string strip_buffer_typename(std::string t)
{
  if (!t.empty() && t[0] == '_') {
    t.erase(0, 1);
  }
  size_t u = t.rfind('_');
  if (u != std::string::npos && u + 1 < t.size()) {
    bool all_digits = true;
    for (size_t i = u + 1; i < t.size(); i++) {
      if (!isdigit((unsigned char)t[i])) {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      t.erase(u);
    }
  }
  const std::string atomic_sfx = "_atomic";
  if (t.size() > atomic_sfx.size() &&
      t.compare(t.size() - atomic_sfx.size(), atomic_sfx.size(), atomic_sfx) == 0)
  {
    t.erase(t.size() - atomic_sfx.size());
  }
  return t;
}

static std::string trim_ws(const std::string &s)
{
  size_t a = s.find_first_not_of(" \t\n\r");
  size_t b = s.find_last_not_of(" \t\n\r");
  return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static bool has(const std::string &s, const char *sub)
{
  return s.find(sub) != std::string::npos;
}

static WGPUTextureViewDimension wgsl_view_dim(const std::string &type)
{
  if (has(type, "cube_array")) {
    return WGPUTextureViewDimension_CubeArray;
  }
  if (has(type, "cube")) {
    return WGPUTextureViewDimension_Cube;
  }
  if (has(type, "2d_array")) {
    return WGPUTextureViewDimension_2DArray;
  }
  if (has(type, "_3d")) {
    return WGPUTextureViewDimension_3D;
  }
  if (has(type, "_1d")) {
    return WGPUTextureViewDimension_1D;
  }
  return WGPUTextureViewDimension_2D;
}

static WGPUTextureSampleType wgsl_sample_type(const std::string &type)
{
  if (has(type, "depth")) {
    return WGPUTextureSampleType_Depth;
  }
  if (has(type, "<u32>")) {
    return WGPUTextureSampleType_Uint;
  }
  if (has(type, "<i32>")) {
    return WGPUTextureSampleType_Sint;
  }
  return WGPUTextureSampleType_Float;
}

static WGPUTextureFormat wgsl_storage_format(const std::string &type)
{
  /* Token between '<' and ',' (or '>'). */
  size_t lt = type.find('<');
  if (lt == std::string::npos) {
    return WGPUTextureFormat_RGBA8Unorm;
  }
  size_t end = type.find_first_of(",>", lt + 1);
  std::string f = trim_ws(type.substr(lt + 1, end - lt - 1));
  struct {
    const char *name;
    WGPUTextureFormat fmt;
  } table[] = {
      {"rgba8unorm", WGPUTextureFormat_RGBA8Unorm},
      {"rgba8snorm", WGPUTextureFormat_RGBA8Snorm},
      {"rgba8uint", WGPUTextureFormat_RGBA8Uint},
      {"rgba8sint", WGPUTextureFormat_RGBA8Sint},
      {"rgba16unorm", WGPUTextureFormat_RGBA16Unorm},
      {"rgba16snorm", WGPUTextureFormat_RGBA16Snorm},
      {"rgba16uint", WGPUTextureFormat_RGBA16Uint},
      {"rgba16sint", WGPUTextureFormat_RGBA16Sint},
      {"rgba16float", WGPUTextureFormat_RGBA16Float},
      {"r8unorm", WGPUTextureFormat_R8Unorm},
      {"r8snorm", WGPUTextureFormat_R8Snorm},
      {"r8uint", WGPUTextureFormat_R8Uint},
      {"r8sint", WGPUTextureFormat_R8Sint},
      {"rg8unorm", WGPUTextureFormat_RG8Unorm},
      {"rg8snorm", WGPUTextureFormat_RG8Snorm},
      {"rg8uint", WGPUTextureFormat_RG8Uint},
      {"rg8sint", WGPUTextureFormat_RG8Sint},
      {"r16unorm", WGPUTextureFormat_R16Unorm},
      {"r16snorm", WGPUTextureFormat_R16Snorm},
      {"r16uint", WGPUTextureFormat_R16Uint},
      {"r16sint", WGPUTextureFormat_R16Sint},
      {"r16float", WGPUTextureFormat_R16Float},
      {"rg16unorm", WGPUTextureFormat_RG16Unorm},
      {"rg16snorm", WGPUTextureFormat_RG16Snorm},
      {"rg16uint", WGPUTextureFormat_RG16Uint},
      {"rg16sint", WGPUTextureFormat_RG16Sint},
      {"rg16float", WGPUTextureFormat_RG16Float},
      {"r32uint", WGPUTextureFormat_R32Uint},
      {"r32sint", WGPUTextureFormat_R32Sint},
      {"r32float", WGPUTextureFormat_R32Float},
      {"rg32uint", WGPUTextureFormat_RG32Uint},
      {"rg32sint", WGPUTextureFormat_RG32Sint},
      {"rg32float", WGPUTextureFormat_RG32Float},
      {"rgba32uint", WGPUTextureFormat_RGBA32Uint},
      {"rgba32sint", WGPUTextureFormat_RGBA32Sint},
      {"rgba32float", WGPUTextureFormat_RGBA32Float},
      {"rgb10a2unorm", WGPUTextureFormat_RGB10A2Unorm},
      {"rgb10a2uint", WGPUTextureFormat_RGB10A2Uint},
      {"rg11b10ufloat", WGPUTextureFormat_RG11B10Ufloat},
      {"bgra8unorm", WGPUTextureFormat_BGRA8Unorm},
  };
  for (auto &e : table) {
    if (f == e.name) {
      return e.fmt;
    }
  }
  return WGPUTextureFormat_RGBA8Unorm;
}

/* Parse `@group(0u) @binding(Nu) var<...> name : type;` declarations from WGSL
 * and append them to `out`. WGSL is the source of truth for the pipeline's
 * expected bindings (Tint renumbers after splitting combined image-samplers). */
static void parse_wgsl_bindings(const std::string &wgsl, std::vector<WgslResourceBinding> &out)
{
  size_t pos = 0;
  const std::string key = "@binding(";
  while ((pos = wgsl.find(key, pos)) != std::string::npos) {
    size_t np = pos + key.size();
    uint32_t binding = 0;
    bool got = false;
    while (np < wgsl.size() && isdigit((unsigned char)wgsl[np])) {
      binding = binding * 10 + (wgsl[np] - '0');
      np++;
      got = true;
    }
    pos = np;
    if (!got) {
      continue;
    }
    size_t var_pos = wgsl.find("var", np);
    size_t semi = wgsl.find(';', np);
    if (var_pos == std::string::npos || semi == std::string::npos || var_pos > semi) {
      continue;
    }
    std::string decl = wgsl.substr(var_pos, semi - var_pos); /* "var<...> name : type" */
    size_t colon = decl.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string type = trim_ws(decl.substr(colon + 1));
    /* var name = token between the var<...> header and ':'. */
    size_t name_start = 3; /* after "var" */
    if (decl.size() > 3 && decl[3] == '<') {
      size_t close = decl.find('>');
      name_start = (close == std::string::npos) ? 3 : close + 1;
    }
    std::string varname = trim_ws(decl.substr(name_start, colon - name_start));

    WgslResourceBinding b;
    b.binding = binding;
    if (decl.compare(0, 12, "var<uniform>") == 0) {
      b.buffer_type = WGPUBufferBindingType_Uniform;
      if (type.compare(0, 9, "constants") == 0) {
        b.kind = WgslResourceBinding::PUSH_CONST;
        b.res_name = "";
      }
      else {
        b.kind = WgslResourceBinding::UBO;
        b.res_name = strip_buffer_typename(type);
      }
    }
    else if (decl.compare(0, 11, "var<storage") == 0) {
      b.kind = WgslResourceBinding::SSBO;
      b.res_name = strip_buffer_typename(type);
      /* `var<storage, read>` = read-only; `read_write` (or bare) = writable. */
      const std::string head = decl.substr(0, decl.find('>'));
      if (has(head, "read_write")) {
        b.buffer_type = WGPUBufferBindingType_Storage;
        b.writable = true;
      }
      else if (has(head, "read")) {
        b.buffer_type = WGPUBufferBindingType_ReadOnlyStorage;
      }
      else {
        b.buffer_type = WGPUBufferBindingType_Storage;
        b.writable = true;
      }
    }
    else if (type.compare(0, 7, "sampler") == 0) {
      b.kind = WgslResourceBinding::SAMPLER;
      b.sampler_type = has(type, "comparison") ? WGPUSamplerBindingType_Comparison :
                                                 WGPUSamplerBindingType_Filtering;
      b.res_name = varname;
      if (b.res_name.size() > 8 && b.res_name.compare(b.res_name.size() - 8, 8, "_sampler") == 0) {
        b.res_name.erase(b.res_name.size() - 8);
      }
    }
    else if (type.compare(0, 15, "texture_storage") == 0) {
      b.kind = WgslResourceBinding::STORAGE_TEXTURE;
      b.view_dim = wgsl_view_dim(type);
      b.storage_format = wgsl_storage_format(type);
      b.storage_access = has(type, "read_write") ? WGPUStorageTextureAccess_ReadWrite :
                         has(type, "read")       ? WGPUStorageTextureAccess_ReadOnly :
                                                   WGPUStorageTextureAccess_WriteOnly;
      b.writable = (b.storage_access != WGPUStorageTextureAccess_ReadOnly);
      b.res_name = varname;
    }
    else if (type.compare(0, 8, "texture_") == 0) {
      b.kind = WgslResourceBinding::TEXTURE;
      b.tex_sample = wgsl_sample_type(type);
      b.view_dim = wgsl_view_dim(type);
      b.res_name = varname;
      if (b.res_name.size() > 6 && b.res_name.compare(b.res_name.size() - 6, 6, "_image") == 0) {
        b.res_name.erase(b.res_name.size() - 6);
      }
    }
    else {
      continue;
    }
    /* Dedup by binding (a resource used in vert+frag appears in both). */
    bool dup = false;
    for (const WgslResourceBinding &e : out) {
      if (e.binding == b.binding) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      out.push_back(b);
    }
  }
}

/* Bitmask of color locations the fragment entry point actually writes.
 * WebGPU rejects a pipeline whose color target has a non-zero writeMask but no
 * matching fragment output (EEVEE prepass shaders bind color targets they never
 * write). Tint emits either `fn main(...) -> @location(N) T` or a return struct
 * whose members carry @location(N); scan whichever form is present. */
static uint32_t parse_fragment_output_mask(const std::string &wgsl)
{
  uint32_t mask = 0;
  const size_t frag = wgsl.find("@fragment");
  if (frag == std::string::npos) {
    return 0;
  }
  const size_t fn = wgsl.find("fn ", frag);
  const size_t arrow = wgsl.find("->", fn == std::string::npos ? frag : fn);
  if (arrow == std::string::npos) {
    return 0;
  }
  const size_t body = wgsl.find('{', arrow);
  std::string ret = wgsl.substr(arrow + 2, (body == std::string::npos ? wgsl.size() : body) -
                                               arrow - 2);
  auto scan_locations = [&mask](const std::string &s) {
    const std::string key = "@location(";
    size_t p = 0;
    while ((p = s.find(key, p)) != std::string::npos) {
      p += key.size();
      uint32_t n = 0;
      bool got = false;
      while (p < s.size() && isdigit((unsigned char)s[p])) {
        n = n * 10 + (s[p] - '0');
        p++;
        got = true;
      }
      if (got && n < 32) {
        mask |= 1u << n;
      }
    }
  };
  if (ret.find("@location(") != std::string::npos) {
    scan_locations(ret);
    return mask;
  }
  /* Struct return: strip attributes/whitespace to get the type name, then scan
   * the struct body for member @location attributes. */
  std::string tname = trim_ws(ret);
  const size_t last_sp = tname.rfind(' ');
  if (last_sp != std::string::npos) {
    tname = tname.substr(last_sp + 1);
  }
  const size_t sdef = wgsl.find("struct " + tname);
  if (sdef == std::string::npos) {
    return 0;
  }
  const size_t open = wgsl.find('{', sdef);
  const size_t close = wgsl.find('}', open);
  if (open == std::string::npos || close == std::string::npos) {
    return 0;
  }
  scan_locations(wgsl.substr(open, close - open));
  return mask;
}

/* Vertex inputs (@location) the vertex entry point consumes. Tint emits them
 * as entry-point parameters: `fn main(..., @location(2u) nor : vec3<f32>, ...)`. */
static void parse_vertex_inputs(const std::string &wgsl, std::vector<WgslVertexInput> &out)
{
  out.clear();
  const size_t vert = wgsl.find("@vertex");
  if (vert == std::string::npos) {
    return;
  }
  const size_t fn = wgsl.find("fn ", vert);
  const size_t open = wgsl.find('(', fn == std::string::npos ? vert : fn);
  if (open == std::string::npos) {
    return;
  }
  size_t depth = 1, p = open + 1, close = std::string::npos;
  while (p < wgsl.size()) {
    if (wgsl[p] == '(') {
      depth++;
    }
    else if (wgsl[p] == ')') {
      if (--depth == 0) {
        close = p;
        break;
      }
    }
    p++;
  }
  if (close == std::string::npos) {
    return;
  }
  const std::string params = wgsl.substr(open + 1, close - open - 1);
  const std::string key = "@location(";
  size_t q = 0;
  while ((q = params.find(key, q)) != std::string::npos) {
    q += key.size();
    uint32_t loc = 0;
    bool got = false;
    while (q < params.size() && isdigit((unsigned char)params[q])) {
      loc = loc * 10 + (params[q] - '0');
      q++;
      got = true;
    }
    if (!got) {
      continue;
    }
    const size_t colon = params.find(':', q);
    if (colon == std::string::npos) {
      break;
    }
    const size_t end = params.find(',', colon);
    const std::string ty = params.substr(
        colon + 1, (end == std::string::npos ? params.size() : end) - colon - 1);
    int comps = 1;
    if (ty.find("vec2") != std::string::npos) {
      comps = 2;
    }
    else if (ty.find("vec3") != std::string::npos) {
      comps = 3;
    }
    else if (ty.find("vec4") != std::string::npos) {
      comps = 4;
    }
    WGPUVertexFormat fmt;
    if (ty.find("i32") != std::string::npos) {
      const WGPUVertexFormat t[4] = {WGPUVertexFormat_Sint32,
                                     WGPUVertexFormat_Sint32x2,
                                     WGPUVertexFormat_Sint32x3,
                                     WGPUVertexFormat_Sint32x4};
      fmt = t[comps - 1];
    }
    else if (ty.find("u32") != std::string::npos) {
      const WGPUVertexFormat t[4] = {WGPUVertexFormat_Uint32,
                                     WGPUVertexFormat_Uint32x2,
                                     WGPUVertexFormat_Uint32x3,
                                     WGPUVertexFormat_Uint32x4};
      fmt = t[comps - 1];
    }
    else {
      const WGPUVertexFormat t[4] = {WGPUVertexFormat_Float32,
                                     WGPUVertexFormat_Float32x2,
                                     WGPUVertexFormat_Float32x3,
                                     WGPUVertexFormat_Float32x4};
      fmt = t[comps - 1];
    }
    out.push_back({loc, fmt});
  }
}

static std::string join_sources(MutableSpan<StringRefNull> sources)
{
  std::string out;
  for (StringRefNull s : sources) {
    out.append(s.c_str(), s.size());
  }
  return out;
}

WebGPUShader::WebGPUShader(const char *name) : Shader(name) {}

WebGPUShader::~WebGPUShader()
{
  /* Bind groups are cached keyed on the shader pointer — a later allocation
   * could recycle it and hit stale groups with a different layout. */
  WebGPUContext::clear_bind_group_cache_all_contexts();
  if (vertex_module_) {
    wgpuShaderModuleRelease(vertex_module_);
  }
  if (fragment_module_) {
    wgpuShaderModuleRelease(fragment_module_);
  }
  if (compute_module_) {
    wgpuShaderModuleRelease(compute_module_);
  }
  if (pc_slice_buf_) {
    wgpuBufferRelease(pc_slice_buf_);
  }
  if (push_const_data_) {
    free(push_const_data_);
  }
}

/* Copy `bytes` from src to dst only when they differ; report whether anything
 * changed. Blender re-sets identical uniforms on most draws — treating those as
 * dirty would allocate a fresh arena slice (and miss the bind-group cache)
 * every draw for no reason. */
static bool copy_if_diff(uint8_t *dst, const uint8_t *src, size_t bytes)
{
  if (memcmp(dst, src, bytes) == 0) {
    return false;
  }
  memcpy(dst, src, bytes);
  return true;
}

/* Write a uniform value into the push-constant shadow at its std140 offset.
 * `location` is the byte offset of the member; ARRAY elements must be scattered
 * at std140 strides (16 B for scalar/vec2/vec3/vec4 elements, 48 B for mat3
 * with 16 B columns, 64 B for mat4) — the CPU-side data is tightly packed.
 * Returns true when the shadow actually CHANGED. */
static bool push_const_store(uint8_t *dst,
                             size_t cap,
                             int location,
                             int comp_len,
                             int array_size,
                             const void *data,
                             size_t elem = 4)
{
  const int n = array_size > 0 ? array_size : 1;
  const uint8_t *src = static_cast<const uint8_t *>(data);
  bool changed = false;
  if (comp_len == 9) {
    /* mat3: 3 columns of 3 floats, each column padded to 16 B (stride 48/elem). */
    if (size_t(location) + size_t(n) * 48 > cap) {
      return false;
    }
    for (int a = 0; a < n; a++) {
      for (int c = 0; c < 3; c++) {
        changed |= copy_if_diff(
            dst + location + a * 48 + c * 16, src + (a * 9 + c * 3) * elem, 3 * elem);
      }
    }
    return changed;
  }
  if (n == 1 || comp_len == 16) {
    /* Single value (any vec size) or mat4 array: tightly packed either way. */
    const size_t bytes = size_t(comp_len) * size_t(n) * elem;
    if (size_t(location) + bytes > cap) {
      return false;
    }
    return copy_if_diff(dst + location, src, bytes);
  }
  /* Array of scalars/vectors: 16 B element stride. */
  if (size_t(location) + size_t(n) * 16 > cap) {
    return false;
  }
  for (int a = 0; a < n; a++) {
    changed |= copy_if_diff(
        dst + location + a * 16, src + size_t(a) * comp_len * elem, size_t(comp_len) * elem);
  }
  return changed;
}

void WebGPUShader::uniform_float(int location, int comp_len, int array_size, const float *data)
{
  if (push_const_data_ == nullptr || location < 0 || data == nullptr) {
    return;
  }
  if (push_const_store(push_const_data_, push_const_size_, location, comp_len, array_size, data))
  {
    push_const_dirty_ = true;
  }
}

void WebGPUShader::uniform_int(int location, int comp_len, int array_size, const int *data)
{
  if (push_const_data_ == nullptr || location < 0 || data == nullptr) {
    return;
  }
  if (push_const_store(push_const_data_, push_const_size_, location, comp_len, array_size, data))
  {
    push_const_dirty_ = true;
  }
}

WGPUShaderModule WebGPUShader::vertex_module()
{
  ensure_translated();
  if (vertex_module_ == nullptr && !vertex_wgsl_.empty()) {
    vertex_module_ = webgpu_module_from_wgsl(vertex_wgsl_);
  }
  return vertex_module_;
}

WGPUShaderModule WebGPUShader::fragment_module()
{
  ensure_translated();
  if (fragment_module_ == nullptr && !fragment_wgsl_.empty()) {
    fragment_module_ = webgpu_module_from_wgsl(fragment_wgsl_);
  }
  return fragment_module_;
}

WGPUShaderModule WebGPUShader::compute_module()
{
  ensure_translated();
  if (compute_module_ == nullptr && !compute_wgsl_.empty()) {
    compute_module_ = webgpu_module_from_wgsl(compute_wgsl_);
  }
  return compute_module_;
}

uint64_t WebGPUShader::spec_hash() const
{
  const shader::SpecializationConstants &st = (!spec_state_.values.is_empty() || !constants) ?
                                                  spec_state_ :
                                                  *constants;
  uint64_t h = 1469598103934665603ull;
  for (const shader::SpecializationConstant::Value &v : st.values) {
    h = (h ^ uint64_t(v.u)) * 1099511628211ull;
  }
  return h;
}

void WebGPUShader::spec_entries(const std::string &wgsl, std::vector<WGPUConstantEntry> &out)
{
  const shader::SpecializationConstants &st = (!spec_state_.values.is_empty() || !constants) ?
                                                  spec_state_ :
                                                  *constants;
  /* Fill ALL key strings before taking any c_str() — growing the vector inside
   * the loop reallocates and dangles previously captured pointers (SSO strings
   * live inside the vector storage). */
  while (spec_keys_.size() < size_t(st.types.size())) {
    spec_keys_.push_back(std::to_string(spec_keys_.size()));
  }
  for (int i = 0; i < st.types.size(); i++) {
    /* Only override ids that survived into this stage's WGSL (unused spec
     * constants are stripped; Dawn errors on unmatched constant keys). */
    char tag[24];
    snprintf(tag, sizeof(tag), "@id(%d)", i);
    if (wgsl.find(tag) == std::string::npos) {
      continue;
    }
    WGPUConstantEntry e = {};
    e.key = {spec_keys_[i].c_str(), WGPU_STRLEN};
    switch (st.types[i]) {
      case shader::Type::int_t:
        e.value = double(st.values[i].i);
        break;
      case shader::Type::float_t:
        /* The GLSL preamble lowers float spec constants to a UINT spec constant
         * holding the bit pattern (uintBitsToFloat define) — pass the bits. */
      case shader::Type::uint_t:
      case shader::Type::bool_t:
      default:
        e.value = double(st.values[i].u);
        break;
    }
    out.push_back(e);
  }
  static int s_spec_log = 0;
  if (s_spec_log < 20 && !out.empty()) {
    s_spec_log++;
    fprintf(stderr, "WGPU_SPEC '%s':", name_get().c_str());
    for (const WGPUConstantEntry &e : out) {
      fprintf(stderr, " %s=%g", e.key.data, e.value);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
  }
}

bool WebGPUShader::push_const_slice(WGPUBuffer &r_buf, uint64_t &r_off, uint64_t &r_size)
{
  if (push_const_size_ == 0) {
    return false;
  }
  WebGPUContext *ctx = WebGPUContext::get();
  if (ctx == nullptr || ctx->device() == nullptr) {
    return false;
  }
  if (push_const_data_ == nullptr) {
    push_const_data_ = static_cast<uint8_t *>(calloc(1, push_const_size_));
    push_const_dirty_ = true;
  }
  /* A slice from a previous frame lives in reused arena space (the arena
   * resets every present) — it MUST be re-uploaded even when clean. */
  if (push_const_dirty_ || pc_slice_buf_ == nullptr ||
      pc_slice_epoch_ != ctx->uniform_arena_epoch())
  {
    WGPUBuffer buf = nullptr;
    uint64_t off = 0;
    if (!ctx->uniform_arena_alloc(push_const_size_, buf, off)) {
      return false;
    }
    wgpuQueueWriteBuffer(ctx->queue(), buf, off, push_const_data_, push_const_size_);
    wgpuBufferAddRef(buf);
    if (pc_slice_buf_) {
      wgpuBufferRelease(pc_slice_buf_);
    }
    pc_slice_buf_ = buf;
    pc_slice_off_ = off;
    pc_slice_epoch_ = ctx->uniform_arena_epoch();
    push_const_dirty_ = false;
  }
  r_buf = pc_slice_buf_;
  r_off = pc_slice_off_;
  r_size = push_const_size_;
  return true;
}

void WebGPUShader::init(const shader::ShaderCreateInfo & /*info*/, bool /*is_codegen_only*/) {}

void WebGPUShader::vertex_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                           MutableSpan<StringRefNull> sources)
{
  std::string patch = webgpu_glsl_patch("#define GPU_VERTEX_SHADER\n");
  /* Point-sprite expansion eligibility (see record_draw): the shader must set
   * a point size, and must not derive data from gl_VertexID (expansion turns
   * the vertex index into a quad-corner index). Scan the PRE-patch sources
   * (the patch itself mentions both tokens). */
  for (int i = int(!sources.is_empty()); i < sources.size(); i++) {
    const StringRefNull s = sources[i];
    if (strstr(s.c_str(), "gpu_point_expand")) {
      /* Our own generated wrapper (vertex_interface_declare) — its corner code
       * mentions gl_VertexID, which must not count as USER usage (it disabled
       * point expansion for every create-info shader). */
      continue;
    }
    if (!writes_point_size_ && strstr(s.c_str(), "gl_PointSize")) {
      writes_point_size_ = true;
    }
    if (!uses_vertex_id_ &&
        (strstr(s.c_str(), "gl_VertexID") || strstr(s.c_str(), "gl_VertexIndex")))
    {
      uses_vertex_id_ = true;
    }
  }
  if (!sources.is_empty()) {
    sources[SOURCES_INDEX_VERSION] = patch;
  }
  vertex_src_ = join_sources(sources);
}

void WebGPUShader::geometry_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                             MutableSpan<StringRefNull> sources)
{
  geometry_src_ = join_sources(sources);
}

void WebGPUShader::fragment_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                             MutableSpan<StringRefNull> sources)
{
  std::string patch = webgpu_glsl_patch("#define GPU_FRAGMENT_SHADER\n");
  for (int i = int(!sources.is_empty()); i < sources.size(); i++) {
    if (!uses_point_coord_ && strstr(sources[i].c_str(), "gl_PointCoord")) {
      uses_point_coord_ = true;
    }
  }
  if (!sources.is_empty()) {
    sources[SOURCES_INDEX_VERSION] = patch;
  }
  fragment_src_ = join_sources(sources);
}

void WebGPUShader::compute_shader_from_glsl(const shader::ShaderCreateInfo & /*info*/,
                                            MutableSpan<StringRefNull> sources)
{
  std::string patch = webgpu_glsl_patch("#define GPU_COMPUTE_SHADER\n");
  if (!sources.is_empty()) {
    sources[SOURCES_INDEX_VERSION] = patch;
  }
  compute_src_ = join_sources(sources);
}

/* Rewrite 1D sampler usage for the 1D->2D texture promotion (see the texture
 * descriptor in webgpu_texture.cc). Collects every identifier declared with a
 * sampler1D/sampler1DArray type (uniforms AND function parameters), rewrites
 * texture/textureLod/texelFetch/textureSize calls whose first argument is such
 * a name to the wgpu_*1d helper overloads (defined in webgpu_glsl_patch), then
 * retypes the declarations to sampler2D/sampler2DArray. */
static void promote_1d_samplers(std::string &src)
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

/* Translate all present stages GLSL -> SPIR-V -> WGSL and parse the WGSL
 * metadata (bindings, outputs, vertex inputs). DEFERRED to first use: Blender
 * batch-precompiles whole shader modules (opening the Add menu compiled 174
 * overlay shaders = a 21 s main-thread freeze); translating at first DRAW means
 * the never-drawn variants never pay. */
void WebGPUShader::ensure_translated()
{
  if (translate_attempted_) {
    return;
  }
  translate_attempted_ = true;

  const bool is_compute = !compute_src_.empty();
  bool vert_ok = true, frag_ok = true, comp_ok = true;

#ifdef __EMSCRIPTEN__
  const double t0_ms = emscripten_get_now();
#endif

  if (!is_compute) {
    /* Interstage validity: a fragment input must be fed by the vertex stage,
     * so the point-coord varying is enabled in both sources or neither. */
    if (uses_point_coord_ && !vertex_src_.empty() && !fragment_src_.empty()) {
      const char *anchor = "#version 450\n";
      const size_t vp = vertex_src_.find(anchor);
      const size_t fp = fragment_src_.find(anchor);
      if (vp != std::string::npos && fp != std::string::npos) {
        const size_t alen = strlen(anchor);
        vertex_src_.insert(vp + alen, "#define GPU_POINTCOORD_VARYING\n");
        fragment_src_.insert(fp + alen, "#define GPU_POINTCOORD_VARYING\n");
      }
    }
    if (!vertex_src_.empty()) {
      promote_1d_samplers(vertex_src_);
      if (!wgsl_cache_get("v", vertex_src_, vertex_wgsl_)) {
        vert_ok = glsl_to_wgsl(
            vertex_src_, shaderc_vertex_shader, name_get().c_str(), vertex_wgsl_);
        if (vert_ok) {
          wgsl_cache_store("v", vertex_src_, vertex_wgsl_);
        }
      }
    }
    if (!fragment_src_.empty()) {
      promote_1d_samplers(fragment_src_);
      if (!wgsl_cache_get("f", fragment_src_, fragment_wgsl_)) {
        frag_ok = glsl_to_wgsl(
            fragment_src_, shaderc_fragment_shader, name_get().c_str(), fragment_wgsl_);
        if (frag_ok) {
          wgsl_cache_store("f", fragment_src_, fragment_wgsl_);
        }
      }
    }
  }
  else {
    promote_1d_samplers(compute_src_);
    if (!wgsl_cache_get("c", compute_src_, compute_wgsl_)) {
      comp_ok = glsl_to_wgsl(
          compute_src_, shaderc_compute_shader, name_get().c_str(), compute_wgsl_);
      if (comp_ok) {
        wgsl_cache_store("c", compute_src_, compute_wgsl_);
      }
    }
  }
  const bool translated = is_compute ? comp_ok : (vert_ok && frag_ok);

  /* Debug aid: WGPU_DUMP_WGSL_STDERR=<substring> dumps a shader's generated
   * WGSL to stderr (readable from the browser console log, unlike the wasm FS). */
  if (const char *pat = getenv("WGPU_DUMP_WGSL_STDERR")) {
    if (strstr(name_get().c_str(), pat) != nullptr) {
      fprintf(stderr,
              "WGSL_DUMP %s\n--- vertex ---\n%s\n--- fragment ---\n%s\n--- compute ---\n%s\n"
              "WGSL_DUMP_END\n",
              name_get().c_str(),
              vertex_wgsl_.c_str(),
              fragment_wgsl_.c_str(),
              compute_wgsl_.c_str());
      fflush(stderr);
    }
  }

  if (translated) {
    parse_wgsl_bindings(vertex_wgsl_, render_bindings_);
    parse_wgsl_bindings(fragment_wgsl_, render_bindings_);
    parse_wgsl_bindings(compute_wgsl_, compute_bindings_);
    frag_output_mask_ = parse_fragment_output_mask(fragment_wgsl_);
    parse_vertex_inputs(vertex_wgsl_, vertex_inputs_);
  }

#ifdef __EMSCRIPTEN__
  const double dt_ms = emscripten_get_now() - t0_ms;
  if (dt_ms > 30.0) {
    fprintf(stderr, "WGPU_SHADER_MS '%s' %.0fms\n", name_get().c_str(), dt_ms);
    fflush(stderr);
  }
#endif
  if (!translated) {
    fprintf(stderr, "WGPU_SHADER translate FAILED '%s'\n", name_get().c_str());
    fflush(stderr);
  }
}

bool WebGPUShader::finalize(const shader::ShaderCreateInfo *info)
{
  /* Build the ShaderInterface from the create-info and ALWAYS report success —
   * translation and WGSL parsing are DEFERRED to first draw/dispatch
   * (ensure_translated). Rationale for unconditional success: EEVEE gates the
   * WHOLE render on every `needed_shaders` group being loaded
   * (Instance::render_frame: `skip_render_ |= !is_loaded(needed_shaders)`), so a
   * single failed shader would make it draw NOTHING; the draw path skips any
   * shader whose module is null instead. */
  if (info != nullptr && this->interface == nullptr) {
    WebGPUShaderInterface *iface = new WebGPUShaderInterface();
    iface->init(*info);
    this->interface = iface;
  }

  /* Size the push-constant buffer from the create-info (conservative std140
   * upper bound: 16 B per scalar/vec, 64 B per mat4, 48 B per mat3, ×array). */
  if (info != nullptr && !info->push_constants_.is_empty()) {
    size_t sz = 0;
    for (const shader::ShaderCreateInfo::PushConst &pc : info->push_constants_) {
      const int n = (pc.array_size > 0) ? pc.array_size : 1;
      size_t elem = 16;
      if (pc.type == Type::float4x4_t) {
        elem = 64;
      }
      else if (pc.type == Type::float3x3_t) {
        elem = 48;
      }
      sz += elem * size_t(n);
    }
    push_const_size_ = (sz + 15) & ~size_t(15);
    /* Allocate the CPU shadow now so uniform_float/int can write before the GPU
     * buffer is lazily created at first draw. */
    push_const_data_ = static_cast<uint8_t *>(calloc(1, push_const_size_));
  }

  valid_ = (this->interface != nullptr);
  return valid_;
}


/* --- WebGPUShader GLSL preamble generators (Vulkan-flavoured GLSL; sequential
 *     set=0 bindings — refine to real bind-group layout when wiring pipelines) --- */

std::string WebGPUShader::resources_declare(const shader::ShaderCreateInfo &info) const
{
  std::stringstream ss;
  /* Specialization constants. */
  uint constant_id = 0;
  for (const SpecializationConstant &sc : info.specialization_constants_) {
    ss << "layout (constant_id=" << constant_id++ << ") const ";
    switch (sc.type) {
      case Type::int_t: ss << "int " << sc.name << "=" << std::to_string(sc.value.i) << ";\n"; break;
      case Type::uint_t: ss << "uint " << sc.name << "=" << std::to_string(sc.value.u) << "u;\n"; break;
      case Type::bool_t: ss << "bool " << sc.name << "=" << (sc.value.u ? "true" : "false") << ";\n"; break;
      case Type::float_t:
        ss << "uint " << sc.name << "_uint=" << std::to_string(sc.value.u) << "u;\n";
        ss << "#define " << sc.name << " uintBitsToFloat(" << sc.name << "_uint)\n"; break;
      default: break;
    }
  }
  for (const CompilationConstant &sc : info.compilation_constants_) {
    ss << "const ";
    switch (sc.type) {
      case Type::int_t: ss << "int " << sc.name << "=" << std::to_string(sc.value.i) << ";\n"; break;
      case Type::uint_t: ss << "uint " << sc.name << "=" << std::to_string(sc.value.u) << "u;\n"; break;
      case Type::bool_t: ss << "bool " << sc.name << "=" << (sc.value.u ? "true" : "false") << ";\n"; break;
      default: break;
    }
  }

  /* Workgroup-shared variables ([[shared]] in BSL): the shader body invokes
   * CREATE_INFO_RES_SHARED_VARS_<info>, which must expand to the `shared`
   * declarations (matches VKShader). Missing this leaves `local_depths` etc.
   * undeclared in every compute shader that uses workgroup memory. */
  {
    std::string active;
    for (const ShaderCreateInfo::SharedVariable &sv : info.shared_variables_) {
      if (active != std::string(sv.info_name)) {
        active = std::string(sv.info_name);
        ss << "\n#define CREATE_INFO_RES_SHARED_VARS_" << sv.info_name << " \\\n";
      }
      ss << "shared " << to_string(sv.type) << " " << sv.name << ";";
      ss << " \\\n";
    }
    ss << "\n";
  }

  int binding = 0;
  /* Emit one resource as a single line (no trailing newline — the macro adds the
   * line-continuation). */
  auto print_one = [&](const ShaderCreateInfo::Resource &res) {
    ss << "layout(binding = " << binding++;
    if (res.bind_type == ShaderCreateInfo::Resource::BindType::IMAGE) {
      ss << ", " << to_string(res.image.format);
    }
    else if (res.bind_type == ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER) {
      ss << ", std140";
    }
    else if (res.bind_type == ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER) {
      ss << ", std430";
    }
    ss << ") ";
    switch (res.bind_type) {
      case ShaderCreateInfo::Resource::BindType::SAMPLER:
        ss << "uniform "; print_image_type(ss, res.sampler.type, res.bind_type);
        ss << " " << res.sampler.name << ";"; break;
      case ShaderCreateInfo::Resource::BindType::IMAGE:
        ss << "uniform "; print_qualifier(ss, res.image.qualifiers);
        print_image_type(ss, res.image.type, res.bind_type);
        ss << " " << res.image.name << ";"; break;
      case ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER:
        ss << "uniform _" << res.uniformbuf.name.str_no_array() << " { "
           << info.buffer_typename(res.uniformbuf.type_name, true) << " " << res.uniformbuf.name
           << "; };"; break;
      case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
        print_qualifier(ss, res.storagebuf.qualifiers);
        ss << "buffer _" << res.storagebuf.name.str_no_array() << " { "
           << info.buffer_typename(res.storagebuf.type_name) << " " << res.storagebuf.name
           << "; };"; break;
    }
  };
  /* Wrap each create-info's resources in a CREATE_INFO_RES_<freq>_<info> macro.
   * The shader body invokes these macros AFTER the struct/typedef definitions are
   * in scope, so struct-typed UBOs/SSBOs resolve correctly (matches VKShader). */
  auto emit_freq = [&](const char *freq, const auto &resources) {
    std::string active;
    for (const ShaderCreateInfo::Resource &res : resources) {
      if (active != std::string(res.info_name)) {
        active = std::string(res.info_name);
        ss << "\n#define CREATE_INFO_RES_" << freq << "_" << res.info_name << " \\\n";
      }
      print_one(res);
      ss << " \\\n";
    }
    ss << "\n";
  };
  emit_freq("PASS", info.pass_resources_);
  emit_freq("BATCH", info.batch_resources_);
  emit_freq("GEOMETRY", info.geometry_resources_);

  if (!info.push_constants_.is_empty()) {
    ss << "layout(binding = " << binding++ << ", std140) uniform constants {\n";
    for (const ShaderCreateInfo::PushConst &uniform : info.push_constants_) {
      ss << "  " << to_string(uniform.type) << " " << uniform.name;
      if (uniform.array_size > 0) { ss << "[" << uniform.array_size << "]"; }
      ss << ";\n";
    }
    ss << "};\n";
  }
  ss << "\n";
  return ss.str();
}

std::string WebGPUShader::vertex_interface_declare(const shader::ShaderCreateInfo &info) const
{
  std::stringstream ss;
  for (const ShaderCreateInfo::VertIn &attr : info.vertex_inputs_) {
    ss << "layout(location = " << attr.index << ") in " << to_string(attr.type) << " "
       << attr.name << ";\n";
  }
  int location = 0;
  for (const StageInterfaceInfo *iface : info.vertex_out_interfaces_) {
    print_interface(ss, "out", *iface, location);
  }
  ss << "\n";
  /* Blender emits gl_Position in OpenGL clip-space conventions (z in -1..1;
   * EEVEE's reverse_z::transform also assumes "scaling to 0..1 is handled by the
   * backend"). WebGPU clips z to 0..1, so wrap main and retarget depth exactly
   * like the Vulkan backend does — without this every vertex ends up outside the
   * clip volume and NOTHING rasterizes. */
  ss << "\n";
  /* Point-sprite emulation (WebGPU points are 1px, no PointSize/PointCoord):
   * point draws are expanded to one 4-vertex triangle-strip quad PER INSTANCE
   * (vertex buffers switch to per-instance stepping — see record_draw). The
   * corner offset scales by whatever the shader wrote to gl_PointSize (routed
   * into gpu_point_size_var by the preamble). All three constants are WGSL
   * overrides supplied at pipeline creation; gpu_point_expand stays 0.0 for
   * regular draws, making this a no-op. */
  ss << "#ifdef GPU_POINTCOORD_VARYING\n";
  ss << "layout(location = 15) out vec2 gpu_pointcoord_var;\n";
  ss << "#endif\n";
  ss << "void main_function_();\n";
  ss << "void main() {\n";
  ss << "  main_function_();\n";
  ss << "#ifdef GPU_POINTCOORD_VARYING\n";
  ss << "  gpu_pointcoord_var = vec2(0.5);\n";
  ss << "#endif\n";
  ss << "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
  /* Full GL emulation: negate y so every render target gets GL's bottom-up row
   * layout. Rendered textures then sample consistently with GL-convention UVs
   * (region compositing, film, HiZ...); the single remaining mirror is undone
   * once when blitting the window backbuffer to the canvas (present) or when
   * reading pixels back. This also restores GL's winding relationship, so
   * frontFace stays CCW-front like GL. */
  ss << "  gl_Position.y = -gl_Position.y;\n";
  ss << "  if (gpu_point_expand > 0.5) {\n";
  ss << "    vec2 gpu_corner_ = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1)) *\n";
  ss << "                       2.0 - 1.0;\n";
  ss << "    gl_Position.xy += gpu_corner_ * max(gpu_point_size_var, 1.0) * gl_Position.w /\n";
  ss << "                      vec2(gpu_viewport_w, gpu_viewport_h);\n";
  /* gl_PointCoord convention: (0,0) at the sprite's TOP-left. gpu_corner_ is
   * post-y-negation clip space where +y is up on screen. */
  ss << "#ifdef GPU_POINTCOORD_VARYING\n";
  ss << "    gpu_pointcoord_var = vec2(gpu_corner_.x * 0.5 + 0.5, 0.5 - gpu_corner_.y * 0.5);\n";
  ss << "#endif\n";
  ss << "  }\n";
  ss << "}\n";
  ss << "#define main main_function_\n";
  ss << "\n";
  return ss.str();
}

std::string WebGPUShader::fragment_interface_declare(const shader::ShaderCreateInfo &info) const
{
  std::stringstream ss;
  int location = 0;
  for (const StageInterfaceInfo *iface : info.vertex_out_interfaces_) {
    print_interface(ss, "in", *iface, location);
  }
  /* WebGPU requires a fragment output to have AT LEAST as many components as its
   * color target format (GL allows fewer). Attachment formats are unknown at
   * shader build, so declare every output as a 4-component vector: a plain
   * global keeps the original name/type for the shader body, and a wrapped main
   * copies it into the padded real output (extra components written as 0). */
  std::string post_main;
  std::string pre_main;

  /* Sub-pass inputs, emulated like the GL non-framebuffer-fetch path: declare a
   * plain global with the input's name, a hidden sampler bound at the ATTACHMENT
   * index (WebGPUFrameBuffer::subpass_transition binds the read attachment there),
   * and populate the global from a texelFetch before main. WGSL bindings 200+
   * keep the hidden samplers clear of the sequential resource bindings. */
  for (const ShaderCreateInfo::SubpassIn &input : info.subpass_inputs_) {
    const std::string type_str = to_string(input.type);
    std::string image_name = "gpu_subpass_img_" + std::to_string(input.index);
    ss << type_str << " " << input.name << ";\n";
    ss << "layout(binding = " << (200 + input.index) << ") uniform ";
    print_image_type(ss, input.img_type, ShaderCreateInfo::Resource::BindType::SAMPLER);
    ss << image_name << ";\n";

    int comp = 4;
    char last = type_str.empty() ? '4' : type_str.back();
    comp = (last >= '2' && last <= '4') ? last - '0' : 1;
    const char *swizzles[] = {"", ".x", ".xy", ".xyz", ".xyzw"};
    const bool is_layered = ELEM(input.img_type,
                                 ImageType::Uint2DArray,
                                 ImageType::Int2DArray,
                                 ImageType::Float2DArray);
    pre_main += "  " + std::string(input.name) + " = texelFetch(" + image_name + ", " +
                (is_layered ? "ivec3(gl_FragCoord.xy, 0)" : "ivec2(gl_FragCoord.xy)") + ", 0)" +
                swizzles[comp] + ";\n";
  }

  for (const ShaderCreateInfo::FragOut &output : info.fragment_outputs_) {
    const std::string type_str = to_string(output.type);
    /* Component count from the type name: vecN/ivecN/uvecN or scalar. */
    int comp = 4;
    char last = type_str.empty() ? '4' : type_str.back();
    if (last >= '2' && last <= '4') {
      comp = last - '0';
    }
    else {
      comp = 1; /* float/int/uint scalar. */
    }
    if (comp == 4) {
      ss << "layout(location = " << output.index;
      if (output.blend != DualBlend::NONE) {
        ss << ", index = " << ((output.blend == DualBlend::SRC_0) ? 0 : 1);
      }
      ss << ") out " << to_string(output.type) << " " << output.name << ";\n";
      continue;
    }
    /* Padded output. Base scalar kind decides vec4/ivec4/uvec4. */
    const char *pad4 = "vec4";
    if (type_str[0] == 'i') {
      pad4 = "ivec4";
    }
    else if (type_str[0] == 'u') {
      pad4 = "uvec4";
    }
    ss << "layout(location = " << output.index;
    if (output.blend != DualBlend::NONE) {
      ss << ", index = " << ((output.blend == DualBlend::SRC_0) ? 0 : 1);
    }
    ss << ") out " << pad4 << " " << output.name << "_wgpu_pad_;\n";
    ss << type_str << " " << output.name << ";\n";
    post_main += "  " + std::string(output.name) + "_wgpu_pad_ = " + pad4 + "(" + std::string(output.name);
    for (int i = comp; i < 4; i++) {
      post_main += ", 0";
    }
    post_main += ");\n";
  }
  if (!post_main.empty() || !pre_main.empty()) {
    ss << "\n";
    ss << "void main_function_();\n";
    ss << "void main() {\n";
    ss << pre_main;
    ss << "  main_function_();\n";
    ss << post_main;
    ss << "}\n";
    ss << "#define main main_function_\n";
  }
  ss << "\n";
  return ss.str();
}

std::string WebGPUShader::geometry_interface_declare(const shader::ShaderCreateInfo & /*info*/) const
{
  return "";
}

std::string WebGPUShader::geometry_layout_declare(const shader::ShaderCreateInfo & /*info*/) const
{
  return "";
}

std::string WebGPUShader::compute_layout_declare(const shader::ShaderCreateInfo &info) const
{
  std::stringstream ss;
  if (info.compute_layout_.local_size_x != -1) {
    ss << "layout(local_size_x = " << info.compute_layout_.local_size_x;
    if (info.compute_layout_.local_size_y != -1) {
      ss << ", local_size_y = " << info.compute_layout_.local_size_y;
    }
    if (info.compute_layout_.local_size_z != -1) {
      ss << ", local_size_z = " << info.compute_layout_.local_size_z;
    }
    ss << ") in;\n";
  }
  return ss.str();
}

}  // namespace blender::gpu
