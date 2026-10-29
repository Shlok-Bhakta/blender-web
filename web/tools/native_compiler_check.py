#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute compiler and query checks against installed target libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from native_deps import run
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check(base, jobs=2, llvm_only=False):
    base = base.resolve()
    prefix = base / 'deps/prefix'
    work = base / 'compiler-check'
    work.mkdir(parents=True, exist_ok=True)
    log = work / 'build.log'
    log.write_text('')
    sdk = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    if sdk != json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']:
        raise RuntimeError('Activate the pinned Emscripten SDK')
    stamps = {name: digest(base / f'deps/{name}/installed.json')
              for name in (('llvm',) if llvm_only else ('llvm', 'osl'))}
    inputs = ['web/tools/native_compiler_check.py', 'web/native/compiler-check/CMakeLists.txt',
              'web/native/tests/llvm-compiler.cc', 'web/tests/llvm-generated.cjs']
    fixtures = work / 'fixtures'
    options = []
    if not llvm_only:
        fixtures.mkdir(exist_ok=True)
        source = ROOT / 'web/native/tests/fixtures/osl-query/web_query.osl'
        reference = ROOT / 'web/native/tests/fixtures/osl-query/native-query.json'
        inputs += [str(source.relative_to(ROOT)), str(reference.relative_to(ROOT)),
                   'web/native/tests/osl-query.cc',
                   'web/native/osl-query-probe/cmake/OSLDependencies.cmake']
        shutil.copy2(source, fixtures / source.name)
        shutil.copy2(reference, fixtures / reference.name)
        run([prefix / 'bin/oslc', '-o', fixtures / 'web_query.oso', source], log)
        invalid = fixtures / 'invalid.osl'
        invalid.write_text('shader invalid(output color c=0) { c = ; }\n')
        (fixtures / 'invalid.oso').unlink(missing_ok=True)
        rejected = subprocess.run([prefix / 'bin/oslc', '-o', fixtures / 'invalid.oso', invalid],
                                  text=True, capture_output=True, timeout=60)
        (work / 'invalid-shader.log').write_text(rejected.stdout + rejected.stderr)
        if not rejected.returncode or (fixtures / 'invalid.oso').exists():
            raise RuntimeError('The installed compiler accepted an invalid shader')
        # A new invocation must still compile a valid shader after the error.
        run([prefix / 'bin/oslc', '-o', fixtures / 'web_query.oso', source], log)
        options += [f'-DBLENDER_WEB_OSL_FIXTURES={fixtures}']
    source_hashes = {name: digest(ROOT / name) for name in inputs}
    build = work / ('llvm-only' if llvm_only else 'all')
    run(['emcmake', 'cmake', '-S', ROOT / 'web/native/compiler-check', '-B', build,
         '-DCMAKE_BUILD_TYPE=Release', f'-DCMAKE_PREFIX_PATH={prefix}',
         f'-DCMAKE_FIND_ROOT_PATH={prefix}', '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY',
         f'-DLLVM_DIR={prefix / "lib/cmake/llvm"}', f'-DClang_DIR={prefix / "lib/cmake/clang"}',
         f'-DLLD_DIR={prefix / "lib/cmake/lld"}', f'-DBLENDER_SOURCE={ROOT / "blender"}',
         *options], log)
    run(['cmake', '--build', build, '--parallel', jobs], log)
    result = subprocess.check_output(['node', ROOT / 'web/tests/llvm-generated.cjs', build],
                                     text=True, timeout=120)
    report = dict(llvm=json.loads(result), emscriptenVersion=sdk,
                  installedDependencyStamps=stamps, sourceHashes=source_hashes)
    if not llvm_only:
        result = subprocess.check_output(['node', build / 'osl-query.js'], text=True, timeout=120)
        report.update(osl=json.loads(result), invalidShaderRejected=True,
                      compilerRecovery=True, shaderSha256=digest(fixtures / 'web_query.oso'))
    if source_hashes != {name: digest(ROOT / name) for name in inputs}:
        raise RuntimeError('Compiler check sources changed during execution')
    if stamps != {name: digest(base / f'deps/{name}/installed.json') for name in stamps}:
        raise RuntimeError('Compiler libraries changed during execution')
    report['artifacts'] = {str(path.relative_to(base)): dict(bytes=path.stat().st_size, sha256=digest(path))
        for path in sorted(build.glob('*.wasm'))}
    destination = base / ('llvm-install-check.json' if llvm_only else 'compiler-check.json')
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--llvm-only', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    check(args.build_dir, args.jobs, args.llvm_only)
