#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the browser's saved Cycles scene independently in pinned native Blender."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--blender',default='blender')
    args=parser.parse_args()
    directory=args.directory.resolve()
    report=json.loads((directory/'acceptance.json').read_text())
    assert report['sourceSha256']==hashlib.sha256((ROOT/'web/tests/embree.py').read_bytes()).hexdigest()
    for name,entry in report['images'].items():
        assert hashlib.sha256((directory/name).read_bytes()).hexdigest()==entry['sha256']
    assert hashlib.sha256((directory/'embree.blend').read_bytes()).hexdigest()==report['blend']['sha256']
    target=directory/'native-scene.json'
    rendered=directory/'native-embree-1.png'
    expression=("import bpy,runpy,json,pathlib; ns=runpy.run_path("+repr(str(ROOT/'web/tests/embree.py'))+
                "); bpy.ops.wm.open_mainfile(filepath="+repr(str(directory/'embree.blend'))+
                "); pathlib.Path("+repr(str(target))+").write_text(json.dumps(ns['inspect_scene']())); "
                "scene=bpy.context.scene; ns['select_backend'](scene,'EMBREE'); scene.frame_set(1); "
                "scene.render.filepath="+repr(str(rendered))+"; bpy.ops.render.render(write_still=True)")
    with (directory/'native-readback.log').open('w') as log:
        subprocess.run([args.blender,'--background','--factory-startup','--python-exit-code','1',
                        '--log','cycles','--log-level','info',
                        '--python-expr',expression],stdout=log,stderr=subprocess.STDOUT,check=True)
    scene=json.loads(target.read_text())
    assert scene==report['scene'],(scene,report['scene'])
    assert 'Using EMBREE layout.' in (directory/'native-readback.log').read_text()
    reference=(ROOT/'web/tests/fixtures/embree/embree-1.png' if report['fallback'] else
               directory/'embree-1.png')
    def pixels(path):
        return subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-frames:v','1',
                                        '-threads','1','-pix_fmt','rgb24','-f','rawvideo','pipe:1'])
    actual,expected=pixels(rendered),pixels(reference)
    assert len(actual)==len(expected)==256*192*3
    errors=[abs(a-b) for a,b in zip(actual,expected)]
    image=dict(path=rendered.name,sha256=hashlib.sha256(rendered.read_bytes()).hexdigest(),
               reference='native fixture' if report['fallback'] else 'browser Embree render',
               meanAbsoluteError=sum(errors)/len(errors),
               outlierFraction=sum(error>25 for error in errors)/len(errors),maximumError=max(errors))
    assert image['meanAbsoluteError']<1.5 and image['outlierFraction']<.005,image
    result=dict(bundleVersion=report['bundleVersion'],sourceSha256=report['sourceSha256'],
                savedBlendVerified=True,scene=scene,
                nativeRender=image,
                blendSha256=hashlib.sha256((directory/'embree.blend').read_bytes()).hexdigest())
    (directory/'native-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))

if __name__=='__main__':main()
