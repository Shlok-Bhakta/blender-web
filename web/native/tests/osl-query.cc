// SPDX-License-Identifier: GPL-2.0-or-later
#include <OSL/oslquery.h>
#include <OSL/encodedtypes.h>
#include <json.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>

using Json = nlohmann::json;
static void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "OSL query check: %s\n", message);
    std::exit(1);
  }
}

static Json parameter(const OSL::OSLQuery::Parameter &p)
{
  Json value = nullptr;
  if (p.type.basetype == OSL::TypeDesc::INT) {
    if (p.idefault.size() == 1) value = p.idefault[0];
    else value = p.idefault;
  }
  else if (p.type.basetype == OSL::TypeDesc::FLOAT) {
    if (p.fdefault.size() == 1) value = p.fdefault[0];
    else value = p.fdefault;
  }
  else if (p.type.basetype == OSL::TypeDesc::STRING) {
    if (p.sdefault.size() == 1) value = p.sdefault[0].c_str();
    else {
      value = Json::array();
      for (auto s : p.sdefault) value.push_back(s.c_str());
    }
  }
  Json metadata = Json::array();
  for (const auto &m : p.metadata) metadata.push_back(parameter(m));
  return Json{{"name", p.name.c_str()}, {"type", p.type.c_str()}, {"output", p.isoutput},
              {"variableArray", p.varlenarray}, {"structure", p.isstruct}, {"closure", p.isclosure},
              {"value", value}, {"metadata", metadata}};
}

static Json query_json(const OSL::OSLQuery &query)
{
  Json parameters = Json::array(), metadata = Json::array();
  for (const auto &p : query.parameters()) parameters.push_back(parameter(p));
  for (const auto &m : query.metadata()) metadata.push_back(parameter(m));
  return Json{{"name", query.shadername().c_str()}, {"type", query.shadertype().c_str()},
              {"parameters", parameters}, {"metadata", metadata}};
}

int main()
{
  Json reference;
  std::ifstream("/fixtures/native-query.json") >> reference;
  std::ifstream file("/fixtures/web_query.oso", std::ios::binary);
  std::string bytecode{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  require(!bytecode.empty(), "native-authored bytecode loaded");
  OSL::OSLQuery file_query;
  require(file_query.open("/fixtures/web_query.oso"), "query original shader file");
  require(query_json(file_query) == reference, "file query matches native metadata");
  OSL::OSLQuery query;
  require(!query.open_bytecode("not valid OSL bytecode\n"), "invalid bytecode rejected");
  require(!query.open("missing-query-fixture", "/fixtures"), "missing file rejected");
  require(!query.geterror().empty(), "missing file has a diagnostic");
  require(query.open_bytecode(bytecode), "query recovers after invalid input");
  require(query_json(query) == reference, "memory query matches native metadata");
  require(query.getparam("missing") == nullptr, "missing parameter lookup");
  require(query.getparam("gain") != nullptr && query.getparam(8) != nullptr,
          "parameter lookups by name and index");

  constexpr uint64_t full_hash = UINT64_C(0x123456789abcdef0);
  auto hash = OSL::ustringhash::from_hash(full_hash);
  auto encoded = OSL::pvt::TypeEncoder<OSL::ustringhash>::Encode(hash);
  require(sizeof(encoded) == 8 && encoded == full_hash, "hash high bits preserved");
  OSL::ustring text("browser OSL metadata");
  auto string_hash = OSL::pvt::TypeEncoder<OSL::ustring>::Encode(text);
  require(string_hash == text.hash() && string_hash > UINT32_MAX, "string hash high bits preserved");

  static_assert(OSL::pvt::TypeEncoder<long>::value == OSL::EncodedType::kInt32);
  static_assert(OSL::pvt::TypeEncoder<unsigned long>::value == OSL::EncodedType::kUInt32);
  require(OSL::pvt::TypeEncoder<long>::Encode(-2147483647L - 1) == INT32_MIN,
          "signed long diagnostic value preserved");
  require(OSL::pvt::TypeEncoder<unsigned long>::Encode(4294967295UL) == UINT32_MAX,
          "unsigned long diagnostic value preserved");

  std::atomic<unsigned> errors{0};
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 4; ++worker) workers.emplace_back([&] {
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
      OSL::OSLQuery local;
      if (!local.open_bytecode(bytecode) || query_json(local) != reference) ++errors;
    }
  });
  for (auto &worker : workers) worker.join();
  require(errors.load() == 0, "parallel queries match native metadata");
  std::puts(Json{{"oslVersion", OSL_LIBRARY_VERSION_STRING}, {"parameters", query.nparams()},
                 {"nativeMetadataExact", true}, {"fileAndMemory", true},
                 {"invalidInputRecovery", true}, {"hashPodBytes", sizeof(OSL::ustringhash_pod)},
                 {"hashHighBitsPreserved", true}, {"workers", 4}, {"parallelQueries", 128},
                 {"pointerBytes", sizeof(void *)}}.dump().c_str());
}
