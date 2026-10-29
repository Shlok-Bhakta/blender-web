#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify original OSL EXR textures on workers with independent native readback."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from native_deps import run
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native/osl-textures-probe')
    parser.add_argument('--osl-build-dir', type=Path, default=ROOT / 'build/native/osl-closures-probe')
    parser.add_argument('--compiler-build-dir', type=Path, default=ROOT / 'build/native/osl-query-probe')
    parser.add_argument('--llvm-build-dir', type=Path, default=ROOT / 'build/native/llvm-probe')
    parser.add_argument('--prefix', type=Path, default=ROOT / 'build/native/deps/prefix')
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or not all(shutil.which(name) for name in ('emcmake', 'c++', args.blender)):
        parser.error('Activate the pinned SDK and provide native Blender and a C++ compiler')
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    if subprocess.check_output(['emcc', '-dumpversion'], text=True).strip() != expected:
        parser.error('Use the pinned SDK')
    base, osl, compiler, llvm, prefix = [value.resolve() for value in
        (args.build_dir, args.osl_build_dir, args.compiler_build_dir, args.llvm_build_dir, args.prefix)]
    prerequisite = json.loads((osl / 'acceptance.json').read_text())
    if prerequisite['emscriptenVersion'] != expected or prerequisite['llvmVersion'] != '20.1.8':
        parser.error('First reproduce the original OSL closure proof')
    for name, checksum in prerequisite['sourceHashes'].items():
        if digest(ROOT / name) != checksum:
            parser.error(f'Reproduce the closure proof after changing {name}')
    libraries = ['osl-wasm/lib/liboslexec.a', 'osl-wasm/lib/liboslquery.a',
                 'osl-wasm/lib/liboslnoise.a']
    # The closure proof records its execution archive. Also fingerprint the two
    # companion archives used here so subsequent shader results identify them.
    archive = libraries[0]
    if digest(osl / archive) != prerequisite['artifacts'][archive]['sha256']:
        parser.error('The qualified OSL execution archive changed')
    native_ffi = osl / 'native-ffi'
    for name, checksum in [('lib/libffi.a', 'archiveSha256'),
                           ('include/ffi.h', 'ffiHeaderSha256'),
                           ('include/ffitarget.h', 'targetHeaderSha256')]:
        if digest(native_ffi / name) != prerequisite['nativeFFI'][checksum]:
            parser.error('The qualified native libffi files changed')
    base.mkdir(parents=True, exist_ok=True)
    log = base / 'texture-proof-build.log'
    log.write_text('')
    env = dict(os.environ)
    for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'EM_PKG_CONFIG_PATH'):
        env.pop(name, None)
    sdk = Path(shutil.which('emcc')).resolve().parent
    executable = base / 'runtime'
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/osl-textures-probe', '-B', executable,
         '-DCMAKE_BUILD_TYPE=Release', f'-DOSL_WORK={osl}', f'-DOSL_COMPILER_WORK={compiler}',
         f'-DOSL_PREFIX={prefix}', f'-DCMAKE_PREFIX_PATH={prefix}',
         f'-DCMAKE_FIND_ROOT_PATH={prefix};{llvm / "wasm"}', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY',
         f'-DLLVM_DIR={llvm / "wasm/lib/cmake/llvm"}', f'-DLLD_DIR={llvm / "wasm/lib/cmake/lld"}',
         f'-DSDK_CRTBEGIN={sdk / "cache/sysroot/lib/wasm32-emscripten/pic/crtbegin.o"}'], log, env=env)
    run(['cmake', '--build', executable, '--parallel', args.jobs], log, env=env)
    output = base / 'evidence'
    run(['node', ROOT / 'web/tests/osl-textures.cjs', executable, compiler, output], log, env=env)
    report = json.loads((output / 'acceptance.json').read_text())
    fixture = ROOT / 'web/native/tests/fixtures/osl-textures/web_execute.osl'
    run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
         '--python', ROOT / 'web/native/tests/osl-runtime-native.py', '--', fixture,
         output / 'native-fixtures'], log, env=env)
    native_libs = Path(shutil.which(args.blender)).resolve().parent / 'lib'
    native_libraries = [native_libs / name for name in
        ('liboslexec.so.1.15.3', 'libOpenImageIO.so.3.1.13', 'libOpenImageIO_Util.so.3.1.13')]
    native_binary = base / 'osl-native'
    run(['c++', '-std=c++20', '-pthread', '-O2', f'-I{native_ffi / "include"}',
         f'-I{osl / "osl/src/include"}', f'-I{osl / "osl-wasm/include"}',
         f'-I{prefix / "include"}', f'-I{ROOT / "web/native/ghost"}',
         ROOT / 'web/native/tests/osl-textures.cc', *native_libraries,
         native_ffi / 'lib/libffi.a', f'-Wl,-rpath,{native_libs}', '-o', native_binary], log, env=env)
    native = {}
    for name in ('native', 'node', 'browser'):
        result = subprocess.check_output([native_binary, output / (name + '-fixtures')],
                                         env=env, text=True, timeout=180)
        native[name] = json.loads(result.strip())
        if native[name] != native['native'] or not native[name]['nativeTextureSamples']:
            raise RuntimeError(f'{name} bytecode failed native texture comparison')
    report.update(source=prerequisite['source'], emscriptenVersion=expected,
                  llvmVersion='20.1.8', native=native, nativeFFI=prerequisite['nativeFFI'],
                  oslLibraryHashes={name: digest(osl / name) for name in libraries},
                  nativeLibraries={p.name: digest(p) for p in native_libraries},
                  prerequisiteSha256=digest(osl / 'acceptance.json'))
    files = [executable / 'osl-runtime.js', executable / 'osl-runtime.wasm', native_binary,
             output / 'osl-runtime.png', output / 'osl-runtime.webm']
    files += sorted(output.glob('*-fixtures/web_execute.oso'))
    files += sorted(output.glob('*-fixtures/texture.exr'))
    report['artifacts'] = {str(p.relative_to(base)): dict(bytes=p.stat().st_size, sha256=digest(p))
                           for p in files}
    sources = ['web/tools/native_osl_textures_probe.py', 'web/native/tests/osl-textures.cc',
               'web/native/osl-textures-probe/CMakeLists.txt', 'web/tests/osl-textures.cjs',
               'web/native/tests/osl-runtime-native.py',
               'web/native/tests/fixtures/osl-textures/web_execute.osl']
    report['sourceHashes'] = {name: digest(ROOT / name) for name in sources}
    (base / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(browser=report['browser']['result'], native=native,
                          browserErrors=report['browserErrors'], offline=report['browser']['offline']), indent=2))


if __name__ == '__main__':
    main()
