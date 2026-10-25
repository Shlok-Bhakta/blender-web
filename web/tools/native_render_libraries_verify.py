#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently reopen and render the browser's saved scene in native Blender."""
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
    args=parser.parse_args();directory=args.directory.resolve()
    report=json.loads((directory/'acceptance.json').read_text())
    source=ROOT/'web/tests/render-libraries.py'
    assert report['sourceSha256']==hashlib.sha256(source.read_bytes()).hexdigest()
    blend=directory/'render-libraries.blend'
    assert hashlib.sha256(blend.read_bytes()).hexdigest()==report['blend']['sha256']
    for name,image in report['images'].items():
        assert hashlib.sha256((directory/name).read_bytes()).hexdigest()==image['sha256']
    output=directory/'native';output.mkdir(exist_ok=True)
    expression=("import bpy,runpy,pathlib,json; ns=runpy.run_path("+repr(str(source))+
        "); bpy.ops.wm.open_mainfile(filepath="+repr(str(blend))+
        "); assert bpy.context.scene.name=='Path guiding and denoising'; "
        "assert bpy.context.scene.cycles.use_guiding and bpy.context.scene.cycles.use_denoising; "
        "result={case:ns['render_case'](pathlib.Path("+repr(str(output))+"),case) for case in ns['CASES']}; "
        "pathlib.Path("+repr(str(output/'results.json'))+").write_text(json.dumps(result))")
    with (output/'readback.log').open('w') as log:
        subprocess.run([args.blender,'--background','--factory-startup','--python-exit-code','1',
            '--log','cycles','--log-level','trace','--python-expr',expression],
            stdout=log,stderr=subprocess.STDOUT,check=True)
    log=(output/'readback.log').read_text();assert 'Path guiding field valid: 1' in log
    comparisons={}
    def pixels(path):
        return subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-frames:v','1',
            '-threads','1','-pix_fmt','rgb24','-f','rawvideo','pipe:1'])
    for name in json.loads((output/'results.json').read_text()):
        reference=(ROOT/'web/tests/fixtures/render-libraries'/f'{name}.png' if report['fallback'] else directory/f'{name}.png')
        a,b=pixels(output/f'{name}.png'),pixels(reference);assert len(a)==len(b)==192*144*3
        errors=[abs(x-y) for x,y in zip(a,b)]
        image=dict(meanAbsoluteError=sum(errors)/len(errors),outlierFraction=sum(e>25 for e in errors)/len(errors),maximumError=max(errors))
        guided=name in ('guided','volume-guided')
        assert image['meanAbsoluteError']<(4 if guided else 1.5) and image['outlierFraction']<(.035 if guided else .005),(name,image)
        comparisons[name]=image
    hevc={}
    if not report['fallback']:
        for depth in (8,10,12):
            path=directory/f'hevc-{depth}.mkv'
            assert hashlib.sha256(path.read_bytes()).hexdigest()==report['hevc'][str(depth)]['sha256']
            stream=json.loads(subprocess.check_output(['ffprobe','-v','error','-count_frames',
                '-select_streams','v:0','-show_entries','stream=pix_fmt,width,height,nb_read_frames',
                '-of','json',str(path)],text=True))['streams'][0]
            pixelFormat='yuv444p' if depth==8 else f'yuv444p{depth}le'
            assert stream['pix_fmt']==pixelFormat and int(stream['nb_read_frames'])==3,stream
            assert [stream['width'],stream['height']]==[96,64]
            def samples(path):
                data=subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-threads','1',
                    '-pix_fmt',pixelFormat,'-f','rawvideo','pipe:1'])
                if depth==8:return list(data)
                import struct
                return struct.unpack('<'+str(len(data)//2)+'H',data)
            actual=samples(path);expected=samples(ROOT/'web/tests/fixtures/render-libraries'/path.name)
            assert len(actual)==len(expected)==96*64*3*3
            errors=[abs(x-y) for x,y in zip(actual,expected)]
            comparison=dict(maximumError=max(errors),meanAbsoluteError=sum(errors)/len(errors),
                comparisonSpace='Decoded YUV sample values at the encoded bit depth',
                pixelFormat=pixelFormat,frames=3,sha256=report['hevc'][str(depth)]['sha256'])
            # Native SIMD and Wasm float->YUV quantization can differ by one
            # least-significant bit. Compare before RGB matrix amplification.
            assert comparison['maximumError']<=1 and comparison['meanAbsoluteError']<.02,comparison
            hevc[str(depth)]=comparison
    result=dict(verifierSourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),bundleVersion=report['bundleVersion'],sourceSha256=report['sourceSha256'],
        savedBlendVerified=True,blendSha256=report['blend']['sha256'],images=comparisons,hevcExports=hevc)
    (directory/'native-verification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))


if __name__=='__main__':main()
