#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install pinned original Python packages without executing their build systems."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import zipfile

from prepare import ROOT


def install_wheel(archive, destination, notices):
    notices.mkdir(parents=True, exist_ok=True)
    installed = []
    with zipfile.ZipFile(archive) as wheel:
        metadata = [name for name in wheel.namelist() if name.endswith('.dist-info/WHEEL')
                    and len(PurePosixPath(name).parts) == 2]
        if len(metadata) != 1 or 'root-is-purelib: true' not in wheel.read(metadata[0]).decode().lower():
            raise RuntimeError(f'Expected a pure Python wheel: {archive}')
        for entry in wheel.infolist():
            if entry.is_dir():
                continue
            path = PurePosixPath(entry.filename)
            if path.is_absolute() or '..' in path.parts or '\\' in entry.filename:
                raise RuntimeError(f'Unsafe wheel path: {entry.filename}')
            if path.suffix.lower() == '.exe':
                # pip/setuptools bundle Windows launcher data in universal
                # wheels. These launchers do not run on native Linux or Wasm.
                continue
            if path.suffix.lower() in {'.so', '.pyd', '.dll', '.a', '.dylib'}:
                raise RuntimeError(f'Native executable in pure Python wheel: {entry.filename}')
            parts = path.parts
            if parts[0].endswith('.data'):
                if len(parts) < 3 or parts[1] not in {'purelib', 'platlib'}:
                    raise RuntimeError(f'Unsupported wheel install scheme: {entry.filename}')
                path = PurePosixPath(*parts[2:])
            output = destination / path
            output.parent.mkdir(parents=True, exist_ok=True)
            data = wheel.read(entry)
            output.write_bytes(data)
            installed.append(str(path))
            if re.search(r'(^|/)(licen[cs]e|copying|copyright|notice|authors)([./_-]|$)',
                         entry.filename, re.IGNORECASE):
                notice = notices / entry.filename
                notice.parent.mkdir(parents=True, exist_ok=True)
                notice.write_bytes(data)
    if not any(path.is_file() for path in notices.rglob('*')):
        raise RuntimeError(f'Missing license notices in {archive}')
    return installed


def build(base):
    from native_deps import download, install_notices
    prefix = base / 'deps/prefix'
    work = base / 'deps/python-packages'
    work.mkdir(parents=True, exist_ok=True)
    pins_path = ROOT / 'web/native/python-packages.json'
    pins = json.loads(pins_path.read_text())
    versions = (ROOT / 'blender/build_files/build_environment/cmake/versions.cmake').read_text()
    for package in pins['packages']:
        native = re.search(r'set\(' + package['blenderVersionVariable'] + r' ([^)]+)\)', versions)
        if not native or native[1] != package['blenderVersionLiteral']:
            raise RuntimeError(f'Refresh Python package lock for {package["name"]}')
    lock = work / 'source-lock.json'
    subprocess.run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}',
        '-DDEPENDENCIES=PYTHON;CYTHON', f'-DOUTPUT={lock}',
        '-P', str(ROOT / 'web/native/dependency-lock.cmake')], check=True)
    sources = json.loads(lock.read_text())
    patch = ROOT / 'web/native/patches/urllib3.patch'
    transport = ROOT / 'web/native/ghost/GHOST_WebNetwork.py'
    downloads = ROOT / 'web/native/ghost/GHOST_WebDownload.py'
    signature = hashlib.sha256(Path(__file__).read_bytes() + pins_path.read_bytes() +
                               patch.read_bytes() + transport.read_bytes() + downloads.read_bytes() +
                               json.dumps(sources, sort_keys=True).encode()).hexdigest()
    destination = prefix / 'lib/python3.13/site-packages'
    notices = prefix / 'share/blender-web/licenses'
    stamp = work / 'installed.json'
    previous = json.loads(stamp.read_text()) if stamp.exists() else {}
    if (previous.get('signature') == signature and
            all((destination / path).is_file() for path in previous['files'])):
        print('Python runtime packages: cached', flush=True)
        return previous
    # Only remove paths recorded by this installer, inside its own prefix.
    for name in previous.get('files', []):
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts:
            raise RuntimeError(f'Invalid package receipt: {name}')
        (destination / path).unlink(missing_ok=True)
    destination.mkdir(parents=True, exist_ok=True)
    files = []
    for package in pins['packages']:
        archive = download(dict(FILE=package['file'], HASH_TYPE='SHA256',
            HASH=package['sha256'], URI=package['url']), base / 'deps/downloads')
        files += install_wheel(archive, destination, notices / package['name'])
    subprocess.run(['patch', '--silent', '--forward', '-p1', '-i', str(patch)],
                   cwd=destination, check=True)
    shutil.copy2(transport, destination / '_bpy_ghost_web_network.py')
    files.append('_bpy_ghost_web_network.py')
    shutil.copy2(downloads, destination / '_bpy_ghost_web_download.py')
    files.append('_bpy_ghost_web_download.py')
    # Both sources are independently verified against Blender's pinned hashes.
    # Python's own bundled pip wheel needs no separately resolved dependency.
    from native_python import extract_source
    python = extract_source(sources['PYTHON'], base, work / 'python')
    wheels = list((python / 'Lib/ensurepip/_bundled').glob('pip-*-none-any.whl'))
    if len(wheels) != 1:
        raise RuntimeError('Expected the pinned CPython bundled pip wheel')
    files += install_wheel(wheels[0], destination, notices / 'pip')
    cython = extract_source(sources['CYTHON'], base, work / 'cython')
    install_notices(cython, prefix, 'cython')
    for package in ('Cython', 'pyximport'):
        for path in sorted((cython / package).rglob('*')):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            # Keep original Python compiler code, include declarations and C
            # utility templates. Generated accelerators have a Python source
            # counterpart and are not needed by Cython's supported pure mode.
            if path.suffix == '.c' and any(path.with_suffix(s).exists() for s in ('.py', '.pyx')):
                continue
            relative = path.relative_to(cython)
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, output)
            files.append(str(relative))
    shutil.copy2(cython / 'cython.py', destination / 'cython.py')
    files.append('cython.py')
    metadata = Path('Cython-' + sources['CYTHON']['VERSION'] + '.egg-info/PKG-INFO')
    (destination / metadata).parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(cython / 'PKG-INFO', destination / metadata)
    files.append(str(metadata))
    report = dict(signature=signature, packages=pins['packages'], sources=sources,
        bundledPip=wheels[0].name, cythonMode='original pure Python compiler',
        files=sorted(set(files)), installedBytes=sum((destination / name).stat().st_size for name in set(files)),
        limitations=['Native subprocess build tools and native machine-code extensions need browser equivalents.',
                     'Downloading packages requires browser network transport integration.',
                     'Cython native accelerators are not compiled; its original pure Python compiler is included.'])
    stamp.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('bundledPip', 'cythonMode', 'installedBytes')}), flush=True)
    return report
