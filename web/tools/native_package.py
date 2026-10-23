#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage upstream Blender and Python assets, then collect the browser executable."""
import hashlib
import json
from pathlib import Path
import re
import shutil


def stage(base, source, prefix, shader_seed=True):
    target = base / 'assets'
    target.mkdir(exist_ok=True)
    blender = target / '5.2'
    blender.mkdir(exist_ok=True)
    for origin, destination in [(source / 'scripts', blender / 'scripts'),
                                (source / 'release/datafiles', blender / 'datafiles'),
                                (prefix / 'lib/python3.13', target / 'python3.13')]:
        if not origin.is_dir():
            raise RuntimeError(f'Missing runtime assets: {origin}')
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(origin, destination, ignore=shutil.ignore_patterns(
            '__pycache__', '*.pyc', 'test', 'tests', 'idlelib', 'tkinter',
            'turtledemo', 'ensurepip', 'config-3.13-*', '*.a', '*.xcf'))
    shutil.copy2(source / 'intern/ghost/web/native/ghost/GHOST_WebClock.py',
                 blender / 'scripts/modules/_bpy_ghost_web_clock.py')
    # The extension code is static; retain OpenUSD's original Python packages.
    usd_python = prefix / 'lib/python/pxr'
    if usd_python.is_dir():
        shutil.copytree(usd_python, target / 'python3.13/site-packages/pxr',
                        ignore=shutil.ignore_patterns('*.a', '*.pyc', '__pycache__', 'testenv'))
    seed = source / 'intern/ghost/web/native/shader-seed.json'
    if shader_seed and seed.is_file():
        from native_shader_seed import translator_inputs
        metadata = json.loads(seed.read_text())
        if metadata['translatorInputs'] != translator_inputs():
            raise RuntimeError('Shader seed is stale; recapture using native_shader_seed.py')
        shutil.copy2(seed, blender / 'datafiles/shader-seed.json')
    # Cycles' Python engine is installed from intern/cycles, outside scripts/.
    # Its CPU kernels are compiled into the binary; do not preload GPU sources.
    cycles = source / 'intern/cycles/blender/addon'
    shutil.copytree(cycles, blender / 'scripts/addons_core/cycles', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    for path in (blender / 'datafiles').rglob('*'):
        if path.is_file() and path.stat().st_size < 1024 and path.read_bytes().startswith(b'version https://git-lfs.github.com/spec/v1'):
            raise RuntimeError(f'Unhydrated Blender asset: {path}')

    fingerprint_assets(base, prefix)


def stage_locales(base, source, prefix):
    """Copy catalogs compiled by Blender's original msgfmt target."""
    destination = base / 'assets/5.2/datafiles/locale'
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / 'locale/languages', destination / 'languages')
    catalogs = sorted((source / 'locale/po').glob('*.po'))
    if not catalogs:
        raise RuntimeError('Missing pinned Blender translation sources')
    for catalog in catalogs:
        output = destination / catalog.stem / 'LC_MESSAGES/blender.mo'
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(base / 'editor/source/creator' / (catalog.stem + '.mo'), output)
    fingerprint_assets(base, prefix)


def fingerprint_assets(base, prefix):
    target = base / 'assets'
    fingerprint = hashlib.sha256()
    for path in sorted(target.rglob('*')):
        if path.is_file():
            fingerprint.update(str(path.relative_to(target)).encode())
            fingerprint.update(hashlib.sha256(path.read_bytes()).digest())
    # USD's imported CMake targets preload their resources outside assets/.
    # Their content must invalidate Blender's link as well as its cache key.
    for resources in (prefix / 'lib/usd', prefix / 'plugin/usd'):
        for path in sorted(resources.rglob('*')):
            if path.is_file():
                fingerprint.update(str(path.relative_to(prefix)).encode())
                fingerprint.update(hashlib.sha256(path.read_bytes()).digest())
    stamp = base / 'assets.sha256'
    digest = fingerprint.hexdigest() + '\n'
    if not stamp.exists() or stamp.read_text() != digest:
        stamp.write_text(digest)


