#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build pinned image, color, volume and material Python bindings as static Wasm."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from prepare import ROOT


MATERIALX_MODULES = ('Core', 'Format', 'GenShader', 'GenGlsl', 'GenMsl', 'GenOsl', 'GenMdl', 'Render')
MODULES = [('PyOpenColorIO.PyOpenColorIO', 'PyOpenColorIO'),
           ('OpenImageIO.OpenImageIO', 'OpenImageIO'), ('openvdb', 'openvdb'),
           ('oslquery.oslquery', 'oslquery')]
MODULES += [('MaterialX.PyMaterialX' + module, 'PyMaterialX' + module)
            for module in MATERIALX_MODULES]
LIBRARIES = ['PyOpenColorIO', 'PyOpenImageIO', 'openvdb_python', 'nanobind_runtime',
             'oslquery_python']
LIBRARIES += ['PyMaterialX' + module for module in MATERIALX_MODULES]
LIBRARIES += ['MaterialXGenOsl', 'MaterialXGenMdl', 'MaterialXRender']
INPUTS = ['web/tools/native_bindings.py', 'web/native/python-bindings/CMakeLists.txt',
          'web/native/python-bindings/nanobind-newfunc.patch', 'web/native/patches/yamlcpp.patch',
          'web/native/toolchain.cmake', 'web/upstream.json']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(base, jobs):
    from native_deps import C_FLAGS, install_notices, run
    from native_python import extract_source
    expected_sdk = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    installed_sdk = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    if installed_sdk != expected_sdk:
        raise RuntimeError(f'Expected Emscripten {expected_sdk}, found {installed_sdk}')
    base = base.resolve()
    work = base / 'deps/python-bindings'
    work.mkdir(parents=True, exist_ok=True)
    prefix = base / 'deps/prefix'
    lock = work / 'source-lock.json'
    subprocess.run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}',
        '-DDEPENDENCIES=PYBIND11;NANOBIND;OPENCOLORIO;OPENIMAGEIO;OPENVDB;MATERIALX;OSL',
        f'-DOUTPUT={lock}', '-P', str(ROOT / 'web/native/dependency-lock.cmake')], check=True)
    sources = json.loads(lock.read_text())
    dependency_stamps = {}
    for name in ('opencolorio', 'openimageio', 'openvdb', 'materialx', 'yamlcpp', 'robinmap', 'osl'):
        stamp = base / f'deps/{name}/installed.json'
        if not stamp.is_file():
            raise RuntimeError(f'Build {name} before its Python bindings')
        dependency_stamps[name] = digest(stamp)
    yaml_stamp = json.loads((base / 'deps/yamlcpp/installed.json').read_text())
    if yaml_stamp.get('patch') != digest(ROOT / 'web/native/patches/yamlcpp.patch'):
        raise RuntimeError('Rebuild YAMLCPP with the Wasm destructor fix before the Python bindings')
    python_stamp = base / 'gpu-deps/python.installed.json'
    dependency_stamps['python'] = digest(python_stamp)
    inputs = {name: digest(ROOT / name) for name in INPUTS}
    signature = hashlib.sha256(json.dumps(dict(sources=sources, inputs=inputs,
        dependencyStamps=dependency_stamps, flags=C_FLAGS), sort_keys=True).encode()).hexdigest()
    stamp = work / 'installed.json'
    if stamp.exists():
        cached = json.loads(stamp.read_text())
        if cached.get('signature') == signature and all(
            (prefix / name).is_file() and digest(prefix / name) == expected
            for name, expected in cached.get('artifacts', {}).items()
        ) and cached.get('artifacts'):
            print('Python library bindings: cached', flush=True)
            return cached
    src = work / 'sources'
    src.mkdir(exist_ok=True)
    patch = ROOT / 'web/native/python-bindings/nanobind-newfunc.patch'
    patch_stamp = work / 'nanobind-patch.sha256'
    if patch_stamp.exists() and patch_stamp.read_text().strip() != digest(patch):
        shutil.rmtree(src / 'nanobind', ignore_errors=True)
    for name, entry in sources.items():
        source = extract_source(entry, base, src / name.lower())
        install_notices(source, prefix, name)
    nanobind = src / 'nanobind'
    check = subprocess.run(['patch', '--silent', '--dry-run', '--forward', '-p1', '-i', str(patch)],
                           cwd=nanobind, capture_output=True)
    if check.returncode == 0:
        subprocess.run(['patch', '--silent', '--forward', '-p1', '-i', str(patch)],
                       cwd=nanobind, check=True)
    elif subprocess.run(['patch', '--silent', '--dry-run', '--reverse', '-p1', '-i', str(patch)],
                        cwd=nanobind, capture_output=True).returncode:
        raise RuntimeError('The pinned nanobind constructor patch does not apply')
    patch_stamp.write_text(digest(patch) + '\n')
    # The upstream nanobind archive contains a git submodule placeholder.
    # Supply the already pinned robin-map headers used by Blender's native recipe.
    shutil.copytree(prefix / 'include/tsl', nanobind / 'ext/robin_map/include/tsl', dirs_exist_ok=True)
    log = work / 'build.log'
    run(['emcmake', 'cmake', '-G', 'Unix Makefiles', '-S', ROOT / 'web/native/python-bindings',
         '-B', work / 'build', '-DCMAKE_BUILD_TYPE=Release',
         f'-DBLENDER_WEB_BINDING_SOURCES={src}', f'-DBLENDER_WEB_PREFIX={prefix}',
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
         f'-DCMAKE_INSTALL_PREFIX={prefix}', f'-DCMAKE_CXX_FLAGS=-O3 {C_FLAGS}'], log)
    run(['cmake', '--build', work / 'build', f'-j{jobs}'], log)
    run(['cmake', '--install', work / 'build'], log)
    site = prefix / 'lib/python3.13/site-packages'
    artifacts = []
    for name in ('pybind11', 'nanobind'):
        artifacts += [path for path in (prefix / 'share/blender-web/licenses' / name).rglob('*')
                      if path.is_file()]
    render_notices = prefix / 'share/blender-web/licenses/materialx/render'
    external = src / 'materialx/source/MaterialXRender/External'
    for name in ('TinyObjLoader/LICENSE', 'Cgltf/LICENSE',
                 'StbImage/stb_image.h', 'StbImage/stb_image_write.h'):
        target = render_notices / name
        target.parent.mkdir(parents=True, exist_ok=True)
        # stb's complete license text is embedded in each original header.
        shutil.copy2(external / name, target)
        artifacts.append(target)
    for package, source in (
        ('PyOpenColorIO', src / 'opencolorio/src/bindings/python/package'),
        ('OpenImageIO', src / 'openimageio/src/python'),
        ('MaterialX', src / 'materialx/python/MaterialX'),
        ('oslquery', src / 'osl/src/liboslquery'),
    ):
        destination = site / package
        destination.mkdir(parents=True, exist_ok=True)
        # Preserve the original package wrappers. Desktop binaries and launcher
        # executables are never installed as browser extensions.
        for path in sorted(source.rglob('*.py')):
            if '__pycache__' in path.parts:
                continue
            target = destination / path.relative_to(source)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            artifacts.append(target)
    resource_root = site / 'MaterialX/libraries'
    shutil.copytree(prefix / 'libraries', resource_root, dirs_exist_ok=True)
    artifacts += [path for path in resource_root.rglob('*') if path.is_file()]
    header = prefix / 'include/blender-web-python-bindings.hh'
    header.write_text('/* SPDX-License-Identifier: GPL-2.0-or-later */\n#pragma once\n'
        '#include <Python.h>\n#include <cstdlib>\n' +
        '\n'.join(f'extern "C" PyObject *PyInit_{init}();' for _, init in MODULES) +
        '\ninline void GHOST_web_register_python_bindings()\n{\n' +
        '\n'.join(f'  if (PyImport_AppendInittab("{name}", PyInit_{init}) < 0) std::abort();'
                  for name, init in MODULES) + '\n}\n')
    destination = prefix / 'lib/blender-web-bindings'
    cmake = destination / 'libraries.cmake'
    cmake.write_text('set(BLENDER_WEB_PYTHON_BINDING_LIBRARIES\n' +
        '\n'.join(f'  "${{CMAKE_CURRENT_LIST_DIR}}/lib{name}.a"' for name in LIBRARIES) +
        '\n  "${CMAKE_CURRENT_LIST_DIR}/../liboslquery.a"\n)\n')
    artifacts += [header, cmake, prefix / 'lib/liboslquery.a',
                  *(destination / f'lib{name}.a' for name in LIBRARIES)]
    report = dict(signature=signature, sources=sources, sourceHashes=inputs,
        dependencyStamps=dependency_stamps, modules=[name for name, _ in MODULES],
        artifacts={str(path.relative_to(prefix)): digest(path) for path in sorted(artifacts)},
        limitations=['MaterialX GPU rendering needs a browser backend.',
                     'MaterialX OSL rendering needs the OSL compiler/runtime integration.',
                     'These are statically registered modules; arbitrary dynamic extensions remain separate.'])
    stamp.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(modules=report['modules'], archives=len(LIBRARIES))), flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    build(args.build_dir, args.jobs)
