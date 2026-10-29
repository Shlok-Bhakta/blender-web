/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <blender-web-numpy.hh>
#include <blender-web-python.hh>
#include <expat.h>
#include "python-tls-fixture.hh"
int main()
{
  GHOST_web_register_numpy();
  GHOST_web_register_python_extensions();
  create_python_tls_fixture("/tmp/python-foundation");
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
    "import numpy as np\n"
    "assert np.__version__ == '2.3.4'\n"
    "assert np.allclose(np.linalg.solve([[3.,1.],[1.,2.]], [9.,8.]), [2.,3.])\n"
    "assert np.allclose(np.fft.rfft([0.,1.,0.,-1.]), [0.,-2j,0.])\n"
    "assert np.random.default_rng(42).integers(0,100,5).tolist() == [8,77,65,43,43]\n"
    "assert np.random.default_rng(42).multinomial(10, [.2,.3,.5], size=4).sum() == 40\n"
    "assert np.random.RandomState(42).multinomial(10, [.2,.3,.5], size=4).sum() == 40\n"
    "import runpy\n"
    "foundation=runpy.run_path('/python-foundation.py')['run']('/tmp/python-foundation')\n"
    "ctypes=runpy.run_path('/python-ctypes.py')['run']()\n"
    "print(json.dumps({'pythonVersion': sys.version.split()[0], 'embeddedModules': True, 'numpyVersion': np.__version__, 'numpyCompiledModules': True, 'foundation':foundation, 'ctypes':ctypes}))\n");
  return result || Py_FinalizeEx();
}
