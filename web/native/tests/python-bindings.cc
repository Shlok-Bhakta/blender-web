/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <blender-web-numpy.hh>
#include <blender-web-python-bindings.hh>
int main()
{
  GHOST_web_register_numpy();
  GHOST_web_register_python_bindings();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  PyConfig_SetBytesString(&config, &config.home, "/python");
  PyStatus status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);
  const int result = PyRun_SimpleString(
      "import runpy, json, gc\n"
      "print(json.dumps(runpy.run_path('/python-bindings.py')['run']('/tmp/python-bindings')))\n"
      "gc.collect()\n");
  return result || Py_FinalizeEx();
}
