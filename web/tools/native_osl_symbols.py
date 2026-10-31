#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the static symbol table that OSL-generated Wasm programs import.

The application is not a dynamic-linking main module, so `dlsym` cannot find
host symbols. OSL links its precompiled ops bitcode into every shader group;
compiling that bitcode for wasm32 lists every external symbol a group can
import, with the exact Wasm signature of each function.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

from prepare import ROOT

OUTPUT = ROOT / 'web/native/ghost/GHOST_WebOSLSymbols.inc'
BITCODE = ['osl_llvm_compiled_ops.bc', 'osl_llvm_compiled_rs_dependent_ops.bc']
# The loader supplies these per program instance.
LOADER = {'__indirect_function_table', '__memory_base', '__table_base', '__stack_pointer', 'memory'}
# LLVM's Wasm backend lowers these intrinsics to libcalls; generated IR may use them
# even when the ops bitcode does not.
LIBCALLS = {
    'roundf': 'f32:f32', 'round': 'f64:f64', 'fmodf': 'f32,f32:f32', 'fmod': 'f64,f64:f64',
    'powf': 'f32,f32:f32', 'pow': 'f64,f64:f64', 'expf': 'f32:f32', 'exp': 'f64:f64',
    'exp2f': 'f32:f32', 'exp2': 'f64:f64', 'logf': 'f32:f32', 'log': 'f64:f64',
    'log2f': 'f32:f32', 'log2': 'f64:f64', 'log10f': 'f32:f32', 'log10': 'f64:f64',
    'sinf': 'f32:f32', 'sin': 'f64:f64', 'cosf': 'f32:f32', 'cos': 'f64:f64',
    'tanf': 'f32:f32', 'tan': 'f64:f64', 'asinf': 'f32:f32', 'asin': 'f64:f64',
    'acosf': 'f32:f32', 'acos': 'f64:f64', 'atanf': 'f32:f32', 'atan': 'f64:f64',
    'atan2f': 'f32,f32:f32', 'atan2': 'f64,f64:f64', 'sinhf': 'f32:f32', 'sinh': 'f64:f64',
    'coshf': 'f32:f32', 'cosh': 'f64:f64', 'tanhf': 'f32:f32', 'tanh': 'f64:f64',
    'fmaf': 'f32,f32,f32:f32', 'fma': 'f64,f64,f64:f64', 'ldexpf': 'f32,i32:f32',
    'ldexp': 'f64,i32:f64', 'frexpf': 'f32,i32:f32', 'frexp': 'f64,i32:f64',
    'memcpy': 'i32,i32,i32:i32', 'memmove': 'i32,i32,i32:i32', 'memset': 'i32,i32,i32:i32',
}
TYPES = {0x7f: 'i32', 0x7e: 'i64', 0x7d: 'f32', 0x7c: 'f64', 0x7b: 'v128'}
C_TYPES = {'i32': 'int32_t', 'i64': 'int64_t', 'f32': 'float', 'f64': 'double'}


def leb(data, offset):
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if not byte & 0x80:
            return value, offset


def name(data, offset):
    size, offset = leb(data, offset)
    return data[offset:offset + size].decode(), offset + size


