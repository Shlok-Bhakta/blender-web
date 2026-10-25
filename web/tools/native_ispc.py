#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lower OIDN's generic ISPC kernels to the pinned Emscripten SIMD target.

This is a compiler bridge for OIDN's 32-bit interfaces, not a general x86 IR
translator. Generic ISPC emits no native instructions. The Wasm compiler then
lowers its eight/sixteen-lane vectors into v128 operations.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

from native_ispc_half import lower_half

SSA = r'%(?:[-a-zA-Z$._0-9]+|"(?:[^"\\]|\\.)+")'


def lower_masks(source):
    # LLVM 5243501's WebAssembly performSETCCCombine casts a variable RHS to
    # ConstantSDNode. Express packed mask equality with v128.any_true instead.
    # https://github.com/llvm/llvm-project/blob/5243501cca02d7e54294d4bb5de0a85b06c40b7d/llvm/lib/Target/WebAssembly/WebAssemblyISelLowering.cpp
    pattern = re.compile(r'(?m)^(\s*)(' + SSA + r') = icmp (eq|ne) i(4|8|16) (' + SSA + r'), (' + SSA + r')\s*$')
    widths = set()
    count = 0

    def replace(match):
        nonlocal count
        lanes = int(match[4])
        bits = 128 // lanes
        widths.add(lanes)
        prefix = f'%webmask{count}'
        count += 1
        return f'''{match[1]}{prefix}a = bitcast i{lanes} {match[5]} to <{lanes} x i1>
  {prefix}b = bitcast i{lanes} {match[6]} to <{lanes} x i1>
  {prefix}x = xor <{lanes} x i1> {prefix}a, {prefix}b
  {prefix}v = sext <{lanes} x i1> {prefix}x to <{lanes} x i{bits}>
  {prefix}r = call i32 @llvm.wasm.anytrue.v{lanes}i{bits}(<{lanes} x i{bits}> {prefix}v)
  {match[2]} = icmp {match[3]} i32 {prefix}r, 0'''

    source = pattern.sub(replace, source)
    for lanes in sorted(widths):
        bits = 128 // lanes
        source += f'\ndeclare i32 @llvm.wasm.anytrue.v{lanes}i{bits}(<{lanes} x i{bits}>)\n'
    return source


def main(host_ispc=None):
    host_ispc = host_ispc or os.environ['BLENDER_WEB_ISPC']
    args = sys.argv[1:]
    if '-o' not in args:
        return subprocess.call([str(host_ispc), *args], timeout=30)
    if '--arch=x86' not in args or '--addressing=32' not in args or not any(
            value in args for value in ('--target=generic-i32x8', '--target=generic-i32x16')):
        raise ValueError('OIDN Wasm requires 32-bit generic ISPC interfaces')
    output = Path(args[args.index('-o') + 1])
    llvm = output.with_suffix('.ll')
    args[args.index('-o') + 1] = str(llvm)
    args = [arg for arg in args if arg not in ('--emit-obj', '--pic')]
    args += ['--emit-llvm-text', '-DOIDN_ARCH_WASM=1']
    subprocess.run([str(host_ispc), *args], check=True, timeout=90)
    header = Path(args[args.index('-h') + 1]).read_text() if '-h' in args else ''
    for fields in re.findall(r'struct[^{}]*\{([^{}]*)\}', header, re.DOTALL):
        if re.search(r'\b(?:double|int64_t|uint64_t)\b', fields):
            raise ValueError('OIDN interface requires an audited 64-bit field alignment')
    source = llvm.read_text()
    if re.search(r'\basm\b|@llvm\.(x86|aarch64)', source):
        raise ValueError('Native instruction in generic ISPC output')
    llvm.write_text(lower_half(lower_masks(source)))
    # timeout kills the complete compiler process group, including clang.
    subprocess.run(['timeout', '--kill-after=5', '90', 'emcc', '-O3', '-msimd128',
                    '-pthread', '-fwasm-exceptions', '-Wno-override-module',
                    '-c', str(llvm), '-o', str(output)], check=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
