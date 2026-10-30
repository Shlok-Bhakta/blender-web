#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify guarded Cycles OSL layouts against the original OSL types."""
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
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1 or not shutil.which('emcmake') or not shutil.which(args.blender):
        parser.error('Activate the pinned SDK and provide native Blender and a positive job count')
    sdk = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    pins = json.loads((ROOT / 'web/upstream.json').read_text())
    if sdk != pins['emscriptenVersion']:
        parser.error('Use the pinned Emscripten SDK')
    revision = subprocess.check_output(['git', '-C', ROOT / 'blender', 'rev-parse', 'HEAD'], text=True).strip()
    assert revision == pins['blenderRevision']
    subprocess.run(['git', '-C', ROOT / 'blender', 'diff', '--quiet', 'HEAD', '--', 'intern/cycles'], check=True)
    base = args.build_dir.resolve()
    prefix, work = base / 'deps/prefix', base / 'cycles-osl-abi'
    source = work / 'source'
    headers = source / 'intern/cycles/kernel/osl'
    headers.mkdir(parents=True, exist_ok=True)
    inputs = ['web/tools/native_cycles_osl_abi.py', 'web/native/cycles-osl-abi/CMakeLists.txt',
              'web/native/tests/cycles-osl-abi.cc', 'web/native/ghost/GHOST_WebOSLTypes.hh',
              'web/native/ghost/GHOST_WebOSLRuntime.hh',
              'web/patches/native-osl-types.patch', 'web/patches/native-osl-runtime.patch']
    hashes = {name: digest(ROOT / name) for name in inputs}
    stamps = {name: digest(base / f'deps/{name}/installed.json') for name in ('osl', 'openimageio')}
    upstream_inputs = {name: digest(ROOT / 'blender/intern/cycles' / name)
                        for name in ('kernel/osl/types.h', 'kernel/osl/closures_template.h',
                                     'kernel/osl/services.h', 'kernel/osl/services.cpp',
                                     'kernel/osl/strings.h', 'kernel/osl/closures.cpp',
                                     'kernel/osl/globals.cpp', 'kernel/types.h', 'scene/osl.cpp')}
    for name in ('types.h', 'closures_template.h', 'services.cpp', 'strings.h'):
        shutil.copy2(ROOT / 'blender/intern/cycles/kernel/osl' / name, headers / name)
    scene = source / 'intern/cycles/scene'
    scene.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / 'blender/intern/cycles/scene/osl.cpp', scene / 'osl.cpp')
    compiler_args = ['-std=c++20', '-fno-strict-aliasing', '-UNDEBUG', '-DWITH_GHOST_WEB', '-DWITH_OSL',
        '-DCCL_NAMESPACE_BEGIN=namespace ccl {', '-DCCL_NAMESPACE_END=}',
        f'-I{source / "intern/cycles"}', f'-I{ROOT / "blender/intern/cycles"}',
        f'-I{ROOT / "blender/intern/atomic"}',
        f'-I{ROOT / "web"}', f'-I{prefix / "include"}', f'-I{prefix / "include/Imath"}']
    test = ROOT / 'web/native/tests/cycles-osl-abi.cc'
    baseline = subprocess.run(['em++', '-pthread', '-msimd128', '-fwasm-exceptions',
                               '-fsyntax-only', '-DBLENDER_WEB_OSL_ABI_BASELINE',
                               *compiler_args, test], text=True, capture_output=True)
    (work / 'baseline.log').write_text(baseline.stdout + baseline.stderr)
    assert baseline.returncode and '16 == 12' in baseline.stderr and '24 == 20' in baseline.stderr
    assert 'StringFactory' in baseline.stderr
    log = work / 'build.log'
    log.write_text('')
    run(['git', 'apply', '--check', ROOT / 'web/patches/native-osl-types.patch'], log, cwd=source)
    run(['git', 'apply', ROOT / 'web/patches/native-osl-types.patch'], log, cwd=source)
    run(['git', 'apply', '--check', ROOT / 'web/patches/native-osl-runtime.patch'], log, cwd=source)
    run(['git', 'apply', ROOT / 'web/patches/native-osl-runtime.patch'], log, cwd=source)
    env = dict(os.environ, BINARYEN_CORES=os.environ.get('BINARYEN_CORES', '1'))
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/cycles-osl-abi', '-B', work / 'wasm',
         '-DCMAKE_BUILD_TYPE=Release', f'-DBLENDER_WEB_STAGED={source}', f'-DBLENDER_SOURCE={ROOT / "blender"}',
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
         '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY'], log, env=env)
    run(['cmake', '--build', work / 'wasm', '--parallel', args.jobs], log, env=env)
    wasm = subprocess.check_output(['node', work / 'wasm/cycles-osl-abi.js'], text=True)
    (work / 'wasm.json').write_text(wasm)
    native_lib = Path(shutil.which(args.blender)).resolve().parent / 'lib'
    library = native_lib / 'libOpenImageIO_Util.so.3.1.13'
    binary = work / 'native'
    run(['c++', '-O2', *compiler_args, test, library, f'-Wl,-rpath,{native_lib}', '-o', binary], log)
    native = subprocess.check_output([binary], text=True)
    (work / 'native.json').write_text(native)
    wasm, native = json.loads(wasm), json.loads(native)
    assert wasm['pointerBytes'] == 4 and native['pointerBytes'] == 8
    for item in (wasm, native):
        assert item['closureTree'] and item['stringHashFull'] and not item['applicationIntegrated']
        assert item['textureHandleCases'] == 24
    assert wasm['hash'] == native['hash']
    assert hashes == {name: digest(ROOT / name) for name in inputs}, 'Sources changed during validation'
    assert stamps == {name: digest(base / f'deps/{name}/installed.json') for name in stamps}
    assert upstream_inputs == {name: digest(ROOT / 'blender/intern/cycles' / name) for name in upstream_inputs}
    objects = sorted((work / 'wasm/CMakeFiles/cycles-osl-kernel.dir').rglob('*.cpp.o'))
    assert sorted(path.name for path in objects) == ['closures.cpp.o', 'globals.cpp.o', 'services.cpp.o']
    scene_objects = sorted((work / 'wasm/CMakeFiles/cycles-osl-scene.dir').rglob('*.cpp.o'))
    assert len(scene_objects) == 1 and scene_objects[0].name == 'osl.cpp.o'
    readobj = Path(shutil.which('emcc')).resolve().parent.parent / 'bin/llvm-readobj'
    for path in [*objects, *scene_objects]:
        header = subprocess.check_output([readobj, '--file-headers', path], text=True)
        assert 'Format: WASM' in header and 'Arch: wasm32' in header
    string_assertions = sum(line.startswith('OSL_DEVICE_STRING(') for line in (headers / 'strings.h').read_text().splitlines())
    report = dict(wasm=wasm, native=native, emscriptenVersion=sdk, blenderRevision=revision,
                  sourceHashes=hashes, upstreamSourceHashes=upstream_inputs,
                  compiledKernelSources=3, compiledSceneSources=1, predefinedStringAssertions=string_assertions,
                  installedDependencyStamps=stamps, nativeLibrarySha256=digest(library),
                  baselineLayoutMismatchReproduced=True, applicationIntegrated=False,
                  artifacts={str(path.relative_to(base)): dict(bytes=path.stat().st_size, sha256=digest(path))
                             for path in [work / 'wasm/cycles-osl-abi.js', work / 'wasm/cycles-osl-abi.wasm', binary, *objects, *scene_objects]})
    (work / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
