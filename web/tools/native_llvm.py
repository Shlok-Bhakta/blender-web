#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build and install original LLVM/Clang/LLD libraries for the browser target."""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

from prepare import ROOT

COMMON = [
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17',
    '-DLLVM_TARGETS_TO_BUILD=WebAssembly', '-DLLVM_ENABLE_EH=ON', '-DLLVM_ENABLE_RTTI=ON',
    '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
    '-DLLVM_BUILD_DOCS=OFF', '-DLLVM_ENABLE_BINDINGS=OFF', '-DLLVM_ENABLE_ZLIB=OFF',
    '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF',
    '-DLLVM_ENABLE_Z3_SOLVER=OFF',
]
HOST_TARGETS = ['llvm-tblgen', 'clang-tblgen', 'clang', 'llvm-as', 'llvm-link']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cache(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line.split('=', 1)[0]:
            name, value = line.split('=', 1)
            result[name.split(':', 1)[0]] = value
    return result


def pinned_reuse(name, lock):
    value = os.environ.get(name)
    if not value:
        return None
    directory = Path(value).resolve()
    receipt = directory.parent / 'llvm-source.json'
    if not receipt.is_file() or json.loads(receipt.read_text()) != lock:
        raise RuntimeError(f'{name} must name a build with the matching original LLVM source receipt')
    return directory


def host_tools(source, directory, lock, jobs, log, run, env):
    reused = pinned_reuse('BLENDER_WEB_LLVM_HOST_BUILD', lock)
    host = reused or directory / 'host'
    if not reused:
        run(['cmake', '-G', 'Unix Makefiles', '-S', source / 'llvm', '-B', host,
             f'-DCMAKE_C_COMPILER={shutil.which("cc")}', f'-DCMAKE_CXX_COMPILER={shutil.which("c++")}',
             '-DLLVM_ENABLE_PROJECTS=clang;lld', *COMMON], log, env=env)
        run(['cmake', '--build', host, '--target', *HOST_TARGETS, '--parallel', jobs], log, env=env)
    tools = [host / 'bin' / name for name in HOST_TARGETS if name != 'clang']
    tools += [host / 'bin/clang++']
    if not all(path.is_file() for path in tools):
        raise RuntimeError('The original native LLVM tools are incomplete')
    version = subprocess.check_output([host / 'bin/clang++', '-dumpversion'], text=True).strip()
    if version != lock['VERSION']:
        raise RuntimeError('OSL helper bitcode needs the matching original Clang version')
    if not (host / 'lib/clang/20/include/stddef.h').is_file():
        raise RuntimeError('The original Clang resource headers are missing')
    return host, {path.name: digest(path) for path in tools}


def build_library(source, work, prefix, options, jobs, log, run, flags):
    directory = work.parent
    lock = json.loads((directory / 'source.json').read_text())
    env = dict(os.environ)
    for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'EM_PKG_CONFIG_PATH'):
        env.pop(name, None)
    host, native_hashes = host_tools(source, directory, lock, jobs, log, run, env)
    reused = pinned_reuse('BLENDER_WEB_LLVM_WASM_BUILD', lock)
    wasm = reused or work
    if reused:
        configuration = cache(wasm / 'CMakeCache.txt')
        expected = {'LLVM_ENABLE_THREADS': 'ON', 'LLVM_ENABLE_EH': 'ON', 'LLVM_ENABLE_RTTI': 'ON',
                    'LLVM_TARGETS_TO_BUILD': 'WebAssembly', 'LLVM_ENABLE_PROJECTS': 'clang;lld'}
        if any(configuration.get(key) != value for key, value in expected.items()):
            raise RuntimeError('The reused LLVM libraries have incompatible target or ABI settings')
        for key in ('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS'):
            if not set(flags.split()) <= set(configuration.get(key, '').split()):
                raise RuntimeError('The reused LLVM libraries lack the required Wasm ABI flags')
        cmake_version = '.'.join(configuration['CMAKE_CACHE_' + part + '_VERSION']
                                 for part in ('MAJOR', 'MINOR', 'PATCH'))
        compiler_info = wasm / 'CMakeFiles' / cmake_version / 'CMakeCCompiler.cmake'
        match = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', compiler_info.read_text())
        if not match:
            raise RuntimeError('The configured C compiler could not be verified')
        actual_compiler = Path(match.group(1)).resolve()
        if actual_compiler != Path(shutil.which('emcc')).resolve():
            raise RuntimeError('The reused LLVM build must use the active pinned SDK')
        proof = json.loads((wasm.parent / 'acceptance.json').read_text())
        expected_sdk = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
        if proof['emscriptenVersion'] != expected_sdk:
            raise RuntimeError('The reused LLVM proof used a different SDK')
    else:
        run(['emcmake', 'cmake', '-G', 'Unix Makefiles', '-S', source / 'llvm', '-B', wasm,
             *COMMON, *options, '-DLLVM_ENABLE_PROJECTS=clang;lld',
             f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags}',
             '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG',
             '-DCMAKE_EXE_LINKER_FLAGS=-pthread -fwasm-exceptions -sALLOW_MEMORY_GROWTH=1',
             f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DLLVM_HOST_TRIPLE=wasm32-unknown-emscripten',
             '-DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-unknown-emscripten', '-DLLVM_TARGET_ARCH=wasm32',
             '-DLLVM_ENABLE_THREADS=ON', '-DLLVM_ENABLE_BACKTRACES=OFF',
             '-DLLVM_ENABLE_CRASH_OVERRIDES=OFF', '-DLLVM_BUILD_TOOLS=OFF', '-DLLVM_BUILD_UTILS=OFF',
             '-DLLVM_ENABLE_LIBPFM=OFF', '-DLLVM_ENABLE_FFI=OFF', '-DBUILD_SHARED_LIBS=OFF',
             f'-DLLVM_TABLEGEN={host / "bin/llvm-tblgen"}', f'-DCLANG_TABLEGEN={host / "bin/clang-tblgen"}',
             '-DCLANG_BUILD_TOOLS=OFF', '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_ENABLE_STATIC_ANALYZER=OFF',
             '-DCLANG_ENABLE_ARCMT=OFF', '-DCLANG_ENABLE_OBJC_REWRITER=OFF'], log, env=env)
    # Installed CMake exports describe all enabled static libraries. Build their
    # real archives before installing; absent exports must never be fabricated.
    run(['cmake', '--build', wasm, '--parallel', jobs], log, env=env)
    run(['cmake', '--install', wasm, '--prefix', prefix], log, env=env)
    # CMake records the .js executable target but does not install its companion
    # Wasm. Copy genuine linker outputs for every installed LLVM tool.
    tool_artifacts = {}
    for name in (wasm / 'install_manifest.txt').read_text().splitlines():
        executable = Path(name)
        if executable.parent == prefix / 'bin' and executable.suffix == '.js':
            companion = wasm / 'bin' / executable.with_suffix('.wasm').name
            if not companion.is_file():
                raise RuntimeError(f'The LLVM tool {executable.name} lacks its Wasm output')
            destination = executable.with_suffix('.wasm')
            shutil.copy2(companion, destination)
            for artifact in (executable, destination):
                tool_artifacts[str(artifact.relative_to(prefix))] = digest(artifact)
    required = ['lib/libLLVMCore.a', 'lib/libLLVMWebAssemblyCodeGen.a', 'lib/libclangFrontend.a',
                'lib/liblldWasm.a', 'lib/cmake/llvm/LLVMConfig.cmake',
                'lib/cmake/clang/ClangConfig.cmake', 'lib/cmake/lld/LLDConfig.cmake']
    if not all((prefix / name).is_file() for name in required):
        raise RuntimeError('Original LLVM installation is incomplete')
    report = dict(source=lock, nativeTools=native_hashes, hostBuild=str(host),
                  wasmBuild=str(wasm), reusedHost=bool(os.environ.get('BLENDER_WEB_LLVM_HOST_BUILD')),
                  reusedWasm=bool(reused), toolArtifacts=tool_artifacts,
                  archives={name: digest(prefix / name) for name in required})
    (directory / 'compiler-installation.json').write_text(json.dumps(report, indent=2) + '\n')
