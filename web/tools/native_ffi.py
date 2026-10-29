#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build pinned libffi and describe the statically linked Python namespace."""
import os
from pathlib import Path
import re
import subprocess

LINK_OPTIONS = ['-sALLOW_TABLE_GROWTH=1', *(
    '-Wl,--wrap=' + name for name in
    ('ffi_closure_alloc', 'ffi_prep_closure_loc', 'ffi_closure_free',
     'pthread_create', 'emscripten_futex_wait'))]


def build_library(source, work, prefix, options, jobs, log, run, flags):
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CC='emcc', CXX='em++', AR='emar', RANLIB='emranlib',
               NM='emnm', CFLAGS='-O3 ' + flags, CXXFLAGS='-O3 ' + flags)
    run(['emconfigure', source / 'configure', f'--prefix={prefix}', *options],
        log, cwd=work, env=env)
    run(['emmake', 'make', f'-j{jobs}'], log, cwd=work, env=env)
    run(['emmake', 'make', 'install'], log, cwd=work, env=env)
    (prefix / 'lib/blender-web-ffi.cmake').write_text(
        'set(BLENDER_WEB_FFI_LINK_OPTIONS\n' +
        ''.join(f'  {option}\n' for option in LINK_OPTIONS) + ')\n')


def generate_namespace(prefix, destination):
    # Take the intersection of declarations and definitions. Referencing actual
    # declarations preserves Wasm signatures and distinguishes data from code.
    headers = prefix / 'include/python3.13'
    declarations = set()
    for path in [*headers.glob('*.h'), *(headers / 'cpython').glob('*.h')]:
        declarations.update(re.findall(
            r'PyAPI_(?:FUNC|DATA)\s*\([^)]*\)\s*([A-Za-z_]\w*)', path.read_text()))
    output = subprocess.check_output(
        ['emnm', '--defined-only', '--extern-only', '--format=posix',
         str(prefix / 'lib/libpython3.13.a')], text=True)
    definitions = {line.split()[0] for line in output.splitlines()
                   if len(line.split()) >= 2 and not line.endswith(':')}
    symbols = sorted(declarations & definitions)
    if not {'Py_GetVersion', 'PyExc_ValueError', 'PyErr_Format', '_Py_c_sum'} <= set(symbols):
        raise RuntimeError('Incomplete typed Python namespace')
    destination.write_text('/* Generated from installed Python headers and libpython. */\n' +
        ''.join(f'  {{"{name}", const_cast<void *>(reinterpret_cast<const void *>(&{name}))}},\n'
                for name in symbols))
    return symbols
