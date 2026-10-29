#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Repeat browser ctypes checks with native Blender's original _ctypes module."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--blender', default='blender')
    args = parser.parse_args()
    directory = args.directory.resolve()
    report = json.loads((directory / 'acceptance.json').read_text())
    source = ROOT / 'web/native/tests/python-ctypes.py'
    assert report['sourceSha256'] == hashlib.sha256(source.read_bytes()).hexdigest()
    blend = directory / 'ctypes.blend'
    assert blend.stat().st_size == report['blend']['bytes']
    assert hashlib.sha256(blend.read_bytes()).hexdigest() == report['blend']['sha256']
    output = directory / 'native'
    output.mkdir(exist_ok=True)
    script = output / 'verify.py'
    script.write_text('''import bpy, ctypes, _ctypes, hashlib, json, runpy, sys
from pathlib import Path
directory=Path(sys.argv[sys.argv.index('--')+1])
source=Path(sys.argv[sys.argv.index('--')+2])
report=json.loads((directory/'acceptance.json').read_text())
native=runpy.run_path(str(source))['run']()
differences={key for key in native if native[key]!=report['report'][key]}
assert differences=={'pointerBytes','longBytes'}, differences
assert native['pointerBytes']==native['longBytes']==8
assert report['report']['pointerBytes']==report['report']['longBytes']==4
bpy.ops.wm.open_mainfile(filepath=str(directory/'ctypes.blend'))
assert json.loads(bpy.data.texts['Original ctypes results'].as_string())==report['report']
library=ctypes.CDLL(_ctypes.__file__)
library.ffi_get_version.restype=ctypes.c_char_p
assert library.ffi_get_version()==b'3.5.2'
result=dict(savedBlendVerified=True, originalNativeModule=True, exactFunctionalResults=True,
            nativeReport=native, architectureDifferences=sorted(differences),
            blenderVersion=bpy.app.version_string, blenderRevision=bpy.app.build_hash.decode(),
            libffiVersion=library.ffi_get_version().decode(),
            nativeModuleSha256=hashlib.sha256(Path(_ctypes.__file__).read_bytes()).hexdigest())
(directory/'native/results.json').write_text(json.dumps(result,indent=2)+'\\n')
''')
    with (output / 'readback.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
                        '--python', str(script), '--', str(directory), str(source)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    result = json.loads((output / 'results.json').read_text())
    result.update(bundleVersion=report['bundleVersion'], sourceSha256=report['sourceSha256'],
                  blend=report['blend'], verifierSourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
