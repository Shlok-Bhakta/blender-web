#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Probe memory64 separately from the production wasm32 ABI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from prepare import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/native/memory64')
    parser.add_argument('--node', default='node', help='Node with memory64 support, e.g. SDK Node 24')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'web/native/tests/memory64.c'
    supported = subprocess.run([args.node, '-e',
        "new WebAssembly.Memory({address:'i64',initial:1n,maximum:1n,shared:true})"],
        capture_output=True)
    if supported.returncode:
        raise RuntimeError('Node does not support shared memory64 by default. '
                           'Pass --node with the SDK Node 24 executable.')
    version = subprocess.check_output(['emcc', '-dumpversion'], text=True).strip()
    expected = json.loads((ROOT / 'web/upstream.json').read_text())['emscriptenVersion']
    if version != expected:
        raise RuntimeError(f'Expected Emscripten {expected}, found {version}')
    flags = ['-O2', '-sMEMORY64=1', '-pthread', '-sPROXY_TO_PTHREAD=1',
             '-sPTHREAD_POOL_SIZE=2', '-sALLOW_MEMORY_GROWTH=1',
             '-sINITIAL_MEMORY=67108864', '-sMAXIMUM_MEMORY=8589934592',
             '-sEXIT_RUNTIME=1', '-sENVIRONMENT=web,worker,node']
    subprocess.run(['emcc', str(source), *flags, '-o', str(output / 'probe.js')], check=True)
    result = subprocess.check_output([args.node, str(output / 'probe.js')], text=True, timeout=120)
    print(result, end='')
    marker = next(line.split(':', 1)[1] for line in result.splitlines()
                  if line.startswith('MEMORY64_PROBE:'))
    report = {'emscriptenVersion': version, 'flags': flags,
              'sourceSha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'wasmSha256': hashlib.sha256((output / 'probe.wasm').read_bytes()).hexdigest(),
              'nodeVersion': subprocess.check_output([args.node, '--version'], text=True).strip(),
              'node': json.loads(marker)}
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    subprocess.run([args.node, str(ROOT / 'web/tests/memory64.cjs'), str(output)], check=True)


if __name__ == '__main__':
    main()
