#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build pinned native libffi for independent OSL comparison executables."""
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


def build(base, jobs, log):
    lock = base / 'native-ffi-lock.json'
    run(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}', '-DDEPENDENCIES=FFI',
         f'-DOUTPUT={lock}', '-P', ROOT / 'web/native/dependency-lock.cmake'], log)
    source_lock = json.loads(lock.read_text())['FFI']
    source = base / 'native-ffi-source'
    stamp = base / 'native-ffi-source.json'
    if source.exists():
        if not stamp.exists() or json.loads(stamp.read_text()) != source_lock:
            raise RuntimeError('Preserve and inspect native libffi sources with an unmatched stamp')
    else:
        archive = download(source_lock, ROOT / 'build/native/deps/downloads')
        with tempfile.TemporaryDirectory(dir=base, prefix='ffi-extract-') as temporary:
            with tarfile.open(archive) as bundle:
                bundle.extractall(temporary, filter='data')
            roots = list(Path(temporary).iterdir())
            if len(roots) != 1 or not (roots[0] / 'configure').is_file():
                raise RuntimeError('Unexpected pinned libffi source layout')
            shutil.move(roots[0], source)
        stamp.write_text(json.dumps(source_lock, indent=2) + '\n')
    work, prefix = base / 'native-ffi-build', base / 'native-ffi'
    work.mkdir(exist_ok=True)
    env = dict(os.environ, CC=shutil.which('cc'), CXX=shutil.which('c++'),
               AR=shutil.which('ar'), RANLIB=shutil.which('ranlib'),
               CFLAGS='-O2', CXXFLAGS='-O2', LDFLAGS='')
    run([source / 'configure', f'--prefix={prefix}', '--disable-shared',
         '--enable-static', '--disable-docs'], log, cwd=work, env=env)
    run(['make', f'-j{jobs}'], log, cwd=work, env=env)
    run(['make', 'install'], log, cwd=work, env=env)
    archive = prefix / 'lib/libffi.a'
    report = dict(source=source_lock, target=subprocess.check_output(
        [env['CC'], '-dumpmachine'], text=True).strip(),
        archiveSha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
        ffiHeaderSha256=hashlib.sha256((prefix / 'include/ffi.h').read_bytes()).hexdigest(),
        targetHeaderSha256=hashlib.sha256((prefix / 'include/ffitarget.h').read_bytes()).hexdigest())
    (base / 'native-ffi.json').write_text(json.dumps(report, indent=2) + '\n')
    return prefix, report
