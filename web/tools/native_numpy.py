#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile pinned NumPy extensions as static built-ins for Blender's CPython."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

from prepare import ROOT
from native_deps import C_FLAGS, download, install_notices, run


def build(base, jobs):
    prefix = base / 'deps/prefix'
    work = base / 'deps/numpy'
    work.mkdir(parents=True, exist_ok=True)
    lock = work / 'source-lock.json'
    subprocess.run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}', '-DDEPENDENCIES=NUMPY',
                    f'-DOUTPUT={lock}', '-P', str(ROOT / 'web/native/dependency-lock.cmake')], check=True)
    entry = json.loads(lock.read_text())['NUMPY']
    inputs = [Path(__file__), ROOT / 'web/native/numpy-static.patch',
              ROOT / 'web/native/shader-deps/build_python.sh', ROOT / 'web/upstream.json',
              ROOT / 'web/tools/native_deps.py']
    signature = hashlib.sha256(b''.join(path.read_bytes() for path in inputs) +
                               json.dumps(entry, sort_keys=True).encode()).hexdigest()
    stamp = work / 'installed.json'
    destination = prefix / 'lib/blender-web-numpy'
    header = prefix / 'include/blender-web-numpy.hh'
    cached = (stamp.exists() and json.loads(stamp.read_text()).get('signature') == signature
              and header.exists() and (destination / 'libraries.cmake').exists()
              and (prefix / 'lib/python3.13/site-packages/numpy/__init__.py').exists())
    if cached and (prefix / 'share/blender-web/licenses/numpy/LICENSES_bundled.txt').exists():
        print('NumPy: cached', flush=True)
        return json.loads(stamp.read_text())
    archive = download(entry, base / 'deps/downloads')
    source = work / ('numpy-' + entry['VERSION'])
    if not source.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(work, filter='data')
    install_notices(source, prefix, 'numpy')
    if cached:
        print('NumPy: cached; restored dependency notices', flush=True)
        return json.loads(stamp.read_text())
    patch = ROOT / 'web/native/numpy-static.patch'
    if subprocess.run(['patch','--silent','--dry-run','--forward','-p1','-i',str(patch)],cwd=source).returncode == 0:
        subprocess.run(['patch','--silent','--forward','-p1','-i',str(patch)],cwd=source,check=True)
    elif subprocess.run(['patch','--silent','--dry-run','--reverse','-p1','-i',str(patch)],cwd=source).returncode:
        raise RuntimeError('NumPy static build patch does not apply')
    # Legacy RandomState compiles a second copy of distributions.c using
    # 32-bit C longs. Keep it private, as separate shared modules normally do.
    declarations = (source / 'numpy/_core/include/numpy/random/distributions.h').read_text()
    declarations += (source / 'numpy/random/src/distributions/distributions.c').read_text()
    legacy_symbols = sorted(set(re.findall(r'\b(random_\w+)\s*\(', declarations)))
    (source / 'numpy/random/blender-web-legacy.h').write_text(
        '\n'.join(f'#define {name} blender_web_legacy_{name}' for name in legacy_symbols) + '\n')
    if not shutil.which('uv'):
        raise RuntimeError('NumPy build requires uv to install pinned host build tools')
    tools = base / 'numpy-tools'
    if not (tools / 'bin/python').exists():
        subprocess.run(['uv','venv','--python',str(base / 'gpu-deps/build/python-native/python'),str(tools)],check=True)
    subprocess.run(['uv','pip','install','--python',str(tools / 'bin/python'),
                    'cython==3.0.11','ninja==1.13.2'],check=True)
    cross = work / 'emscripten.cross'
    flags = (C_FLAGS + f' -I{prefix / "include/python3.13"} -DPY_CALL_TRAMPOLINE').split()
    cross.write_text('''[binaries]
c = 'emcc'
cpp = 'em++'
ar = 'emar'
strip = 'emstrip'
[built-in options]
''' + f'c_args = {flags!r}\ncpp_args = {flags!r}\n' + '''[properties]
needs_exe_wrapper = true
longdouble_format = 'IEEE_QUAD_LE'
[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm'
endian = 'little'
''')
    env = dict(os.environ, EMSDK_PYTHON=shutil.which('python3'),
               PATH=str(tools / 'bin') + os.pathsep + os.environ['PATH'],
               BLENDER_WEB_STATIC_NUMPY='1',
               BLENDER_WEB_PYTHON_INCLUDE=str(prefix / 'include/python3.13'))
    meson = [str(tools / 'bin/python'), str(source / 'vendored-meson/meson/meson.py')]
    log = work / 'build.log'
    build_dir = work / 'build'
    command = [*meson,'setup',str(build_dir),str(source),'--cross-file',str(cross),
               f'--prefix={prefix}','--buildtype=release','-Dblas=none','-Dlapack=none',
               '-Dcpu-baseline=none','-Dcpu-dispatch=none','-Ddisable-highway=true',
               '-Ddisable-intel-sort=true','-Ddisable-svml=true',
               '-Dpython.install_env=prefix','-Dpython.platlibdir=lib/python3.13/site-packages',
               '-Dpython.purelibdir=lib/python3.13/site-packages']
    if (build_dir / 'build.ninja').exists(): command.append('--reconfigure')
    run(command, log, env=env)
    run([*meson,'compile','-C',str(build_dir),'-j',str(jobs)],log,env=env)
    run([*meson,'install','-C',str(build_dir),'--no-rebuild','--tags','runtime,python-runtime,devel'],log,env=env)
    destination.mkdir(parents=True,exist_ok=True)
    # Module archives and their private math/dispatch helpers retain Meson's
    # actual object files. No dynamic loader or replacement numeric code.
    libraries = []
    modules = []
    for library in sorted(build_dir.rglob('*.a')):
        name = str(library.relative_to(build_dir)).replace('/', '__')
        output = destination / name
        output.unlink(missing_ok=True)
        # Meson produces thin archives referencing build-tree objects. Flatten
        # them before installing so a cached prefix is self-contained.
        subprocess.run(['emar','qcsL',str(output),str(library)],check=True)
        libraries.append(name)
        symbols = subprocess.check_output(['emnm','--defined-only',str(library)],text=True)
        initializers = re.findall(r'\bT (PyInit_\w+)\b',symbols)
        for initializer in initializers:
            if 'tests' in initializer or initializer == 'PyInit__simd': continue
            package = '.'.join(library.relative_to(build_dir).parts[:-1])
            modules.append((package + '.' + initializer.removeprefix('PyInit_'),initializer))
    required = {'numpy._core._multiarray_umath','numpy.linalg._umath_linalg','numpy.fft._pocketfft_umath','numpy.random._generator'}
    if not required.issubset({name for name,_ in modules}):
        raise RuntimeError(f'Missing NumPy static modules: {modules}')
    header.write_text('/* SPDX-License-Identifier: GPL-2.0-or-later */\n#pragma once\n#include <Python.h>\n#include <cstdlib>\n' +
                      '\n'.join(f'extern "C" PyObject *{init}();' for _,init in modules) +
                      '\ninline void GHOST_web_register_numpy()\n{\n' +
                      '\n'.join(f'  if (PyImport_AppendInittab("{name}", {init}) < 0) std::abort();' for name,init in modules) + '\n}\n')
    (destination / 'libraries.cmake').write_text('set(BLENDER_WEB_NUMPY_LIBRARIES\n' +
        '\n'.join(f'  "${{CMAKE_CURRENT_LIST_DIR}}/{name}"' for name in libraries) + '\n)\n')
    report = {'signature':signature,'version':entry['VERSION'],'modules':[name for name,_ in modules],
              'archives':len(libraries),'staticBytes':sum((destination / name).stat().st_size for name in libraries)}
    stamp.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report),flush=True)
    return report


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,default=ROOT / 'build/native')
    parser.add_argument('--jobs',type=int,default=8)
    args = parser.parse_args()
    build(args.build_dir.resolve(),args.jobs)
