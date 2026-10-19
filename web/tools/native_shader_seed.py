#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Package verified native shader translations for a fresh browser cache."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from prepare import ROOT


def translator_inputs():
    paths = [ROOT / 'web/upstream.json', ROOT / 'web/native/toolchain.cmake']
    paths += [ROOT / 'web/native/gpu' / name for name in
              ('webgpu_shader.cc', 'shader_compat.hh', 'shader_defines.hh', 'shader_switch.hh')]
    recipes = ROOT / 'web/native/shader-deps'
    paths += [recipes / name for name in ('dep_common.sh', 'build_tint.sh',
              'build_shaderc.sh', 'build_spirv_tools.sh', 'tint-ice-recovery.patch')]
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in paths}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--source-ref', required=True, help='Commit matching the captured shader compiler and adapter sources')
    args = parser.parse_args()
    inputs = translator_inputs()
    for name, expected in inputs.items():
        source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{name}'], cwd=ROOT)
        if hashlib.sha256(source).hexdigest() != expected:
            raise RuntimeError(f'Captured translator differs from working source: {name}')
    acceptance = json.loads((args.capture.parent / 'acceptance.json').read_text())
    assert acceptance['referencePixelsEqual'] and acceptance['browserErrors'] == 0
    assert not acceptance.get('shaderOverrideEntries'), 'Seed capture must use the original translator'
    entries = json.loads(args.capture.read_text())
    assert len(entries) > 50 and len(dict(entries)) == len(entries)
    assert all(isinstance(key, str) and isinstance(value, str) and value for key, value in entries)
    seed = {'license':'GPL-2.0-or-later', 'translatorInputs': inputs, 'captureBundle': acceptance['bundleVersion'],
            'captureSourceRef': subprocess.check_output(['git', 'rev-parse', args.source_ref],
                                                       cwd=ROOT, text=True).strip(),
            'entries': entries}
    target = ROOT / 'web/native/shader-seed.json'
    target.write_text(json.dumps(seed, separators=(',', ':')) + '\n')
    print(f'{len(entries)} native shader translations; {target.stat().st_size} bytes')


if __name__ == '__main__':
    main()
