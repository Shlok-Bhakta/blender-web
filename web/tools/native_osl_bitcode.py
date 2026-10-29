#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate LLVM 20 helper IR with the pinned SDK's actual Wasm ABI headers."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    args, rest = parser.parse_known_args()
    if rest and rest[0] == '--':
        rest = rest[1:]
    compiler, sdk = args.compiler.resolve(), args.sdk.resolve()
    resource = Path(subprocess.check_output([compiler, '-print-resource-dir'], text=True).strip())
    sysroot = sdk / 'cache/sysroot'
    includes = [sysroot / 'include/c++/v1',
                resource / 'include', sysroot / 'include', sysroot / 'include/compat']
    if not all(p.is_dir() for p in includes):
        parser.error('The pinned Clang resource headers or SDK sysroot headers are missing')
    target_cxx = sysroot / 'include/wasm32-emscripten/c++/v1'
    if target_cxx.is_dir():
        includes.insert(0, target_cxx)
    target_c = sysroot / 'include/wasm32-emscripten'
    if target_c.is_dir():
        includes.insert(includes.index(sysroot / 'include'), target_c)
    flags = ['--target=wasm32-unknown-emscripten', '-nostdinc', '-nostdinc++',
             f'--sysroot={sysroot}', '-DEMSCRIPTEN', '-D__EMSCRIPTEN_SHARED_MEMORY__=1',
             '-pthread', '-msimd128', '-fwasm-exceptions', '-fPIC']
    for path in includes:
        flags += ['-isystem', str(path)]
    os.execv(compiler, [str(compiler), *flags, *rest])


if __name__ == '__main__':
    main()
