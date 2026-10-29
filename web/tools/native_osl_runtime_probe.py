#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run original OSL shaders offline through a finite GHOST/LLVM Wasm adapter."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

from native_deps import download, run
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native/osl-runtime-probe')
    parser.add_argument('--compiler-build-dir', type=Path, default=ROOT / 'build/native/osl-query-probe')
    parser.add_argument('--llvm-build-dir', type=Path, default=ROOT / 'build/native/llvm-probe')
    parser.add_argument('--prefix', type=Path, default=ROOT / 'build/native/deps/prefix')
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--flex', default=shutil.which('flex'))
    parser.add_argument('--bison', default=shutil.which('bison'))
    parser.add_argument('--m4', default=shutil.which('m4'))
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or not all((args.flex, args.bison, args.m4, shutil.which(args.blender),
                                shutil.which('emcmake'), shutil.which('c++'))):
        parser.error('Activate the SDK and provide native Blender, a C++ compiler, Flex, Bison and m4')
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    if subprocess.check_output(['emcc', '-dumpversion'], text=True).strip() != expected:
        parser.error('Use the pinned Emscripten SDK')
    base, compiler, llvm, prefix = (args.build_dir.resolve(), args.compiler_build_dir.resolve(),
                                    args.llvm_build_dir.resolve(), args.prefix.resolve())
    prerequisite = json.loads((compiler / 'osl-compiler.acceptance.json').read_text())
    if prerequisite['llvmVersion'] != '20.1.8' or prerequisite['emscriptenVersion'] != expected:
        parser.error('First reproduce the original OSL compiler proof with the pinned SDK')
    base.mkdir(parents=True, exist_ok=True)
    log = base / 'runtime-proof-build.log'
    log.write_text('')
    env = dict(os.environ)
    for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'EM_PKG_CONFIG_PATH'):
        env.pop(name, None)
    env['M4'] = str(Path(args.m4).resolve())
    data = Path(args.bison).resolve().parents[1] / 'share/bison'
    if data.is_dir():
        env['BISON_PKGDATADIR'] = str(data)
    patches = [ROOT / 'web/native/patches/osl.patch', ROOT / 'web/native/patches/osl-runtime.patch']
    if prerequisite['patchSha256'] != digest(patches[0]):
        parser.error('The original compiler proof must use the current base portability patch')
    lock = prerequisite['source']
    signature = dict(source=lock, patches={p.name: digest(p) for p in patches})
    source, stamp = base / 'osl', base / 'osl-source.json'
    if source.exists():
        if not stamp.exists() or json.loads(stamp.read_text()) != signature:
            raise RuntimeError('Existing runtime sources lack the matching stamp; preserve and inspect before reuse')
    else:
        archive = download(lock, ROOT / 'build/native/deps/downloads')
        with tempfile.TemporaryDirectory(dir=base, prefix='extract-') as temporary:
            with tarfile.open(archive) as bundle:
                bundle.extractall(temporary, filter='data')
            roots = list(Path(temporary).iterdir())
            if len(roots) != 1 or not (roots[0] / 'src/liboslexec/llvm_instance.cpp').is_file():
                raise RuntimeError('Unexpected original OSL source layout')
            shutil.move(roots[0], source)
        for patch in patches:
            run(['patch', '--batch', '-p1', '-i', patch], log, cwd=source, env=env)
        stamp.write_text(json.dumps(signature, indent=2) + '\n')
    # The SDK compiler emits LLVM 22 bitcode. The original OSL runtime reads
    # LLVM 20 bitcode, so compile its helper IR with the matching original Clang
    # while preserving the SDK's actual Wasm C++ ABI and compatibility headers.
    run(['cmake', '--build', llvm / 'host', '--target', 'clang', 'llvm-as', 'llvm-link',
         '--parallel', args.jobs], log, env=env)
    host_clang = llvm / 'host/bin/clang++'
    if subprocess.check_output([host_clang, '-dumpversion'], text=True).strip() != '20.1.8':
        raise RuntimeError('OSL helper bitcode requires the pinned original Clang 20.1.8')
    run(['cmake', '--build', llvm / 'wasm', '--target', 'LLVMMCJIT', 'LLVMGlobalISel',
         '--parallel', args.jobs], log, env=env)
    sdk = Path(shutil.which('emcc')).resolve().parent
    generator = ';'.join(map(str, [sys.executable, ROOT / 'web/tools/native_osl_bitcode.py',
                                   '--compiler', host_clang, '--sdk', sdk, '--']))
    adapter = ROOT / 'web/native/osl-query-probe'
    flags = '-pthread -msimd128 -fwasm-exceptions'
    run(['emcmake', 'cmake', '-S', source, '-B', base / 'osl-wasm',
         f'-DCMAKE_PROJECT_OSL_INCLUDE={adapter / "cmake/OSLDependencies.cmake"}',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17', '-DBUILD_SHARED_LIBS=OFF',
         f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags} -I{ROOT / "web/native/ghost"}',
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix};{llvm / "wasm"}',
         '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY', f'-DCMAKE_MODULE_PATH={adapter / "cmake"}',
         f'-DLLVM_DIR={llvm / "wasm/lib/cmake/llvm"}', f'-DClang_DIR={llvm / "wasm/lib/cmake/clang"}',
         f'-DLLD_DIR={llvm / "wasm/lib/cmake/lld"}', '-DLLVM_STATIC=ON', '-DUSE_LLVM_BITCODE=ON',
         f'-DLLVM_BC_GENERATOR={generator}', f'-DLLVM_AS_TOOL={llvm / "host/bin/llvm-as"}',
         f'-DLLVM_LINK_TOOL={llvm / "host/bin/llvm-link"}', '-DOSL_BUILD_TESTS=OFF',
         '-DOSL_BUILD_SHADERS=OFF', '-DOSL_BUILD_PLUGINS=OFF', '-DUSE_PYTHON=OFF', '-DUSE_QT=OFF',
         '-DUSE_PARTIO=OFF', '-DOSL_USE_OPTIX=OFF', '-DSTOP_ON_WARNING=OFF', '-DINSTALL_DOCS=OFF',
         f'-DFLEX_EXECUTABLE={Path(args.flex).resolve()}', f'-DBISON_EXECUTABLE={Path(args.bison).resolve()}'],
        log, env=env)
    run(['cmake', '--build', base / 'osl-wasm', '--target', 'oslexec', '--parallel', args.jobs], log, env=env)
    symbols = subprocess.check_output([sdk.parent / 'bin/llvm-nm', '--defined-only', '--extern-only',
                                       base / 'osl-wasm/lib/liboslexec.a'], text=True)
    helpers = sorted({'_' + p[2] for line in symbols.splitlines() if len(p := line.split()) == 3
                      and p[1] in ('T', 'W') and p[2].startswith(('osl_', 'rs_'))})
    if not helpers:
        raise RuntimeError('Original renderer helpers are missing')
    exports = ['_main', '_malloc', '_free', '_pthread_self', '_emscripten_builtin_memalign',
               '_emscripten_builtin_free', '_sinf', '_cosf', *helpers]
    export_file, export_text = base / 'helper-exports.json', json.dumps(exports) + '\n'
    if not export_file.exists() or export_file.read_text() != export_text:
        export_file.write_text(export_text)
    executable = base / 'runtime'
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/osl-runtime-probe', '-B', executable,
         '-DCMAKE_BUILD_TYPE=Release', f'-DOSL_WORK={base}', f'-DOSL_COMPILER_WORK={compiler}',
         f'-DOSL_PREFIX={prefix}', f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix};{llvm / "wasm"}',
         '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY', f'-DLLVM_DIR={llvm / "wasm/lib/cmake/llvm"}',
         f'-DLLD_DIR={llvm / "wasm/lib/cmake/lld"}',
         f'-DSDK_CRTBEGIN={sdk / "cache/sysroot/lib/wasm32-emscripten/pic/crtbegin.o"}'], log, env=env)
    run(['cmake', '--build', executable, '--parallel', args.jobs], log, env=env)
    output = base / 'evidence'
    run(['node', ROOT / 'web/tests/osl-runtime.cjs', executable, compiler, output], log, env=env)
    report = json.loads((output / 'acceptance.json').read_text())
    fixture = ROOT / 'web/native/tests/fixtures/osl-runtime/web_execute.osl'
    run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
         '--python', ROOT / 'web/native/tests/osl-runtime-native.py', '--', fixture,
         output / 'native-fixtures'], log, env=env)
    native_libs = Path(shutil.which(args.blender)).resolve().parent / 'lib'
    libraries = [native_libs / name for name in ('liboslexec.so.1.15.3', 'libOpenImageIO.so.3.1.13',
                                                'libOpenImageIO_Util.so.3.1.13')]
    native_binary = base / 'osl-native'
    run(['c++', '-std=c++17', '-pthread', '-O2', f'-I{source / "src/include"}',
         f'-I{base / "osl-wasm/include"}', f'-I{prefix / "include"}', f'-I{ROOT / "web/native/ghost"}',
         ROOT / 'web/native/tests/osl-runtime.cc', *libraries, f'-Wl,-rpath,{native_libs}',
         '-o', native_binary], log, env=env)
    native = {}
    for name in ('native', 'node', 'browser'):
        result = subprocess.check_output([native_binary, output / (name + '-fixtures')], env=env, text=True)
        native[name] = json.loads(result.strip())
        if native[name] != report['node']:
            raise RuntimeError(f'{name} bytecode did not match the native operation checks')
    report.update(source=signature, emscriptenVersion=expected, llvmVersion='20.1.8', native=native,
                  nativeLibraries={p.name: digest(p) for p in libraries}, rendererHelperCount=len(helpers),
                  nativeCompiler=json.loads((output / 'native-fixtures/native.json').read_text()))
    files = [executable / 'osl-runtime.js', executable / 'osl-runtime.wasm', native_binary,
             base / 'osl-wasm/lib/liboslexec.a', output / 'osl-runtime.png', output / 'osl-runtime.webm']
    files += sorted(output.glob('*-fixtures/web_execute.oso'))
    report['artifacts'] = {str(p.relative_to(base)): dict(bytes=p.stat().st_size, sha256=digest(p)) for p in files}
    sources = ['web/tools/native_osl_runtime_probe.py', 'web/tools/native_osl_bitcode.py',
               'web/native/ghost/GHOST_WebOSL.hh', 'web/native/ghost/GHOST_WebOSLProbe.cc',
               'web/native/patches/osl.patch', 'web/native/patches/osl-runtime.patch',
               'web/native/osl-runtime-probe/CMakeLists.txt', 'web/native/tests/osl-runtime.cc',
               'web/native/tests/osl-runtime-native.py', 'web/tests/osl-runtime.cjs',
               'web/native/tests/fixtures/osl-runtime/web_execute.osl',
               'web/native/osl-query-probe/cmake/FindLLVM.cmake',
               'web/native/osl-query-probe/cmake/OSLDependencies.cmake']
    report['sourceHashes'] = {name: digest(ROOT / name) for name in sources}
    (base / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(browser=report['browser']['result'], native=native,
                          browserErrors=report['browserErrors'], offline=report['browser']['offline']), indent=2))


if __name__ == '__main__':
    main()
