#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build OpenUSD's original CPU OpenVDB and OpenImageIO plugins as static Wasm."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from prepare import ROOT


INPUTS = ['web/tools/native_usd_imaging.py', 'web/native/usd-imaging/CMakeLists.txt',
          'web/native/toolchain.cmake', 'web/native/patches/usd.patch', 'web/upstream.json']
COMPONENTS = {'hioOpenVDB': 'pxr/imaging/hioOpenVDB',
              'hioOiio': 'pxr/imaging/plugin/hioOiio'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(base, jobs):
    from native_deps import C_FLAGS, run
    from native_python import extract_source
    base = base.resolve()
    prefix = base / 'deps/prefix'
    work = base / 'deps/usd-imaging'
    work.mkdir(parents=True, exist_ok=True)
    sdk = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    actual = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    if actual != sdk:
        raise RuntimeError(f'Expected Emscripten {sdk}, found {actual}')
    lock = work / 'source-lock.json'
    subprocess.run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}', '-DDEPENDENCIES=USD',
        f'-DOUTPUT={lock}', '-P', str(ROOT / 'web/native/dependency-lock.cmake')], check=True)
    usd = json.loads((base / 'deps/usd/installed.json').read_text())
    if usd['source'] != json.loads(lock.read_text())['USD'] or usd['sdk'] != sdk:
        raise RuntimeError('Build the pinned USD dependency before its CPU plugins')
    if usd['patch'] != digest(ROOT / 'web/native/patches/usd.patch'):
        raise RuntimeError('Rebuild USD with the current static runtime patch')
    if '-DPXR_BUILD_GPU_SUPPORT=OFF' not in usd['recipe'][1]:
        raise RuntimeError('This supplemental build expects the pinned CPU USD recipe')
    source = base / 'deps/usd/source'
    if not source.is_dir():
        # CI may restore installed USD archives without its generated source tree.
        source = extract_source(usd['source'], base, work / 'source')
    # Relative includes within upstream headers can bypass the installed prefix.
    # Keep the full source tree on the same Wasm ABI patch as the USD monolith.
    patch = ROOT / 'web/native/patches/usd.patch'
    command = ['patch', '--silent', '--dry-run', '--forward', '-p1', '-i', str(patch)]
    check = subprocess.run(command, cwd=source, capture_output=True)
    if check.returncode == 0:
        subprocess.run(['patch', '--silent', '--forward', '-p1', '-i', str(patch)],
                       cwd=source, check=True)
    elif subprocess.run(['patch', '--silent', '--dry-run', '--reverse', '-p1', '-i',
                         str(patch)], cwd=source, capture_output=True).returncode:
        raise RuntimeError('USD CPU plugin source does not match its checked Wasm ABI patch')
    dependencies = {name: digest(base / f'deps/{name}/installed.json')
                    for name in ('usd', 'openvdb', 'openimageio', 'tbb', 'libheif')}
    inputs = {name: digest(ROOT / name) for name in INPUTS}
    source_hashes = {str(path.relative_to(source)): digest(path)
                     for directory in COMPONENTS.values()
                     for path in sorted((source / directory).iterdir())
                     if path.suffix in {'.cpp', '.h', '.json'}}
    payload = dict(source=usd['source'], sdk=sdk, flags=C_FLAGS,
                   dependencyStamps=dependencies, inputs=inputs, sourceHashes=source_hashes)
    signature = hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()
    stamp = work / 'installed.json'
    if stamp.exists():
        cached = json.loads(stamp.read_text())
        if cached.get('signature') == signature and cached.get('artifacts') and all(
            (prefix / name).is_file() and digest(prefix / name) == expected
            for name, expected in cached['artifacts'].items()
        ):
            print('USD CPU imaging plugins: cached', flush=True)
            return cached
    log = work / 'build.log'
    run(['emcmake', 'cmake', '-G', 'Unix Makefiles', '-S', ROOT / 'web/native/usd-imaging',
         '-B', work / 'build', '-DCMAKE_BUILD_TYPE=Release',
         f'-DBLENDER_WEB_USD_SOURCE={source}', f'-DBLENDER_WEB_PREFIX={prefix}',
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
         f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DCMAKE_CXX_FLAGS=-O3 {C_FLAGS}'], log)
    run(['cmake', '--build', work / 'build', f'-j{jobs}'], log)
    run(['cmake', '--install', work / 'build'], log)
    artifacts = [prefix / f'include/pxr/imaging/hioOpenVDB/{name}'
                 for name in ('api.h', 'utils.h', 'vdbAssetInterface.h')]
    cmake = []
    for name, directory in COMPONENTS.items():
        metadata = json.loads((source / directory / 'plugInfo.json').read_text())
        plugin, = metadata['Plugins']
        # Empty LibraryPath is upstream's convention for built-in/static plugins.
        plugin.update(LibraryPath='', Root='..', ResourcePath='resources')
        target = prefix / f'lib/usd/{name}/resources/plugInfo.json'
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(metadata, indent=2) + '\n')
        archive = prefix / f'lib/blender-web-usd-imaging/lib{name}.a'
        if not archive.is_file():
            raise RuntimeError(f'Missing original USD component archive: {archive}')
        artifacts += [target, archive]
        cmake.append(f'''if(NOT TARGET blender_web_{name})
  add_library(blender_web_{name} STATIC IMPORTED)
  set_target_properties(blender_web_{name} PROPERTIES
    IMPORTED_LOCATION "${{CMAKE_CURRENT_LIST_DIR}}/lib{name}.a"
    INTERFACE_LINK_LIBRARIES "$<LINK_LIBRARY:WHOLE_ARCHIVE,usd_m>;OpenImageIO::OpenImageIO;${{OPENVDB_LIBRARIES}};Imath::Imath;TBB::tbb"
    INTERFACE_LINK_OPTIONS "SHELL:--preload-file ${{CMAKE_CURRENT_LIST_DIR}}/../usd/{name}/resources/plugInfo.json@/usd/{name}/resources/plugInfo.json")
endif()
''')
    destination = prefix / 'lib/blender-web-usd-imaging/libraries.cmake'
    destination.write_text(''.join(cmake) +
        'set(BLENDER_WEB_USD_IMAGING_LIBRARIES\n' + ''.join(
            f'  "$<LINK_LIBRARY:WHOLE_ARCHIVE,blender_web_{name}>"\n' for name in COMPONENTS) + ')\n' +
        'set(BLENDER_WEB_USD_IMAGING_ARCHIVES\n' + ''.join(
            f'  "${{CMAKE_CURRENT_LIST_DIR}}/lib{name}.a"\n' for name in COMPONENTS) + ')\n')
    artifacts.append(destination)
    result = dict(payload, signature=signature, components=list(COMPONENTS), gpuEnabled=False,
                  artifacts={str(path.relative_to(prefix)): digest(path) for path in artifacts})
    stamp.write_text(json.dumps(result, indent=2) + '\n')
    print('Built original USD CPU imaging plugins', flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    build(args.base, args.jobs)
