#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the browser's saved tracking data with the pinned desktop Blender."""
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
    blend = directory / 'tracking.blend'
    assert hashlib.sha256(blend.read_bytes()).hexdigest() == report['blend']['sha256']
    target = directory / 'desktop-tracks.json'
    expression = (f"import bpy,runpy,json,pathlib; bpy.ops.wm.open_mainfile(filepath={str(blend)!r}); "
        f"ns=runpy.run_path({str(ROOT / 'web/tests/tracking.py')!r},run_name='tracking_verify'); "
        f"pathlib.Path({str(target)!r}).write_text(json.dumps(ns['saved_check']()))")
    with (directory / 'desktop-verification.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--disable-autoexec',
            '--python-exit-code', '1', '--python-expr', expression], cwd=ROOT,
            stdout=log, stderr=subprocess.STDOUT, check=True)
    actual = json.loads(target.read_text())
    assert actual == report['saved'] == report['reopened']
    result = dict(bundleVersion=report['bundleVersion'], blendSha256=report['blend']['sha256'],
        desktop=actual, sourceSha256=hashlib.sha256((ROOT / 'web/tests/tracking.py').read_bytes()).hexdigest())
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
