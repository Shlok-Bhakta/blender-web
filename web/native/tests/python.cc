/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <expat.h>
int main()
{
  // Also link Blender's Expat, alongside CPython's private bundled copy.
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser || XML_Parse(parser, "<blender/>", 10, true) != XML_STATUS_OK) return 2;
  XML_ParserFree(parser);
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  PyConfig_SetBytesString(&config, &config.home, "/python");
  PyStatus status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);
  const int result = PyRun_SimpleString(
    "import sys, json, zlib, bz2, decimal, hashlib, xml.etree.ElementTree as ET\n"
    "data = b'Blender Python in Wasm'\n"
    "assert zlib.decompress(zlib.compress(data)) == data\n"
    "assert bz2.decompress(bz2.compress(data)) == data\n"
    "assert decimal.Decimal('0.1') + decimal.Decimal('0.2') == decimal.Decimal('0.3')\n"
    "assert ET.fromstring('<blender/>').tag == 'blender'\n"
    "assert hashlib.sha256(data).hexdigest() == 'f10cca7b130db4656ce3d4aac2112b1ac5c0e2dace5fca7508a8908f4a5c0d67'\n"
    "print(json.dumps({'pythonVersion': sys.version.split()[0], 'embeddedModules': True}))\n");
  return result || Py_FinalizeEx();
}
