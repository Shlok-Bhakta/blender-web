#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build original CPython extension sources as statically registered modules."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

from prepare import ROOT


# Match the release's native SQLite recipe and configure defaults. File mmap is
# disabled because browser file storage does not provide shared POSIX mappings.
SQLITE_DEFINITIONS = [
    'THREADSAFE=1', 'SECURE_DELETE', 'ENABLE_COLUMN_METADATA', 'ENABLE_FTS3',
    'ENABLE_FTS3_PARENTHESIS', 'ENABLE_FTS4', 'ENABLE_FTS5', 'SOUNDEX',
    'ENABLE_UNLOCK_NOTIFY', 'OMIT_LOOKASIDE=1', 'ENABLE_DBSTAT_VTAB',
    'ENABLE_UPDATE_DELETE_LIMIT=1', 'ENABLE_LOAD_EXTENSION',
    'LIKE_DOESNT_MATCH_BLOBS', 'MAX_SCHEMA_RETRY=25', 'ENABLE_PREUPDATE_HOOK',
    'ENABLE_SESSION', 'ENABLE_STMTVTAB', 'MAX_VARIABLE_NUMBER=250000',
    'ENABLE_RTREE', 'ENABLE_GEOPOLY', 'ENABLE_MATH_FUNCTIONS',
    'ENABLE_PERCENTILE', 'DEFAULT_AUTOVACUUM=1', 'DEFAULT_RECURSIVE_TRIGGERS=1',
    'MAX_MMAP_SIZE=0',
]


def extract_source(entry, base, destination):
    from native_deps import download
    archive = download(entry, base / 'deps/downloads')
    receipt = destination.with_name(destination.name + '.source.json')
    if receipt.exists() and json.loads(receipt.read_text()) != entry:
        shutil.rmtree(destination, ignore_errors=True)
    if not destination.is_dir():
        temporary = destination.with_name(destination.name + '.extract')
        temporary.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as bundle:
            bundle.extractall(temporary, filter='data')
        roots = list(temporary.iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise RuntimeError(f'Unexpected source archive: {archive}')
        roots[0].rename(destination)
        temporary.rmdir()
    receipt.write_text(json.dumps(entry, indent=2) + '\n')
    return destination


def build_library(name, source, work, prefix, options, jobs, log, run, flags):
    work.mkdir(parents=True, exist_ok=True)
    for directory in ('include', 'lib', 'lib/pkgconfig'):
        (prefix / directory).mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CC='emcc', CXX='em++', AR='emar', RANLIB='emranlib',
               NM='emnm', CFLAGS='-O3 ' + flags, CXXFLAGS='-O3 ' + flags)
    if name == 'SQLITE':
        run(['emcc', '-O3', *flags.split(),
             *('-DSQLITE_' + value for value in SQLITE_DEFINITIONS),
             '-c', source / 'sqlite3.c', '-o', work / 'sqlite3.o'], log)
        run(['emar', 'rcs', prefix / 'lib/libsqlite3.a', work / 'sqlite3.o'], log)
        for header in ('sqlite3.h', 'sqlite3ext.h'):
            shutil.copy2(source / header, prefix / 'include' / header)
        version = re.search(r'^#define SQLITE_VERSION\s+"([^"]+)"',
                            (source / 'sqlite3.h').read_text(), re.MULTILINE)[1]
        (prefix / 'lib/pkgconfig/sqlite3.pc').write_text(
            f'prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n'
            'Name: SQLite\nDescription: SQLite database engine\n'
            f'Version: {version}\nLibs: -L${{libdir}} -lsqlite3\nCflags: -I${{includedir}}\n')
    elif name == 'LZMA':
        # This pinned config.sub predates the emscripten OS name. Its CPU is
        # still wasm32; all compilation uses the actual Emscripten toolchain.
        run(['emconfigure', source / 'configure', f'--prefix={prefix}', *options],
            log, cwd=work, env=env)
        run(['emmake', 'make', f'-j{jobs}'], log, cwd=work, env=env)
        run(['emmake', 'make', 'install'], log, cwd=work, env=env)
    elif name == 'SSL':
        run(['perl', source / 'Configure',
             f'--config={ROOT / "web/native/openssl-wasm.conf"}',
             'blender-web-wasm32', f'--prefix={prefix}', '--libdir=lib',
             '--openssldir=/dev/null', *options], log, cwd=work, env=env)
        run(['emmake', 'make', f'-j{jobs}', 'build_libs'], log, cwd=work, env=env)
        run(['emmake', 'make', 'install_dev'], log, cwd=work, env=env)
    else:
        raise ValueError(name)


