#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage upstream Blender and Python assets, then collect the browser executable."""
import hashlib
import json
from pathlib import Path
import shutil


def stage(base, source, prefix):
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
    for path in (blender / 'datafiles').rglob('*'):
        if path.is_file() and path.stat().st_size < 1024 and path.read_bytes().startswith(b'version https://git-lfs.github.com/spec/v1'):
            raise RuntimeError(f'Unhydrated Blender asset: {path}')

    fingerprint = hashlib.sha256()
    for path in sorted(target.rglob('*')):
        if path.is_file():
            fingerprint.update(str(path.relative_to(target)).encode())
            fingerprint.update(hashlib.sha256(path.read_bytes()).digest())
    stamp = base / 'assets.sha256'
    digest = fingerprint.hexdigest() + '\n'
    if not stamp.exists() or stamp.read_text() != digest:
        stamp.write_text(digest)


def collect(base, root):
    destination = base / 'dist'
    destination.mkdir(exist_ok=True)
    shell = root / 'web/native/site'
    artifacts = [base / 'editor/bin' / f'blender.{suffix}' for suffix in ['js', 'wasm', 'data', 'worker.js']]
    artifacts = [path for path in artifacts if path.exists()]
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
    files = []
    for path in artifacts:
        output = bundle / path.name
        shutil.copy2(path, output)
        files.append({'name': str(output.relative_to(destination)), 'bytes': output.stat().st_size,
                      'sha256': hashlib.sha256(output.read_bytes()).hexdigest()})
    for path in shell.iterdir():
        output = destination / path.name
        if path.name == 'sw.js':
            output.write_text(path.read_text().replace('__VERSION__', version))
        else:
            shutil.copy2(path, output)
    notices = destination / 'licenses'
    notices.mkdir(exist_ok=True)
    shutil.copy2(root / 'LICENSE', notices / 'GPL-2.0.txt')
    shutil.copy2(root / 'web/native/THIRD_PARTY.md', notices / 'THIRD_PARTY.md')
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
    (destination / 'manifest.json').write_text(json.dumps({'version': version,
        'kind': 'native-blender', 'blenderVersion': pins['blenderVersion'],
        'blenderRevision': pins['blenderRevision'], 'emscriptenVersion': pins['emscriptenVersion'],
        'assetBase': f'assets/{version}/', 'files': files}, indent=2)+'\n')
    return destination
