#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently decode lossless Wasm HEVC exports with host FFmpeg."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from prepare import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix',type=Path,default=ROOT/'build/native/hevc-check')
    args=parser.parse_args();prefix=args.prefix.resolve();images=[]
    for depth in (8,10,12):
        path=Path(str(prefix)+f'-{depth}.hevc')
        metadata=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0',
            '-count_frames','-show_entries','stream=pix_fmt,profile,width,height,nb_read_frames',
            '-of','json',str(path)],text=True))['streams'][0]
        pixelFormat='yuv420p' if depth==8 else f'yuv420p{depth}le'
        assert metadata['pix_fmt']==pixelFormat and int(metadata['nb_read_frames'])==3,metadata
        assert [metadata['width'],metadata['height']]==[96,64]
        raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-threads','1',
            '-pix_fmt',pixelFormat,'-f','rawvideo','pipe:1'])
        expected=bytearray()
        for frame in range(3):
            for plane in range(3):
                for y in range(64>>(1 if plane else 0)):
                    for x in range(96>>(1 if plane else 0)):
                        value=(x*19+y*37+plane*113+frame*67)&((1<<depth)-1)
                        expected.extend(bytes([value]) if depth==8 else struct.pack('<H',value))
        assert raw==expected,f'Host decoder changed {depth}-bit lossless pixels'
        images.append(dict(depth=depth,losslessExact=True,metadata=metadata,
            bytes=path.stat().st_size,sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    result=dict(exports=images)
    Path(str(prefix)+'-native-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__=='__main__':main()
