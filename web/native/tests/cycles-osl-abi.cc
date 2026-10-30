/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include "util/defines.h"
#include "util/types.h"
#include "util/param.h"
#include <OSL/oslclosure.h>
#include <OSL/shaderglobals.h>
#include "kernel/osl/types.h"
#include "kernel/osl/services.h"
#ifndef BLENDER_WEB_OSL_ABI_BASELINE
#  include "kernel/osl/strings.h"
#endif

static_assert(sizeof(ccl::OSLClosure) == sizeof(OSL::ClosureColor));
static_assert(sizeof(ccl::OSLClosureAdd) == sizeof(OSL::ClosureAdd));
static_assert(sizeof(ccl::OSLClosureMul) == sizeof(OSL::ClosureMul));
static_assert(sizeof(ccl::OSLClosureComponent) == sizeof(OSL::ClosureComponent));
static_assert(alignof(ccl::OSLClosureAdd) == alignof(OSL::ClosureAdd));
static_assert(alignof(ccl::OSLClosureMul) == alignof(OSL::ClosureMul));
using StringFactory = ccl::DeviceString (*)(const char *, uint64_t);
static_assert(std::is_same_v<decltype(&ccl::make_string), StringFactory>);

#define MEMBER(cycles, osl) \
  static_assert(offsetof(ccl::ShaderGlobals, cycles) == offsetof(OSL::ShaderGlobals, osl))
MEMBER(P, P); MEMBER(dPdx, dPdx); MEMBER(dPdy, dPdy); MEMBER(dPdz, dPdz);
MEMBER(I, I); MEMBER(dIdx, dIdx); MEMBER(dIdy, dIdy); MEMBER(N, N); MEMBER(Ng, Ng);
MEMBER(u, u); MEMBER(dudx, dudx); MEMBER(dudy, dudy);
MEMBER(v, v); MEMBER(dvdx, dvdx); MEMBER(dvdy, dvdy);
MEMBER(dPdu, dPdu); MEMBER(dPdv, dPdv); MEMBER(time, time); MEMBER(dtime, dtime);
MEMBER(dPdtime, dPdtime); MEMBER(Ps, Ps); MEMBER(dPsdx, dPsdx); MEMBER(dPsdy, dPsdy);
MEMBER(sd, renderstate); MEMBER(tracedata, tracedata); MEMBER(kg, objdata);
MEMBER(context, context); MEMBER(shadingStateUniform, shadingStateUniform);
MEMBER(thread_index, thread_index); MEMBER(shade_index, shade_index);
MEMBER(renderer, renderer); MEMBER(object2common, object2common);
MEMBER(shader2common, shader2common); MEMBER(Ci, Ci); MEMBER(surfacearea, surfacearea);
MEMBER(raytype, raytype); MEMBER(flipHandedness, flipHandedness); MEMBER(backfacing, backfacing);
#undef MEMBER

static void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "Cycles OSL ABI: %s\n", message);
    std::abort();
  }
}

#ifdef BLENDER_WEB_OSL_ABI_BASELINE
int main() { return 0; }
#else
int main()
{
  OSL::ClosureComponent first{}, second{};
  first.id = 7; first.w = OSL::Color3(1, 2, 3);
  second.id = 9; second.w = OSL::Color3(.5f, .25f, .125f);
  OSL::ClosureMul multiply{};
  multiply.id = OSL::ClosureColor::MUL;
  multiply.weight = OSL::Color3(2, 3, 4);
  multiply.closure = &first;
  OSL::ClosureAdd add{};
  add.id = OSL::ClosureColor::ADD;
  add.closureA = &multiply;
  add.closureB = &second;
  const auto &tree = reinterpret_cast<const ccl::OSLClosureAdd &>(add);
  require(tree.id == ccl::OSL_CLOSURE_ADD_ID, "add node id");
  const auto &mul = *static_cast<const ccl::OSLClosureMul *>(tree.closureA);
  require(mul.id == ccl::OSL_CLOSURE_MUL_ID, "multiply node id");
  require(mul.weight.x == 2 && mul.weight.y == 3 && mul.weight.z == 4, "multiply weight");
  const auto &leaf = *static_cast<const ccl::OSLClosureComponent *>(mul.closure);
  const auto &other = *static_cast<const ccl::OSLClosureComponent *>(tree.closureB);
  require(int(leaf.id) == 7 && leaf.weight.x == 1 && leaf.weight.z == 3, "first leaf");
  require(int(other.id) == 9 && other.weight.x == .5f && other.weight.z == .125f, "second leaf");
  const char *text = "osl-cycles-web-hash";
  const uint64_t hash = OIIO::ustring(text).hash();
  require(hash > UINT32_MAX, "hash fixture exercises high bits");
  require(ccl::make_string(text, hash).hash() == hash, "Cycles preserves the complete hash");
  require(ccl::DeviceStrings::u_common.hash() == UINT64_C(14645198576927606093),
          "predefined device strings preserve the complete hash");
  // Exercise the pointer adapter with the actual record field types, without
  // creating an ImageManager or replacing ImageHandle's ownership methods.
  struct Record {
    decltype(ccl::OSLTextureHandle::type) type;
    decltype(ccl::OSLTextureHandle::id) id;
  };
  const ccl::OSLTextureHandleType types[] = {ccl::OSLTextureHandleType::IMAGE,
      ccl::OSLTextureHandleType::IES, ccl::OSLTextureHandleType::BEVEL, ccl::OSLTextureHandleType::AO};
  const int ids[] = {0, 1, INT_MAX, -1, -17, INT_MIN};
  require(GHOST_web_osl_texture_handle_type<Record>(nullptr) == ccl::OSLTextureHandleType{},
          "null handle retains the original zero type");
  require(GHOST_web_osl_texture_handle_id<Record>(nullptr) == 0, "null handle retains the original zero ID");
  unsigned handles = 0;
  for (auto type : types) for (int id : ids) {
    const Record record{type, id};
    const auto *handle = reinterpret_cast<const void *>(GHOST_web_osl_texture_handle(&record));
    require(handle == &record, "texture handle retains the original record address");
    require(GHOST_web_osl_texture_handle_type<Record>(handle) == type, "texture type is preserved");
    require(GHOST_web_osl_texture_handle_id<Record>(handle) == id, "full signed image ID is preserved");
    ++handles;
  }
  // Instantiate the production macro with Cycles' real complete record type.
  using ccl::OSLTextureHandle;
  using ActualType = decltype(OSL_TEXTURE_HANDLE_TYPE(static_cast<const void *>(nullptr)));
  static_assert(std::is_same_v<ActualType, ccl::OSLTextureHandleType>);
  std::printf("{\"pointerBytes\":%zu,\"addBytes\":%zu,\"multiplyBytes\":%zu,"
              "\"componentBytes\":%zu,\"shaderGlobalsPrefixBytes\":%zu,"
              "\"closureTree\":true,\"stringHashFull\":true,\"hash\":\"%016llx\","
              "\"textureHandleCases\":%u,\"applicationIntegrated\":false}\n",
              sizeof(void *), sizeof(ccl::OSLClosureAdd), sizeof(ccl::OSLClosureMul),
              sizeof(ccl::OSLClosureComponent), sizeof(OSL::ShaderGlobals),
              static_cast<unsigned long long>(hash), handles);
}
#endif
