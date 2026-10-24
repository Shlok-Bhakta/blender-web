#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently import browser-compressed GLBs with the pinned desktop Blender."""
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
    source = ROOT / 'web/tests/gltf.py'
    assert hashlib.sha256(source.read_bytes()).hexdigest() == report['sourceSha256']
    for name, metadata in report['exports'].items():
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == metadata['sha256']
    blend = directory / 'compressed-assets.blend'
    assert hashlib.sha256(blend.read_bytes()).hexdigest() == report['blend']['sha256']
    target = directory / 'desktop-gltf.json'
    expression = f'''import bpy,runpy,json,pathlib
ns=runpy.run_path({str(source)!r},run_name='gltf_verify')
report=json.loads(pathlib.Path({str(directory / 'acceptance.json')!r}).read_text())
results={{}}
for case, expected in report['cases'].items():
    actual=ns['imported'](pathlib.Path({str(directory)!r}) / ('browser-'+case+'.glb'))
    error=ns['compare'](actual,expected['snapshot'])
    results[case]={{'maximumPositionError':error,'triangles':actual['triangles'],'shapeKeys':actual['shapeKeys']}}
bpy.ops.wm.open_mainfile(filepath={str(blend)!r})
saved=ns['snapshot']()
ns['compare'](saved,report['saved'])
assert report['saved']==report['reopened']
pathlib.Path({str(target)!r}).write_text(json.dumps({{'cases':results,'saved':saved}},indent=2))
'''
    with (directory / 'desktop-verification.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--disable-autoexec',
            '--python-exit-code', '1', '--python-expr', expression], cwd=ROOT,
            stdout=log, stderr=subprocess.STDOUT, check=True)
    actual = json.loads(target.read_text())
    result = dict(bundleVersion=report['bundleVersion'], exports=report['exports'],
        cases=actual['cases'], savedBlendVerified=True, blendSha256=report['blend']['sha256'],
        sourceSha256=report['sourceSha256'])
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
