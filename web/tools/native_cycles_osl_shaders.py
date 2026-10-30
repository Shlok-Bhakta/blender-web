#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build original Cycles node shaders with installed Wasm oslc and compare native."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/native')
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or not shutil.which(args.blender):
        parser.error('Provide pinned native Blender and a positive job count')
    base = args.build_dir.resolve()
    prefix, work = base / 'deps/prefix', base / 'cycles-osl-shaders'
    work.mkdir(parents=True, exist_ok=True)
    upstream = ROOT / 'blender/intern/cycles/kernel/osl/shaders'
    pins = json.loads((ROOT / 'web/upstream.json').read_text())
    revision = subprocess.check_output(['git', '-C', ROOT / 'blender', 'rev-parse', 'HEAD'], text=True).strip()
    assert revision == pins['blenderRevision']
    clean = subprocess.check_output(['git', '-C', ROOT / 'blender', 'status', '--porcelain', '--',
                                    'intern/cycles/kernel/osl/shaders'], text=True)
    assert not clean, 'The pinned shader sources must be unchanged'
    receipt = json.loads((base / 'deps/osl/runtime-installation.json').read_text())
    compiler = prefix / 'bin/oslc.js'
    assert digest(compiler) == receipt['compilerJavaScript']
    assert digest(compiler.with_suffix('.wasm')) == receipt['compilerWasm']
    stamp = digest(base / 'deps/osl/installed.json')
    inputs = ['web/tools/native_cycles_osl_shaders.py',
              'web/native/cycles-osl-shaders/CMakeLists.txt',
              'web/native/tests/cycles-osl-shaders-native.py']
    hashes = {name: digest(ROOT / name) for name in inputs}
    source_hashes = {path.name: digest(path) for path in sorted(upstream.iterdir()) if path.is_file()}
    compiler_inputs = dict(javaScript=digest(compiler), wasm=digest(compiler.with_suffix('.wasm')),
                           launcher=digest(prefix / 'bin/oslc'),
                           standardHeaders={path.name: digest(path) for path in
                               sorted((prefix / 'share/OSL/shaders').glob('*.h'))})
    build_signature = dict(compiler=compiler_inputs, upstream=source_hashes, wrapper=hashes[inputs[1]])
    signature_file = work / 'build-signature.json'
    reuse = signature_file.is_file() and json.loads(signature_file.read_text()) == build_signature
    previous = work / 'acceptance.json'
    if reuse and previous.is_file():
        for name, item in json.loads(previous.read_text())['artifacts'].items():
            if '/wasm/' in name:
                output = base / name
                reuse &= output.is_file() and digest(output) == item['sha256']
    else:
        reuse = False
    if not reuse:
        for output in (work / 'wasm').rglob('*.oso'):
            output.unlink()
    source = work / 'source'
    shader_source = source / 'intern/cycles/kernel/osl/shaders'
    shutil.copytree(upstream, shader_source, dirs_exist_ok=True)
    shutil.copy2(ROOT / inputs[1], source / 'CMakeLists.txt')
    log = work / 'build.log'
    log.write_text('')
    run(['cmake', '-S', source, '-B', work / 'wasm', f'-DBLENDER_WEB_PREFIX={prefix}'], log)
    run(['cmake', '--build', work / 'wasm', '--target', 'cycles_osl_shaders', '--parallel', args.jobs], log)
    inventory = {}
    for name in (work / 'wasm/shader-sources.txt').read_text().splitlines():
        shader = Path(name)
        assert shader.is_relative_to(source) and shader.suffix == '.osl'
        output = (work / 'wasm' / shader.relative_to(source)).with_suffix('.oso')
        assert output.is_file() and output.stat().st_size > 0, output
        inventory[shader.name] = dict(source=str(shader), wasmOutput=str(output))
    assert inventory, 'Upstream must select its original shader sources'
    (work / 'inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
         '--python', ROOT / inputs[2], '--', work], work / 'native.log')
    native = json.loads((work / 'native-verification.json').read_text())
    assert native['shaderCount'] == len(inventory) and native['compiledMetadataEqual']
    assert hashes == {name: digest(ROOT / name) for name in inputs}, 'Check sources changed during validation'
    assert source_hashes == {path.name: digest(path) for path in sorted(upstream.iterdir()) if path.is_file()}
    assert stamp == digest(base / 'deps/osl/installed.json')
    assert compiler_inputs['javaScript'] == digest(compiler)
    assert compiler_inputs['wasm'] == digest(compiler.with_suffix('.wasm'))
    signature_file.write_text(json.dumps(build_signature, indent=2) + '\n')
    outputs = [Path(item['wasmOutput']) for item in inventory.values()]
    outputs += sorted((work / 'native').glob('*.oso'))
    report = dict(blenderRevision=revision, sourceHashes=hashes, upstreamSourceHashes=source_hashes,
                  compilerJavaScriptSha256=digest(compiler), compilerWasmSha256=digest(compiler.with_suffix('.wasm')),
                  launcherSha256=digest(prefix / 'bin/oslc'), compilerInputs=compiler_inputs,
                  installedDependencyStamp=stamp,
                  shaderCount=len(inventory), compiledMetadataEqual=True, native=native,
                  shaderExecutionVerified=False, applicationIntegrated=False,
                  artifacts={str(path.relative_to(base)): dict(bytes=path.stat().st_size, sha256=digest(path))
                             for path in outputs})
    (work / 'acceptance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(shaderCount=len(inventory), parameterCount=native['parameterCount'],
                          compiledMetadataEqual=True)), flush=True)


if __name__ == '__main__':
    main()
