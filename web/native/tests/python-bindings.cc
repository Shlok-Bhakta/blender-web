/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <blender-web-numpy.hh>
#include <blender-web-python-bindings.hh>
#ifndef BLENDER_WEB_BINDING_TEST_SCRIPT
#  define BLENDER_WEB_BINDING_TEST_SCRIPT "/python-bindings.py"
#endif
int main()
{
  // Match Blender's explicit UTF-8 preinitialization on every platform.
  PyPreConfig preconfig;
  PyPreConfig_InitIsolatedConfig(&preconfig);
  preconfig.utf8_mode = 1;
  PyStatus status = Py_PreInitialize(&preconfig);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);
  GHOST_web_register_numpy();
  GHOST_web_register_python_bindings();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  PyConfig_SetBytesString(&config, &config.home, "/python");
  status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);
  const int result = PyRun_SimpleString(
      "import runpy, json, gc\n"
      "print(json.dumps(runpy.run_path('" BLENDER_WEB_BINDING_TEST_SCRIPT
      "')['run']('/tmp/python-bindings')))\n"
      "gc.collect()\n");
  return result || Py_FinalizeEx();
}