def imports(path):
    """Return {symbol: 'params:results'} for functions and {symbol: None} for data."""
    data = Path(path).read_bytes()
    if data[:8] != b'\0asm\1\0\0\0':
        raise RuntimeError(f'{path} is not a Wasm object')
    offset, signatures, found = 8, [], {}
    while offset < len(data):
        section, offset = data[offset], offset + 1
        size, offset = leb(data, offset)
        end = offset + size
        if section == 1:
            count, cursor = leb(data, offset)
            for _ in range(count):
                if data[cursor] != 0x60:
                    raise RuntimeError('Unexpected Wasm type form')
                cursor += 1
                params, cursor = leb(data, cursor)
                param_types = [TYPES[data[cursor + i]] for i in range(params)]
                cursor += params
                results, cursor = leb(data, cursor)
                result_types = [TYPES[data[cursor + i]] for i in range(results)]
                cursor += results
                signatures.append(','.join(param_types) + ':' + ','.join(result_types))
        elif section == 2:
            count, cursor = leb(data, offset)
            for _ in range(count):
                module, cursor = name(data, cursor)
                field, cursor = name(data, cursor)
                kind, cursor = data[cursor], cursor + 1
                if kind == 0:
                    index, cursor = leb(data, cursor)
                    if module == 'env':
                        found[field] = signatures[index]
                elif kind == 1:
                    cursor += 1
                    flags, cursor = leb(data, cursor)
                    _, cursor = leb(data, cursor)
                    if flags & 1:
                        _, cursor = leb(data, cursor)
                elif kind == 2:
                    flags, cursor = leb(data, cursor)
                    _, cursor = leb(data, cursor)
                    if flags & 1:
                        _, cursor = leb(data, cursor)
                elif kind == 3:
                    cursor += 2
                    if module == 'GOT.mem':
                        found.setdefault(field, None)
                    elif module == 'GOT.func':
                        found.setdefault(field, 'address')
                else:
                    raise RuntimeError(f'Unsupported import kind {kind}')
        offset = end
    return found


def generate(osl_build, compiler):
    sources = [Path(osl_build) / 'src/liboslexec' / name for name in BITCODE]
    symbols, hashes = {}, {}
    with tempfile.TemporaryDirectory() as temporary:
        for source in sources:
            obj = Path(temporary) / (source.stem + '.o')
            subprocess.run([compiler, '--target=wasm32-unknown-emscripten', '-msimd128',
                            '-mbulk-memory', '-matomics', '-fPIC', '-O0', '-c', source, '-o', obj],
                           check=True)
            hashes[source.name] = hashlib.sha256(source.read_bytes()).hexdigest()
            for symbol, kind in imports(obj).items():
                if kind == 'address' and symbols.get(symbol):
                    continue
                symbols[symbol] = kind
    unresolved = [s for s, kind in symbols.items() if kind == 'address']
    if unresolved:
        raise RuntimeError(f'Address-only imports need a declared signature: {unresolved}')
    for symbol, signature in LIBCALLS.items():
        if symbols.setdefault(symbol, signature) != signature:
            raise RuntimeError(f'{symbol} signature differs from the ops bitcode')
    lines = ['/* Generated by web/tools/native_osl_symbols.py. Do not edit. */']
    lines += [f'/* {name} {digest} */' for name, digest in sorted(hashes.items())]
    entries = []
    for index, symbol in enumerate(sorted(s for s in symbols if s not in LOADER)):
        alias = f'ghost_web_osl_import_{index}'
        signature = symbols[symbol]
        if signature is None:
            lines.append(f'extern "C" char {alias} __asm__("{symbol}");')
        else:
            params, results = signature.split(':')
            result = C_TYPES[results] if results else 'void'
            args = ', '.join(C_TYPES[p] for p in params.split(',')) if params else 'void'
            lines.append(f'extern "C" {result} {alias}({args}) __asm__("{symbol}");')
        function = 'false' if signature is None else 'true'
        entries.append(f'    {{"{symbol}", reinterpret_cast<void *>(&{alias}), {function}}},')
    lines += ['#define GHOST_WEB_OSL_SYMBOL_ENTRIES \\']
    lines += [entry + ' \\' for entry in entries[:-1]] + [entries[-1]]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--osl-build', type=Path, default=ROOT / 'build/native/deps/osl/build')
    parser.add_argument('--compiler', default='clang')
    parser.add_argument('--check', action='store_true', help='Fail if the committed table differs')
    args = parser.parse_args()
    text = generate(args.osl_build, args.compiler)
    if args.check:
        if OUTPUT.read_text() != text:
            raise SystemExit(f'{OUTPUT.relative_to(ROOT)} is out of date')
        return
    OUTPUT.write_text(text)
    print(f'Wrote {OUTPUT.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
