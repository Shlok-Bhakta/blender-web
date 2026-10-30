#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare static Wasm OSL Python bindings with the pinned native Blender."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from native_deps import run
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(base, jobs=2, blender=None):
    base = base.resolve()
    prefix = base / 'deps/prefix'
    directory = base / 'osl-python-check'
    directory.mkdir(parents=True, exist_ok=True)
    fixture = base / 'compiler-check/fixtures/web_query.oso'
    inputs = ['web/tools/native_osl_python_check.py', 'web/native/tests/osl-python.py',
              'web/native/tests/python-bindings.cc', 'web/native/python-bindings/check/CMakeLists.txt']
    hashes = {name: digest(ROOT / name) for name in inputs}
    installation = digest(base / 'deps/python-bindings/installed.json')
    log = directory / 'build.log'
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/python-bindings/check',
         '-B', directory / 'wasm', '-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_CXX_FLAGS=-O3 -pthread -msimd128 -fwasm-exceptions',
         f'-DBLENDER_WEB_PREFIX={prefix}', f'-DBLENDER_WEB_ASSETS={base / "assets"}',
         f'-DBLENDER_WEB_OSL_FIXTURE={fixture}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
         f'-DCMAKE_PREFIX_PATH={prefix}'], log)
    run(['cmake', '--build', directory / 'wasm', '--parallel', jobs], log)
    result = subprocess.run(['node', directory / 'wasm/check.js'], text=True,
                            cwd=directory / 'wasm', capture_output=True, timeout=180)
    (directory / 'wasm.log').write_text(result.stdout + result.stderr)
    result.check_returncode()
    wasm = json.loads(result.stdout)
    # The invalid bytecode case emits original parser diagnostics to stderr.
    if 'Aborted(' in result.stderr or 'RuntimeError' in result.stderr:
        raise RuntimeError('Unexpected Wasm runtime diagnostics')
    if blender:
        script = directory / 'native.py'
        script.write_text('import bpy,json,runpy,sys\nfrom pathlib import Path\n'
            f'assert bpy.app.build_hash.decode() == {json.loads((ROOT / "web/upstream.json").read_text())["blenderRevision"][:12]!r}\n'
            f'report=runpy.run_path({str(ROOT / "web/native/tests/osl-python.py")!r})["run"]('
            f'{str(directory / "native")!r}, {str(fixture)!r})\n'
            f'Path({str(directory / "native.json")!r}).write_text(json.dumps(report,indent=2)+"\\n")\n')
        run([blender, '--background', '--factory-startup', '--python-exit-code', '1',
             '--python', script], directory / 'native.log')
        native = json.loads((directory / 'native.json').read_text())
        if wasm != native:
            raise RuntimeError('Original OSL Python results differ between native Blender and Wasm')
    if hashes != {name: digest(ROOT / name) for name in inputs}:
        raise RuntimeError('OSL Python check sources changed during validation')
    if installation != digest(base / 'deps/python-bindings/installed.json'):
        raise RuntimeError('The Python binding installation changed during validation')
    report = dict(nativeAPIsEqual=bool(blender), cleanInterpreterShutdown=True, result=wasm,
                  sourceHashes=hashes, bindingInstallationSha256=installation,
                  fixtureSha256=digest(fixture), artifacts={
                      str(path.relative_to(base)): dict(bytes=path.stat().st_size, sha256=digest(path))
                      for path in sorted((directory / 'wasm').glob('check.*')) if path.is_file()})
    (base / 'osl-python-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--blender', help='Also verify against this pinned native Blender executable')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    check(args.build_dir, args.jobs, args.blender)
