#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reuse Binaryen results for byte-identical application links.

Emscripten implements JSPI in the JavaScript loader, so the render and UI links
hand wasm-opt the same module with the same arguments. Results are keyed by the
input bytes, the arguments and the Binaryen executable; anything else, including
smaller tool links, runs Binaryen unchanged.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

# Only the browser application is large enough to be worth storing.
MINIMUM_BYTES = 64 << 20
# The render result and its predecessor; each entry is the size of the module.
KEPT_ENTRIES = 2
UNCACHEABLE = ('--input-source-map', '-ism', '--output-source-map', '-osm', '--symbolmap')


def environment(cache):
    """Environment that routes Emscripten's wasm-opt calls through the cache."""
    root = cache / 'binaryen'
    query = {key: value for key, value in os.environ.items()
             if not (key == 'EM_BINARYEN_ROOT' and Path(value) == root)}
    real = Path(subprocess.check_output(['em-config', 'BINARYEN_ROOT'], env=query, text=True).strip())
    binaries = root / 'bin'
    binaries.mkdir(parents=True, exist_ok=True)
    for tool in (real / 'bin').iterdir():
        link = binaries / tool.name
        if tool.name != 'wasm-opt' and not (link.is_symlink() and link.readlink() == tool):
            link.unlink(missing_ok=True)
            link.symlink_to(tool)
    wrapper = binaries / 'wasm-opt'
    wrapper.unlink(missing_ok=True)
    wrapper.write_text(f'#!/bin/sh\nexec {sys.executable} -I {Path(__file__).resolve()} '
                       f'{real / "bin/wasm-opt"} {cache} "$@"\n')
    wrapper.chmod(0o755)
    # Emscripten's sanity stamp covers only its version and LLVM_ROOT.
    return {**os.environ, 'EM_BINARYEN_ROOT': str(root)}


def file_digest(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while chunk := stream.read(1 << 24):
            digest.update(chunk)
    return digest.hexdigest()


def main(real, cache, args):
    run = [real, *args]
    if args.count('-o') != 1:
        os.execv(real, run)
    index = args.index('-o')
    if index == 0 or index + 1 == len(args):
        os.execv(real, run)
    source, output = args[index - 1], args[index + 1]
    if (source.startswith('-') or not os.path.isfile(source) or
            os.path.getsize(source) < MINIMUM_BYTES or
            any(arg.startswith(UNCACHEABLE) for arg in args)):
        os.execv(real, run)
    key = hashlib.sha256()
    tool = os.stat(real)
    key.update(f'{os.path.realpath(real)}\0{tool.st_size}\0{tool.st_mtime_ns}\0'.encode())
    for name in sorted(name for name in os.environ if name.startswith('BINARYEN_')):
        key.update(f'{name}={os.environ[name]}\0'.encode())
    for position, arg in enumerate(args):
        key.update(b'<input>' if position == index - 1 else b'<output>' if position == index + 1 else arg.encode())
        key.update(b'\0')
    key.update(file_digest(source).encode())
    entry = Path(cache) / key.hexdigest()
    if (entry / 'output.wasm').is_file():
        shutil.copyfile(entry / 'output.wasm', output)
        sys.stdout.buffer.write((entry / 'stdout').read_bytes())
        os.utime(entry)
        print(f'wasm-opt: reused identical optimization {entry.name[:16]}', file=sys.stderr)
        return 0
    result = subprocess.run(run, stdout=subprocess.PIPE)
    sys.stdout.buffer.write(result.stdout)
    if result.returncode:
        return result.returncode
    staging = entry.with_name(entry.name + '.tmp')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    shutil.copyfile(output, staging / 'output.wasm')
    (staging / 'stdout').write_bytes(result.stdout)
    shutil.rmtree(entry, ignore_errors=True)
    staging.rename(entry)
    entries = sorted((path for path in Path(cache).iterdir() if len(path.name) == 64 and path.is_dir()),
                     key=lambda path: path.stat().st_mtime, reverse=True)
    for stale in entries[KEPT_ENTRIES:]:
        shutil.rmtree(stale, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3:]))
