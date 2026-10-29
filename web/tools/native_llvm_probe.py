#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build original LLVM/LLD for a separate browser compiler proof, outside Blender."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

from native_deps import download, run
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native/llvm-probe')
    parser.add_argument('--downloads-dir', type=Path, default=ROOT / 'build/native/deps/downloads')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--skip-libraries', action='store_true', help='Reuse existing pinned host tools and Wasm libraries')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if not shutil.which('emcmake'):
        parser.error('Activate the pinned Emscripten SDK first')
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    actual = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    if actual != expected:
        parser.error(f'Expected Emscripten {expected}, found {actual}')
    base = args.build_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    downloads = args.downloads_dir.resolve()
    downloads.mkdir(parents=True, exist_ok=True)
    lock_path = base / 'source-lock.json'
    subprocess.run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}', '-DDEPENDENCIES=LLVM',
                    f'-DOUTPUT={lock_path}', '-P', ROOT / 'web/native/dependency-lock.cmake'], check=True)
    lock = json.loads(lock_path.read_text())['LLVM']
    source = base / 'llvm'
    stamp = base / 'llvm-source.json'
    if source.exists():
        if not stamp.exists() or json.loads(stamp.read_text()) != lock:
            raise RuntimeError('Existing LLVM sources lack the matching source stamp; preserve and inspect them before reuse')
    else:
        archive = download(lock, downloads)
        with tempfile.TemporaryDirectory(dir=base, prefix='extract-') as temporary:
            with tarfile.open(archive) as bundle:
                bundle.extractall(temporary, filter='data')
            roots = list(Path(temporary).iterdir())
            if len(roots) != 1 or not (roots[0] / 'llvm/CMakeLists.txt').is_file():
                raise RuntimeError('Unexpected LLVM release archive layout')
            shutil.move(roots[0], source)
        stamp.write_text(json.dumps(lock, indent=2) + '\n')
    env = dict(os.environ)
    for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'EM_PKG_CONFIG_PATH'):
        env.pop(name, None)
    host, wasm = base / 'host', base / 'wasm'
    common = ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17',
              '-DLLVM_TARGETS_TO_BUILD=WebAssembly', '-DLLVM_ENABLE_EH=ON', '-DLLVM_ENABLE_RTTI=ON',
              '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
              '-DLLVM_BUILD_DOCS=OFF', '-DLLVM_ENABLE_BINDINGS=OFF', '-DLLVM_ENABLE_ZLIB=OFF',
              '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
              '-DLLVM_ENABLE_Z3_SOLVER=OFF']
    if not args.skip_libraries:
        log = base / 'libraries-build.log'
        log.write_text('')
        run(['cmake', '-G', 'Unix Makefiles', '-S', source / 'llvm', '-B', host,
             f'-DCMAKE_C_COMPILER={shutil.which("cc")}', f'-DCMAKE_CXX_COMPILER={shutil.which("c++")}',
             '-DLLVM_ENABLE_PROJECTS=clang;lld', *common], log, env=env)
        run(['cmake', '--build', host, '--target', 'llvm-tblgen', '--parallel', args.jobs], log, env=env)
        flags = '-pthread -msimd128 -fwasm-exceptions'
        run(['emcmake', 'cmake', '-G', 'Unix Makefiles', '-S', source / 'llvm', '-B', wasm,
             *common, '-DLLVM_ENABLE_PROJECTS=lld', f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags}',
             '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG',
             '-DCMAKE_EXE_LINKER_FLAGS=-pthread -fwasm-exceptions -sALLOW_MEMORY_GROWTH=1',
             '-DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten', '-DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-unknown-emscripten',
             '-DLLVM_TARGET_ARCH=wasm32', '-DLLVM_ENABLE_THREADS=ON', '-DLLVM_ENABLE_BACKTRACES=OFF',
             '-DLLVM_ENABLE_CRASH_OVERRIDES=OFF', f'-DLLVM_TABLEGEN={host / "bin/llvm-tblgen"}',
             '-DLLVM_BUILD_TOOLS=OFF', '-DLLVM_BUILD_UTILS=OFF', '-DLLVM_ENABLE_LIBPFM=OFF',
             '-DLLVM_ENABLE_FFI=OFF', '-DBUILD_SHARED_LIBS=OFF'], log, env=env)
        run(['cmake', '--build', wasm, '--target', 'LLVMAsmParser', 'LLVMPasses',
             'LLVMWebAssemblyCodeGen', 'LLVMWebAssemblyAsmParser', 'lldWasm', '--parallel', args.jobs], log, env=env)
    executable = base / 'probe'
    log = base / 'probe-build.log'
    log.write_text('')
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/llvm-probe', '-B', executable,
         '-DCMAKE_BUILD_TYPE=Release', f'-DLLVM_DIR={wasm / "lib/cmake/llvm"}',
         f'-DLLD_DIR={wasm / "lib/cmake/lld"}'], log, env=env)
    run(['cmake', '--build', executable, '--parallel', args.jobs], log, env=env)
    run(['node', ROOT / 'web/tests/llvm-generated.cjs', executable], log, env=env)
    disassembler = Path(shutil.which('emcc')).resolve().parents[1] / 'bin/wasm-dis'
    assembly = subprocess.check_output([disassembler, executable / 'generated-shader-0.wasm'], text=True)
    if 'f32x4.mul' not in assembly or 'f32x4.add' not in assembly:
        raise RuntimeError('Generated shader did not retain actual Wasm SIMD instructions')
    run(['node', ROOT / 'web/tests/llvm-compiler.cjs', executable, base / 'browser-evidence'], log, env=env)
    proof = json.loads((base / 'browser-evidence/acceptance.json').read_text())
    proof['source'] = lock
    proof['emscriptenVersion'] = actual
    proof['applicationIntegrated'] = False
    proof['nodeVerified'] = True
    proof['generatedSIMDInstructionsVerified'] = True
    paths = ['web/native/tests/llvm-compiler.cc', 'web/native/llvm-probe/CMakeLists.txt',
             'web/tests/llvm-generated.cjs', 'web/tests/llvm-compiler.cjs', 'web/tools/native_llvm_probe.py']
    proof['sourceHashes'] = {name: digest(ROOT / name) for name in paths}
    proof['compilerJavaScript'] = dict(bytes=(executable / 'llvm-compiler.js').stat().st_size,
                                     sha256=digest(executable / 'llvm-compiler.js'))
    (base / 'acceptance.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps(proof, indent=2))


if __name__ == '__main__':
    main()
