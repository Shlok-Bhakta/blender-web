#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check owned JIT data addresses using installed LLVM and offline Chromium."""
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
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1 or not shutil.which('emcmake'):
        parser.error('Activate the pinned SDK and provide a positive job count')
    sdk_version = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    if sdk_version != json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']:
        parser.error('Activate the pinned Emscripten SDK')
    base = args.build_dir.resolve()
    prefix, work = base / 'deps/prefix', base / 'jit-data-check'
    work.mkdir(parents=True, exist_ok=True)
    inputs = ['web/tools/native_jit_data_check.py', 'web/native/jit-data-check/CMakeLists.txt',
              'web/native/tests/jit-data.cc', 'web/tests/jit-data.cjs',
              'web/native/ghost/GHOST_WebJITProbe.cc', 'web/native/ghost/GHOST_WebJITProbe.hh',
              'web/native/ghost/GHOST_WebOSLOwnedProbe.cc', 'web/native/ghost/GHOST_WebOSLOwnedProbe.hh',
              'web/native/ghost/GHOST_WebOSL.hh', 'web/native/ghost/GHOST_WebWorkerResources.hh',
              'web/native/ghost/GHOST_WebFFI.cc', 'web/native/ghost/GHOST_WebFFI.hh']
    hashes = {name: digest(ROOT / name) for name in inputs}
    stamps = {name: digest(base / f'deps/{name}/installed.json') for name in ('llvm', 'ffi')}
    sdk = Path(shutil.which('emcc')).resolve().parent
    log = work / 'build.log'
    log.write_text('')
    env = dict(os.environ, BINARYEN_CORES=os.environ.get('BINARYEN_CORES', '1'))
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/jit-data-check', '-B', work / 'wasm',
         '-DCMAKE_BUILD_TYPE=Release', f'-DBLENDER_WEB_PREFIX={prefix}',
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
         '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY',
         f'-DSDK_CRTBEGIN={sdk / "cache/sysroot/lib/wasm32-emscripten/pic/crtbegin.o"}'], log, env=env)
    run(['cmake', '--build', work / 'wasm', '--parallel', args.jobs], log, env=env)
    run(['node', ROOT / 'web/tests/jit-data.cjs', work / 'wasm', work / 'evidence'], log, env=env)
    report = json.loads((work / 'evidence/acceptance.json').read_text())
    assert report['node'] == report['browser'] and report['offline'] and not report['browserErrors']
    assert hashes == {name: digest(ROOT / name) for name in inputs}, 'Sources changed during validation'
    assert stamps == {name: digest(base / f'deps/{name}/installed.json') for name in stamps}
    report.update(emscriptenVersion=sdk_version, sourceHashes=hashes,
                  installedDependencyStamps=stamps, applicationIntegrated=False,
                  artifacts={str(path.relative_to(base)): dict(bytes=path.stat().st_size, sha256=digest(path))
                             for path in [work / 'wasm/jit-data.js', work / 'wasm/jit-data.wasm',
                                          work / 'evidence/jit-data.png']})
    (work / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
