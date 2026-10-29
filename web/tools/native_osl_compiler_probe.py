#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile original OSL shaders offline and verify them with native Cycles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from native_deps import run
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native/osl-query-probe')
    parser.add_argument('--llvm-build-dir', type=Path, default=ROOT / 'build/native/llvm-probe')
    parser.add_argument('--prefix', type=Path, default=ROOT / 'build/native/deps/prefix')
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--flex', default=shutil.which('flex'))
    parser.add_argument('--bison', default=shutil.which('bison'))
    parser.add_argument('--m4', default=shutil.which('m4'))
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or not all((args.flex, args.bison, args.m4, shutil.which(args.blender),
                                shutil.which('emcmake'))):
        parser.error('Activate the pinned SDK and provide native Blender, Flex, Bison and m4')
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    if subprocess.check_output(['emcc', '-dumpversion'], text=True).strip() != expected:
        parser.error('Use the pinned Emscripten SDK')
    base, llvm = args.build_dir.resolve(), args.llvm_build_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    log = base / 'compiler-proof-build.log'
    log.write_text('')
    env = dict(os.environ)
    query_args = [sys.executable, ROOT / 'web/tools/native_osl_query_probe.py']
    for name in ('build_dir', 'llvm_build_dir', 'prefix', 'blender', 'flex', 'bison', 'm4', 'jobs'):
        query_args += ['--' + name.replace('_', '-'), getattr(args, name)]
    run(query_args, log, env=env)
    # Clang and LLVM execute inside Wasm. The native TableGen programs only
    # generate build-time tables; shaders are never sent to a compiler service.
    run(['cmake', '--build', llvm / 'wasm', '--target', 'clangFrontend',
         '--parallel', args.jobs], log, env=env)
    env['M4'] = str(Path(args.m4).resolve())
    data = Path(args.bison).resolve().parents[1] / 'share/bison'
    if data.is_dir():
        env['BISON_PKGDATADIR'] = str(data)
    run(['cmake', '--build', base / 'osl-wasm', '--target', 'oslc', '--parallel', args.jobs], log, env=env)
    output = base / 'oslc-evidence'
    run(['node', ROOT / 'web/tests/osl-compiler.cjs', base / 'osl-wasm/bin', output, base], log, env=env)
    run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
         '--python', ROOT / 'web/native/tests/osl-compiler-native.py', '--', base, output], log, env=env)
    report = json.loads((output / 'acceptance.json').read_text())
    report['native'] = json.loads((output / 'native-verification.json').read_text())
    report['source'] = json.loads((base / 'osl-source-lock.json').read_text())['OSL']
    report['emscriptenVersion'] = expected
    report['llvmVersion'] = '20.1.8'
    report['patchSha256'] = digest(ROOT / 'web/native/patches/osl.patch')
    files = [base / 'osl-wasm/bin/oslc.js', base / 'osl-wasm/bin/oslc.wasm',
             base / 'osl-wasm/lib/liboslcomp.a', output / 'native-render.exr',
             output / 'browser-render.exr', *sorted(output.glob('*.oso'))]
    report['artifacts'] = {str(p.relative_to(base)): dict(bytes=p.stat().st_size, sha256=digest(p))
                           for p in files}
    sources = ['web/tools/native_osl_compiler_probe.py', 'web/tools/native_osl_query_probe.py',
               'web/native/tests/osl-compiler-native.py', 'web/tests/osl-compiler.cjs',
               'web/native/tests/fixtures/osl-query/web_query.osl', 'web/native/patches/osl.patch',
               'web/native/osl-query-probe/cmake/FindLLVM.cmake',
               'web/native/osl-query-probe/cmake/OSLDependencies.cmake']
    report['sourceHashes'] = {name: digest(ROOT / name) for name in sources}
    (base / 'osl-compiler.acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(offline=report['browser']['offline'], browserErrors=report['browserErrors'],
                          native=report['native'], wasm=report['artifacts']['osl-wasm/bin/oslc.wasm']),
                     indent=2))


if __name__ == '__main__':
    main()
