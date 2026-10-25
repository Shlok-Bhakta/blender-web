#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Caption the actual Embree/Cycles browser recording and retain its bundle hash."""
import hashlib
import json
import shutil
import subprocess
from prepare import ROOT
from native_recording import timestamp


def main():
    evidence=ROOT/'build/native/embree-evidence-hardware'
    report=json.loads((evidence/'acceptance.json').read_text())
    verified=json.loads((evidence/'native-verification.json').read_text())
    assert report['hardware'] and report['offlineReopen'] and report['videoCaptured']
    assert verified['savedBlendVerified'] and not report['browserErrors']
    duration=float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration',
                    '-of','default=noprint_wrappers=1:nokey=1',report['videoPath']],text=True))
    work=ROOT/'build/native/embree-recording';work.mkdir(exist_ok=True)
    chapters=report['chapters']
    (work/'chapters.ass').write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 800
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,DejaVu Sans,20,&H00FFFFFF,&H00FFFFFF,&H00222222,&H99222222,0,0,0,0,100,100,0,0,3,2,0,8,20,20,35,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
'''+ '\n'.join(f'Dialogue: 0,{timestamp(chapter["seconds"])},{timestamp(chapters[index+1]["seconds"] if index+1<len(chapters) else duration)},Default,,0,0,0,,{chapter["title"]}' for index,chapter in enumerate(chapters))+'\n')
    destination=ROOT/'web/docs/embree-walkthrough.mp4'
    subprocess.run(['ffmpeg','-v','warning','-y','-i',report['videoPath'],'-vf','fps=24,subtitles=chapters.ass',
                    '-an','-c:v','libx264','-preset','fast','-crf','23','-pix_fmt','yuv420p',
                    '-movflags','+faststart',str(destination)],cwd=work,check=True)
    screenshot=ROOT/'web/docs/embree-cycles.png';shutil.copy2(evidence/'embree-cycles.png',screenshot)
    result=dict(bundleVersion=report['bundleVersion'],sourceSha256=report['sourceSha256'],
                path=str(destination.relative_to(ROOT)),bytes=destination.stat().st_size,
                sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),durationSeconds=duration,
                chapters=chapters,captions=True,source='Hardware Chromium recording of original Blender/Cycles',
                screenshot=dict(path=str(screenshot.relative_to(ROOT)),sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest()))
    (ROOT/'web/docs/embree-recording.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))

if __name__=='__main__':main()
