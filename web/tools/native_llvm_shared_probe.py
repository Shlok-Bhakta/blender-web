#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test LLVM-generated Wasm against the caller's shared heap and pthreads."""
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
    parser.add_argument('--llvm-build-dir', type=Path, default=ROOT / 'build/native/llvm-probe')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native/llvm-shared-probe')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or not shutil.which('emcmake'):
        parser.error('Activate the pinned SDK and use a positive job count')
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    if subprocess.check_output(['emcc', '-dumpversion'], text=True).strip() != expected:
        parser.error('Use the pinned Emscripten SDK')
    llvm, base = args.llvm_build_dir.resolve(), args.build_dir.resolve()
    prerequisite = json.loads((llvm / 'acceptance.json').read_text())
    if prerequisite['llvmVersion'] != '20.1.8' or prerequisite['emscriptenVersion'] != expected:
        parser.error('First build the original LLVM proof with the pinned SDK')
    sdk = Path(shutil.which('emcc')).resolve().parent
    crt = sdk / 'cache/sysroot/lib/wasm32-emscripten/pic/crtbegin.o'
    if not crt.is_file():
        parser.error('The pinned SDK PIC startup object is missing')
    base.mkdir(parents=True, exist_ok=True)
    log = base / 'build.log'
    log.write_text('')
    env = dict(os.environ)
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/llvm-shared-probe', '-B', base,
         '-DCMAKE_BUILD_TYPE=Release', f'-DLLVM_DIR={llvm / "wasm/lib/cmake/llvm"}',
         f'-DLLD_DIR={llvm / "wasm/lib/cmake/lld"}', f'-DSDK_CRTBEGIN={crt}'], log, env=env)
    run(['cmake', '--build', base, '--parallel', args.jobs], log, env=env)
    run(['node', ROOT / 'web/tests/llvm-shared.cjs', base, base / 'browser-evidence'], log, env=env)
    report = json.loads((base / 'browser-evidence/acceptance.json').read_text())
    report['source'] = prerequisite['source']
    report['emscriptenVersion'] = expected
    report['sdkStartupObjectSha256'] = digest(crt)
    sources = ['web/tools/native_llvm_shared_probe.py', 'web/native/llvm-shared-probe/CMakeLists.txt',
               'web/native/tests/llvm-shared.cc', 'web/native/tests/llvm-shared-check.hh',
               'web/tests/llvm-shared.cjs']
    report['sourceHashes'] = {name: digest(ROOT / name) for name in sources}
    report['artifacts'] = {p.name: dict(bytes=p.stat().st_size, sha256=digest(p))
                           for p in (base / 'llvm-compiler.js', base / 'llvm-compiler.wasm')}
    (base / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
