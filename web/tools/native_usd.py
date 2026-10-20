#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Register OpenUSD's original static extensions in embedded CPython."""
import json
from pathlib import Path
import re
import subprocess


def install_bindings(prefix):
    package = prefix / 'lib/python/pxr'
    modules = []
    libraries = sorted(package.glob('*/_*.a'))
    for library in libraries:
        symbols = subprocess.check_output(['emnm', '--defined-only', str(library)], text=True)
        initializers = re.findall(r'\bT (PyInit__\w+)\b', symbols)
        if len(initializers) != 1:
            raise RuntimeError(f'Expected one USD initializer in {library}: {initializers}')
        initializer = initializers[0]
        modules.append(('pxr.' + library.parent.name + '.' + initializer.removeprefix('PyInit_'), initializer))
    required = {'pxr.Tf._tf', 'pxr.Sdf._sdf', 'pxr.Usd._usd', 'pxr.UsdGeom._usdGeom',
                'pxr.UsdShade._usdShade', 'pxr.UsdMtlx._usdMtlx'}
    if not required.issubset({name for name, _ in modules}):
        raise RuntimeError(f'Missing USD static extensions: {modules}')
    header = prefix / 'include/blender-web-usd.hh'
    header.write_text('/* SPDX-License-Identifier: GPL-2.0-or-later */\n#pragma once\n'
        '#include <Python.h>\n#include <cstdlib>\n' +
        '\n'.join(f'extern "C" PyObject *{init}();' for _, init in modules) +
        '\ninline void GHOST_web_register_usd()\n{\n' +
        '\n'.join(f'  if (PyImport_AppendInittab("{name}", {init}) < 0) std::abort();'
                  for name, init in modules) + '\n}\n')
    destination = prefix / 'lib/blender-web-usd'
    destination.mkdir(exist_ok=True)
    (destination / 'libraries.cmake').write_text('set(BLENDER_WEB_USD_PYTHON_LIBRARIES\n' +
        '\n'.join(f'  "${{CMAKE_CURRENT_LIST_DIR}}/../python/pxr/{p.relative_to(package)}"'
                  for p in libraries) + '\n)\n')
    report = {'modules': [name for name, _ in modules], 'archives': len(libraries),
              'staticBytes': sum(p.stat().st_size for p in libraries)}
    (destination / 'bindings.json').write_text(json.dumps(report, indent=2) + '\n')
    return report
