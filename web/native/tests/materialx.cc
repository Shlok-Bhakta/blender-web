/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <MaterialXCore/Document.h>
#include <MaterialXCore/Util.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <pugixml.hpp>
#include <cstdio>

int main()
{
  namespace mx = MaterialX;
  auto library = mx::createDocument();
  const auto files = mx::loadLibraries({}, mx::FileSearchPath("/libraries"), library);
  if (files.empty() || library->getNodeDefs().size() < 500) return 1;
  auto graph = mx::createDocument();
  mx::readFromXmlString(graph, R"xml(<materialx version="1.39">
    <open_pbr_surface name="surface" type="surfaceshader">
      <input name="base_color" type="color3" value="0.04, 0.3, 0.8"/>
    </open_pbr_surface>
    <surfacematerial name="Material" type="material">
      <input name="surfaceshader" type="surfaceshader" nodename="surface"/>
    </surfacematerial>
  </materialx>)xml");
  const auto xml = mx::writeToXmlString(graph);
  graph->importLibrary(library);
  std::string validation;
  if (!graph->validate(&validation)) {
    std::fprintf(stderr, "%s\n", validation.c_str());
    return 2;
  }
  // This parser is Blender's pinned PugiXML, while XmlIo uses MaterialX's
  // separately compiled copy. Both must coexist in the same static binary.
  pugi::xml_document parsed;
  if (!parsed.load_string(xml.c_str()) || !parsed.child("materialx").child("open_pbr_surface")) return 3;
  std::printf("{\"materialXVersion\":\"%s\",\"standardLibraries\":%zu,"
              "\"nodeDefinitions\":%zu,\"openPBRGraphValid\":true,\"independentPugiXML\":true}\n",
              mx::getVersionString().c_str(), files.size(), library->getNodeDefs().size());
  return 0;
}
