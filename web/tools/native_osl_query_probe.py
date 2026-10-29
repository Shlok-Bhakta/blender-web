#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify original OSL query code separately from the pending shader runtime."""
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
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native/osl-query-probe')
    parser.add_argument('--llvm-build-dir', type=Path, default=ROOT / 'build/native/llvm-probe')
    parser.add_argument('--prefix', type=Path, default=ROOT / 'build/native/deps/prefix')
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--flex', default=shutil.which('flex'))
    parser.add_argument('--bison', default=shutil.which('bison'))
    parser.add_argument('--m4', default=shutil.which('m4'))
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if not all((args.flex, args.bison, args.m4, shutil.which('emcmake'), shutil.which(args.blender))):
        parser.error('Activate the pinned SDK and provide native Blender, Flex, Bison and m4')
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    if subprocess.check_output(['emcc', '-dumpversion'], text=True).strip() != expected:
        parser.error('Use the pinned Emscripten SDK')
    base, llvm, prefix = args.build_dir.resolve(), args.llvm_build_dir.resolve(), args.prefix.resolve()
    base.mkdir(parents=True, exist_ok=True)
    if not (llvm / 'acceptance.json').exists():
        parser.error('Build the separate LLVM compiler proof first')
    llvm_proof = json.loads((llvm / 'acceptance.json').read_text())
    if llvm_proof['llvmVersion'] != '20.1.8' or llvm_proof['emscriptenVersion'] != expected:
        parser.error('The LLVM proof must use the pinned compiler and SDK')
    log = base / 'query-proof-build.log'
    log.write_text('')
    env = dict(os.environ)
    for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'EM_PKG_CONFIG_PATH'):
        env.pop(name, None)
    env['M4'] = str(Path(args.m4).resolve())
    data = Path(args.bison).resolve().parents[1] / 'share/bison'
    if data.is_dir():
        env['BISON_PKGDATADIR'] = str(data)
    lock_path = base / 'osl-source-lock.json'
    run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}', '-DDEPENDENCIES=OSL',
         f'-DOUTPUT={lock_path}', '-P', ROOT / 'web/native/dependency-lock.cmake'], log, env=env)
    source_lock = json.loads(lock_path.read_text())['OSL']
    patch = ROOT / 'web/native/patches/osl.patch'
    signature = dict(source=source_lock, patchSha256=digest(patch))
    source, stamp = base / 'osl', base / 'osl-source.json'
    if source.exists():
        if not stamp.exists() or json.loads(stamp.read_text()) != signature:
            raise RuntimeError('Existing OSL sources lack the matching stamp; preserve and inspect them before reuse')
    else:
        downloads = ROOT / 'build/native/deps/downloads'
        downloads.mkdir(parents=True, exist_ok=True)
        archive = download(source_lock, downloads)
        with tempfile.TemporaryDirectory(dir=base, prefix='extract-') as temporary:
            with tarfile.open(archive) as bundle:
                bundle.extractall(temporary, filter='data')
            roots = list(Path(temporary).iterdir())
            if len(roots) != 1 or not (roots[0] / 'src/include/OSL/oslquery.h').is_file():
                raise RuntimeError('Unexpected original OSL archive layout')
            shutil.move(roots[0], source)
        run(['patch', '--batch', '--forward', '-p1', '-i', patch], log, cwd=source, env=env)
        stamp.write_text(json.dumps(signature, indent=2) + '\n')
    # Only native TableGen is needed to configure Clang's target exports.
    # The query library itself does not link the Clang compiler or LLVM runtime.
    if not (llvm / 'host/bin/clang-tblgen').exists():
        run(['cmake', '--build', llvm / 'host', '--target', 'clang-tblgen', '--parallel', args.jobs], log, env=env)
    if not (llvm / 'wasm/lib/cmake/clang/ClangConfig.cmake').exists():
        run(['emcmake', 'cmake', '-S', llvm / 'llvm/llvm', '-B', llvm / 'wasm',
             '-DLLVM_ENABLE_PROJECTS=clang;lld', f'-DCLANG_TABLEGEN={llvm / "host/bin/clang-tblgen"}',
             '-DCLANG_BUILD_TOOLS=OFF', '-DCLANG_INCLUDE_TESTS=OFF', '-DCLANG_ENABLE_STATIC_ANALYZER=OFF',
             '-DCLANG_ENABLE_ARCMT=OFF', '-DCLANG_ENABLE_OBJC_REWRITER=OFF'], log, env=env)
    adapter = ROOT / 'web/native/osl-query-probe'
    flags = '-pthread -msimd128 -fwasm-exceptions'
    run(['emcmake', 'cmake', '-S', source, '-B', base / 'osl-wasm',
         f'-DCMAKE_PROJECT_OSL_INCLUDE={adapter / "cmake/OSLDependencies.cmake"}',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17', '-DBUILD_SHARED_LIBS=OFF',
         f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags}',
         '-DCMAKE_EXE_LINKER_FLAGS=-pthread -fwasm-exceptions', f'-DCMAKE_PREFIX_PATH={prefix}',
         f'-DCMAKE_FIND_ROOT_PATH={prefix};{llvm / "wasm"}', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY',
         f'-DCMAKE_MODULE_PATH={adapter / "cmake"}', f'-DCMAKE_INSTALL_PREFIX={base / "osl-prefix"}',
         f'-DLLVM_DIR={llvm / "wasm/lib/cmake/llvm"}', f'-DClang_DIR={llvm / "wasm/lib/cmake/clang"}',
         f'-DLLD_DIR={llvm / "wasm/lib/cmake/lld"}', '-DLLVM_STATIC=ON', '-DUSE_LLVM_BITCODE=OFF',
         '-DOSL_BUILD_TESTS=OFF', '-DOSL_BUILD_SHADERS=OFF', '-DOSL_BUILD_PLUGINS=OFF',
         '-DUSE_PYTHON=OFF', '-DUSE_QT=OFF', '-DUSE_PARTIO=OFF', '-DOSL_USE_OPTIX=OFF',
         '-DSTOP_ON_WARNING=OFF', '-DINSTALL_DOCS=OFF', f'-DFLEX_EXECUTABLE={Path(args.flex).resolve()}',
         f'-DBISON_EXECUTABLE={Path(args.bison).resolve()}'], log, env=env)
    run(['cmake', '--build', base / 'osl-wasm', '--target', 'oslquery', '--parallel', args.jobs], log, env=env)
    # Paired control: restoring the original size_t alias must fail the original
    # encoded-type checks. The corrected header must compile and preserve high bits.
    negative = base / 'hash-original/OSL'
    negative.mkdir(parents=True, exist_ok=True)
    header = (base / 'osl-wasm/include/OSL/oslconfig.h').read_text()
    alias = 'using ustringhash_pod = ustringhash::hash_t;'
    if alias not in header:
        raise RuntimeError('The guarded hash-width fix is missing')
    (negative / 'oslconfig.h').write_text(header.replace(alias, 'using ustringhash_pod = size_t;'))
    hash_check = base / 'hash-width.cc'
    hash_check.write_text('#include <OSL/encodedtypes.h>\nstatic_assert(sizeof(OSL::ustringhash_pod) == 8);\n')
    compile_args = ['em++', '-std=c++17', *flags.split(), f'-I{source / "src/include"}',
                    f'-I{base / "osl-wasm/include"}', f'-I{prefix / "include"}', '-c', hash_check]
    negative_log = base / 'hash-original.log'
    with negative_log.open('w') as output:
        result = subprocess.run(['em++', f'-I{negative.parent}', *map(str, compile_args[1:]),
                                 '-o', str(base / 'hash-original.o')], env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode == 0 or '8 == 4' not in negative_log.read_text():
        raise RuntimeError('The original hash-width control did not fail as expected')
    run([*compile_args, '-o', base / 'hash-corrected.o'], log, env=env)
    fixtures = base / 'query-fixtures'
    fixtures.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'web/native/tests/fixtures/osl-query/web_query.osl', fixtures / 'web_query.osl')
    run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
         '--python', ROOT / 'web/native/tests/osl-query-native.py', '--', fixtures], log, env=env)
    check = base / 'query-proof'
    run(['emcmake', 'cmake', '-S', adapter, '-B', check, '-DCMAKE_BUILD_TYPE=Release',
         f'-DOSL_WORK={base}', f'-DBLENDER_SOURCE={ROOT / "blender"}', f'-DCMAKE_PREFIX_PATH={prefix}',
         f'-DCMAKE_FIND_ROOT_PATH={prefix}', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY'], log, env=env)
    run(['cmake', '--build', check, '--parallel', args.jobs], log, env=env)
    run(['node', ROOT / 'web/tests/osl-query.cjs', check, base / 'query-browser-evidence'], log, env=env)
    report = json.loads((base / 'query-browser-evidence/acceptance.json').read_text())
    report['source'] = source_lock
    report['patchSha256'] = signature['patchSha256']
    report['nativeReference'] = json.loads((fixtures / 'native-behavior.json').read_text())
    report['emscriptenVersion'] = expected
    report['shaderExecutionVerified'] = False
    report['originalHashAliasRejected'] = True
    report['hashNegativeControlLogSha256'] = digest(negative_log)
    report['artifacts'] = {str(p.relative_to(base)): dict(bytes=p.stat().st_size, sha256=digest(p))
                           for p in (check / 'osl-query.js', check / 'osl-query.wasm',
                                     base / 'osl-wasm/lib/liboslquery.a', fixtures / 'web_query.oso',
                                     fixtures / 'native-query.json')}
    sources = ['web/tools/native_osl_query_probe.py', 'web/native/patches/osl.patch',
               'web/native/osl-query-probe/CMakeLists.txt', 'web/native/osl-query-probe/cmake/FindLLVM.cmake',
               'web/native/osl-query-probe/cmake/OSLDependencies.cmake', 'web/native/tests/osl-query.cc',
               'web/native/tests/osl-query-native.py', 'web/native/tests/fixtures/osl-query/web_query.osl',
               'web/tests/osl-query.cjs']
    report['sourceHashes'] = {name: digest(ROOT / name) for name in sources}
    (base / 'osl-query.acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