def collect(base, root, ui_fallback=True):
    destination = base / 'dist'
    destination.mkdir(exist_ok=True)
    shell = root / 'web/native/site'
    artifacts = [base / 'editor/bin' / f'blender.{suffix}' for suffix in ['js', 'wasm', 'data', 'worker.js']]
    if ui_fallback:
        artifacts += [base / 'editor/bin' / f'blender-ui.{suffix}' for suffix in ['js', 'wasm', 'worker.js']]
    artifacts = [path for path in artifacts if path.exists()]
    artifacts.append(shell / 'audio-worklet.js')
    for required in ['blender.js', 'blender.wasm', 'blender.data']:
        if required not in [path.name for path in artifacts]:
            raise RuntimeError(f'Missing browser artifact: {required}')
    version_hash = hashlib.sha256()
    for path in sorted(shell.iterdir()) + artifacts:
        version_hash.update(path.name.encode())
        version_hash.update(path.read_bytes())
    version = version_hash.hexdigest()[:20]
    bundle = destination / 'assets' / version
    bundle.mkdir(parents=True, exist_ok=True)
    # Publish one build, even after repeated local render iterations. Previously
    # fetched versions remain available in each browser's versioned cache.
    for previous in bundle.parent.iterdir():
        if previous.is_dir() and previous != bundle:
            shutil.rmtree(previous)
    files = []
    for path in artifacts:
        output = bundle / path.name
        shutil.copy2(path, output)
        files.append({'name': str(output.relative_to(destination)), 'bytes': output.stat().st_size,
                      'sha256': hashlib.sha256(output.read_bytes()).hexdigest()})
    for path in shell.iterdir():
        output = destination / path.name
        if path.name == 'sw.js':
            output.write_text(path.read_text().replace('__VERSION__', version).replace(
                '__ASSETS__', json.dumps({entry['name']: {'sha256': entry['sha256'], 'bytes': entry['bytes']}
                                         for entry in files})))
        else:
            shutil.copy2(path, output)
    notices = destination / 'licenses'
    notices.mkdir(exist_ok=True)
    installed_notices = base / 'deps/prefix/share/blender-web/licenses'
    if installed_notices.is_dir():
        shutil.copytree(installed_notices, notices / 'dependencies', dirs_exist_ok=True)
    shutil.copy2(root / 'LICENSE', notices / 'GPL-2.0.txt')
    shutil.copy2(root / 'web/native/THIRD_PARTY.md', notices / 'THIRD_PARTY.md')
    upstream = base / 'source' if (base / 'source').is_dir() else root
    shutil.copy2(upstream / 'extern/ufbx/LICENSE', notices / 'ufbx-LICENSE.txt')
    shutil.copy2(upstream / 'extern/mantaflow/LICENSE', notices / 'mantaflow-LICENSE.txt')
    shutil.copy2(upstream / 'extern/audaspace/LICENSE', notices / 'audaspace-LICENSE.txt')
    shutil.copytree(upstream / 'intern/cycles/doc/license', notices / 'cycles',
                    dirs_exist_ok=True, ignore=shutil.ignore_patterns('CMakeLists.txt'))
    bullet = (upstream / 'extern/bullet2/src/btBulletDynamicsCommon.h').read_text()
    (notices / 'Bullet-LICENSE.txt').write_text(bullet.split('*/', 1)[0] + '*/\n')
    origins = list((base / 'deps').glob('*/source')) + list((base / 'gpu-deps/src').glob('*'))
    origins += list((base / 'gpu-deps/src').glob('*/third_party/*'))
    for origin in origins:
        if not origin.is_dir():
            continue
        for notice in origin.iterdir():
            if notice.is_file() and notice.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
                name = str(notice.relative_to(base)).replace('/', '__')
                shutil.copy2(notice, notices / name)
    (destination / 'SOURCE.txt').write_text(
        'Blender Web source and build recipes: https://github.com/Shlok-Bhakta/blender-web\n'
        'Official Blender release source is pinned in web/upstream.json and the blender submodule.\n'
        'WebGPU backend attribution and dependency notices are in licenses/.\n')
    pins = json.loads((root / 'web/upstream.json').read_text())
    # Record the configured binary's options, rather than inferring capabilities
    # from whichever preset happened to be used by the caller.
    options = dict(re.findall(r'^(WITH_\w+):BOOL=(ON|OFF)$',
                             (base / 'editor/CMakeCache.txt').read_text(), re.MULTILINE))
    seed = base / 'assets/5.2/datafiles/shader-seed.json'
    seed_info = None
    if seed.exists():
        seed_info = {'entries':len(json.loads(seed.read_text())['entries']),
                     'sha256':hashlib.sha256(seed.read_bytes()).hexdigest()}
    (destination / 'manifest.json').write_text(json.dumps({'version': version,
        'kind': 'native-blender', 'blenderVersion': pins['blenderVersion'],
        'blenderRevision': pins['blenderRevision'], 'emscriptenVersion': pins['emscriptenVersion'],
        'assetBase': f'assets/{version}/', 'uiFallback': (bundle / 'blender-ui.js').exists(),
        'buildOptions': {key: value == 'ON' for key, value in sorted(options.items())},
        'shaderSeed':seed_info,
        'files': files}, indent=2)+'\n')
    return destination