def build(base, jobs):
    from native_deps import C_FLAGS, download, install_notices, run
    prefix = base / 'deps/prefix'
    work = base / 'deps/python-extensions'
    work.mkdir(parents=True, exist_ok=True)
    lock = work / 'source-lock.json'
    subprocess.run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}',
        '-DDEPENDENCIES=PYTHON;ZSTANDARD', f'-DOUTPUT={lock}',
        '-P', str(ROOT / 'web/native/dependency-lock.cmake')], check=True)
    sources = json.loads(lock.read_text())
    inputs = [Path(__file__), ROOT / 'web/tools/native_deps.py',
              ROOT / 'web/native/shader-deps/build_python.sh', ROOT / 'web/upstream.json']
    inputs += [ROOT / 'web/native/ghost/GHOST_WebNetwork.cc']
    dependency_stamps = {}
    for name in ('sqlite', 'lzma', 'ssl', 'zstd'):
        stamp = base / f'deps/{name}/installed.json'
        if not stamp.is_file():
            raise RuntimeError(f'Build {name} before the Python extension modules')
        dependency_stamps[name] = hashlib.sha256(stamp.read_bytes()).hexdigest()
    signature = hashlib.sha256(b''.join(path.read_bytes() for path in inputs) +
        json.dumps([sources, dependency_stamps], sort_keys=True).encode()).hexdigest()
    destination = prefix / 'lib/blender-web-python'
    header = prefix / 'include/blender-web-python.hh'
    stamp = work / 'installed.json'
    artifacts = [header, destination / 'libraries.cmake', destination / 'extensions.a',
                 prefix / 'lib/python3.13/site-packages/zstandard/__init__.py']
    if (stamp.exists() and json.loads(stamp.read_text()).get('signature') == signature
            and all(path.is_file() for path in artifacts)):
        print('CPython extension modules: cached', flush=True)
        return json.loads(stamp.read_text())
    extracted = {}
    for name, entry in sources.items():
        source = extract_source(entry, base, work / name.lower())
        install_notices(source, prefix, name)
        extracted[name] = source
    python = extracted['PYTHON']
    module_sources = [*sorted((python / 'Modules/_sqlite').glob('*.c')),
                      python / 'Modules/_lzmamodule.c', python / 'Modules/_ssl.c',
                      python / 'Modules/_hashopenssl.c',
                      extracted['ZSTANDARD'] / 'c-ext/backend_c.c',
                      ROOT / 'web/native/ghost/GHOST_WebNetwork.cc']
    objects = work / 'objects'
    objects.mkdir(exist_ok=True)
    log = work / 'build.log'
    flags = ['-O3', *C_FLAGS.split(), '-DPY_CALL_TRAMPOLINE', '-DPy_BUILD_CORE_BUILTIN=1',
             '-DPY_SQLITE_HAVE_SERIALIZE=1', '-DPY_SQLITE_ENABLE_LOAD_EXTENSION=1',
             '-DZSTD_MULTITHREAD', f'-I{prefix}/include',
             f'-I{prefix}/include/python3.13', f'-I{python}/Include/internal',
             f'-I{python}/Modules', f'-I{extracted["ZSTANDARD"]}/c-ext']
    compiled = []
    for source in module_sources:
        target = objects / (source.stem + '.o')
        run(['emcc', *flags, '-c', source, '-o', target], log)
        compiled.append(target)
    destination.mkdir(exist_ok=True)
    library = destination / 'extensions.a'
    library.unlink(missing_ok=True)
    run(['emar', 'rcs', library, *compiled], log)
    modules = [('_sqlite3', 'PyInit__sqlite3'), ('_lzma', 'PyInit__lzma'),
               ('_ssl', 'PyInit__ssl'), ('_hashlib', 'PyInit__hashlib'),
               ('zstandard.backend_c', 'PyInit_backend_c'),
               ('_bpy_ghost_web_http', 'PyInit__bpy_ghost_web_http')]
    header.write_text('/* SPDX-License-Identifier: GPL-2.0-or-later */\n#pragma once\n'
        '#include <Python.h>\n#include <cstdlib>\n' +
        '\n'.join(f'extern "C" PyObject *{init}();' for _, init in modules) +
        '\ninline void GHOST_web_register_python_extensions()\n{\n' +
        '\n'.join(f'  if (PyImport_AppendInittab("{name}", {init}) < 0) std::abort();'
                  for name, init in modules) + '\n}\n')
    (destination / 'libraries.cmake').write_text('set(BLENDER_WEB_PYTHON_EXTENSION_LIBRARIES\n'
        '  "${CMAKE_CURRENT_LIST_DIR}/extensions.a"\n' +
        '\n'.join(f'  "${{CMAKE_CURRENT_LIST_DIR}}/../lib{name}.a"'
                  for name in ('sqlite3', 'lzma', 'ssl', 'crypto', 'zstd')) + '\n)\n')
    shutil.copytree(extracted['ZSTANDARD'] / 'zstandard',
        prefix / 'lib/python3.13/site-packages/zstandard', dirs_exist_ok=True,
        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    shutil.copytree(extracted['ZSTANDARD'] / 'zstandard.egg-info',
        prefix / 'lib/python3.13/site-packages' / ('zstandard-' + sources['ZSTANDARD']['VERSION'] + '.egg-info'),
        dirs_exist_ok=True)
    report = dict(signature=signature, sources=sources, dependencyStamps=dependency_stamps,
                  modules=[name for name, _ in modules], staticBytes=library.stat().st_size,
                  limitations=['Raw socket networking needs a browser transport adapter.',
                               'SQLite loadable Wasm extensions need dynamic linking integration.',
                               'ctypes and foreign-worker libffi closures are not integrated.'])
    stamp.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return report
