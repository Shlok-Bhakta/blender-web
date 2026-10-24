/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include "../ghost/GHOST_WebGLTF.hh"
int main()
{
  GHOST_web_register_gltf();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  PyConfig_SetBytesString(&config, &config.home, "/python");
  PyStatus status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);
  int result = PyRun_SimpleString(
      "import sys, runpy, json\n"
      "sys.path.insert(0, '/')\n"
      "print('GLTF_CHECK:' + json.dumps(runpy.run_path('/check.py')['check']()))\n");
  return result || Py_FinalizeEx();
}
