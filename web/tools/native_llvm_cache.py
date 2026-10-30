#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checkpoint only genuine LLVM installation files, independently of other packages."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    return {name: digest(ROOT / name) for name in
            ('web/upstream.json', 'web/native/toolchain.cmake', 'web/tools/native_llvm.py')}


def entry(path):
    return dict(symlink=str(path.readlink())) if path.is_symlink() else dict(sha256=digest(path), bytes=path.stat().st_size)


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.unlink(missing_ok=True)
    shutil.copy2(source, destination, follow_symlinks=False)


def export(base, cache):
    prefix, llvm = base / 'deps/prefix', base / 'deps/llvm'
    if not (llvm / 'compiler-installation.json').is_file():
        print('LLVM installation has no build receipt; no new checkpoint created')
        return
    receipt = json.loads((llvm / 'compiler-installation.json').read_text())
    manifest = Path(receipt['wasmBuild']) / 'install_manifest.txt'
    if not manifest.is_file():
        print('LLVM installation manifest is unavailable; no new checkpoint created')
        return
    names = {str(Path(name).relative_to(prefix)) for name in manifest.read_text().splitlines()}
    names.update(receipt['toolArtifacts'])
    # LLVM creates executable aliases outside install_manifest.txt.
    targets = {(prefix / name).resolve() for name in names}
    names.update(str(path.relative_to(prefix)) for path in (prefix / 'bin').iterdir()
                 if path.is_symlink() and path.resolve() in targets)
    cache.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(cache / 'prefix', ignore_errors=True)
    files = {}
    for name in sorted(names):
        path = prefix / name
        assert not Path(name).is_absolute() and '..' not in Path(name).parts
        assert path.resolve().is_relative_to(prefix.resolve())
        files[name] = entry(path)
        copy(path, cache / 'prefix' / name)
    copy(llvm / 'installed.json', cache / 'installed.json')
    copy(llvm / 'compiler-installation.json', cache / 'compiler-installation.json')
    result = dict(inputs=inputs(), files=files, installedSha256=digest(cache / 'installed.json'),
                  receiptSha256=digest(cache / 'compiler-installation.json'))
    (cache / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'Checkpointed {len(files)} LLVM files without other dependency files')


def restore(base, cache):
    manifest = cache / 'manifest.json'
    if not manifest.is_file():
        print('No LLVM checkpoint to restore')
        return
    result = json.loads(manifest.read_text())
    assert result['inputs'] == inputs(), 'LLVM checkpoint inputs differ'
    assert digest(cache / 'installed.json') == result['installedSha256']
    assert digest(cache / 'compiler-installation.json') == result['receiptSha256']
    # Validate every cached file before touching the shared dependency prefix.
    for name, expected in result['files'].items():
        assert not Path(name).is_absolute() and '..' not in Path(name).parts
        path = cache / 'prefix' / name
        assert path.resolve().is_relative_to((cache / 'prefix').resolve())
        assert entry(path) == expected, name
    for name in result['files']:
        copy(cache / 'prefix' / name, base / 'deps/prefix' / name)
    for name in ('installed.json', 'compiler-installation.json'):
        copy(cache / name, base / 'deps/llvm' / name)
    print(f'Restored {len(result["files"])} validated LLVM files')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('export', 'restore'))
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--cache-dir', type=Path, default=ROOT / 'build/native/llvm-cache')
    args = parser.parse_args()
    (export if args.action == 'export' else restore)(args.build_dir.resolve(), args.cache_dir.resolve())
