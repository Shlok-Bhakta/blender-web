#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the embedded interpreter and GLSL-to-WGSL toolchain with the Wasm ABI."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare import ROOT
from native_numpy import build as build_numpy


def build(base, jobs):
    recipes = ROOT / 'web/native/shader-deps'
    env = dict(os.environ, ROOT=str(ROOT), BLENDER_WEB_NATIVE_BASE=str(base), NPROC=str(jobs))
    outputs = {
        'python': [base / 'deps/prefix/lib/libpython3.13.a'],
        'tint': [base / 'gpu-deps/build/tint/src/tint/libtint_api.a',
                 base / 'gpu-deps/build/tint/src/tint/libtint_lang_spirv_reader.a',
                 base / 'gpu-deps/build/tint/src/tint/libtint_lang_wgsl_writer.a'],
        'spirv_tools': [base / 'gpu-deps/build/spirv-tools/source/opt/libSPIRV-Tools-opt.a',
                        base / 'gpu-deps/build/spirv-tools/source/libSPIRV-Tools.a'],
        'shaderc': [base / 'deps/prefix/lib/libshaderc_combined.a'],
    }
    reports = []
    for name, artifacts in outputs.items():
        inputs = [recipes / 'dep_common.sh', recipes / f'build_{name}.sh', ROOT / 'web/upstream.json']
        if name == 'tint':
            inputs.append(recipes / 'tint-ice-recovery.patch')
        if name == 'python':
            inputs.append(recipes / 'python-expat-namespace.patch')
        signature = hashlib.sha256(b''.join(path.read_bytes() for path in inputs)).hexdigest()
        stamp = base / 'gpu-deps' / f'{name}.installed.json'
        if not (stamp.exists() and json.loads(stamp.read_text()).get('signature') == signature
                and all(path.is_file() for path in artifacts)):
            log = base / f'{name}-build.log'
            with log.open('w') as output:
                result = subprocess.run(['bash', str(recipes / f'build_{name}.sh')], env=env,
                                         stdout=output, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'{name} failed; see {log}\n' + '\n'.join(log.read_text().splitlines()[-40:]))
            report = {'name': name, 'signature': signature, 'artifacts': []}
            for artifact in artifacts:
                if not artifact.is_file():
                    raise RuntimeError(f'{name} did not produce {artifact}')
                report['artifacts'].append({'path': str(artifact.relative_to(base)),
                    'bytes': artifact.stat().st_size,
                    'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest()})
            stamp.write_text(json.dumps(report, indent=2)+'\n')
        reports.append(json.loads(stamp.read_text()))
        if name == 'python':
            reports.append(build_numpy(base, jobs))
    (base / 'application-dependencies.json').write_text(json.dumps(reports, indent=2)+'\n')
