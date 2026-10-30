#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read browser OSL query exports and the saved scene in native Blender."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--blender', default='blender')
    args = parser.parse_args()
    directory = args.directory.resolve()
    report = json.loads((directory / 'acceptance.json').read_text())
    source = ROOT / 'web/native/tests/osl-python.py'
    runner = ROOT / 'web/tests/osl-python.cjs'
    assert report['sourceSha256'] == digest(source)
    assert report['runnerSourceSha256'] == digest(runner)
    assert report['fixtureSha256'] == digest(directory / 'web_query.oso')
    for name, entry in report['files'].items():
        path = directory / name
        assert path.stat().st_size == entry['bytes'] and digest(path) == entry['sha256'], name
    native = directory / 'native'
    native.mkdir(exist_ok=True)
    script = native / 'verify.py'
    script.write_text('import bpy,json,runpy,oslquery\nfrom pathlib import Path\n'
        f'assert bpy.app.build_hash.decode() == {json.loads((ROOT / "web/upstream.json").read_text())["blenderRevision"][:12]!r}\n'
        f'directory=Path({str(directory)!r})\n'
        'expected=json.loads((directory/"acceptance.json").read_text())["result"]\n'
        f'actual=runpy.run_path({str(source)!r})["run"]('
        'directory/"native/reference",directory/"web_query.oso")\n'
        'assert json.loads(json.dumps(actual))==expected\n'
        'unicode_query=oslquery.OSLQuery(str(directory/"shader-雪.oso"))\n'
        'assert not unicode_query.geterror() and unicode_query.shadername()==expected["query"]["name"]\n'
        'bpy.ops.wm.open_mainfile(filepath=str(directory/"osl-python.blend"))\n'
        'assert json.loads(bpy.data.texts["OSL Python query proof"].as_string())==expected\n'
        'result=dict(nativeAPIsEqual=True,savedBlendVerified=True,unicodeFileReadback=True,blenderRevision=bpy.app.build_hash.decode())\n'
        '(directory/"native/result.json").write_text(json.dumps(result,indent=2)+"\\n")\n')
    with (native / 'readback.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
                        '--python', script], stdout=log, stderr=subprocess.STDOUT, check=True)
    result = json.loads((native / 'result.json').read_text())
    result.update(bundleVersion=report['bundleVersion'], sourceSha256=digest(source),
                  verifierSourceSha256=digest(Path(__file__)),
                  runnerSourceSha256=digest(runner), files=report['files'])
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
