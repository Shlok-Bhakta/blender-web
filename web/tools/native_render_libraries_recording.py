#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Caption the actual hardware browser recording after independent native checks."""
import hashlib
import json
import shutil
import subprocess
from prepare import ROOT
from native_recording import timestamp


def main():
    evidence=ROOT/'build/native/render-libraries-evidence-hardware'
    report=json.loads((evidence/'acceptance.json').read_text())
    verified=json.loads((evidence/'native-verification.json').read_text())
    assert report['hardware'] and not report['fallback'] and report['offlineReopen']
    assert not report['browserErrors'] and verified['savedBlendVerified']
    assert set(verified['hevcExports'])=={'8','10','12'}
    duration=float(subprocess.check_output(['ffprobe','-v','error','-show_entries',
        'format=duration','-of','default=noprint_wrappers=1:nokey=1',report['videoPath']],text=True))
    work=ROOT/'build/native/render-libraries-recording';work.mkdir(exist_ok=True)
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
'''+ '\n'.join(f'Dialogue: 0,{timestamp(chapter["seconds"])},{timestamp(chapters[i+1]["seconds"] if i+1<len(chapters) else duration)},Default,,0,0,0,,{chapter["title"]}' for i,chapter in enumerate(chapters))+'\n')
    destination=ROOT/'web/docs/render-libraries-walkthrough.mp4'
    subprocess.run(['ffmpeg','-v','warning','-y','-i',report['videoPath'],
        '-vf','fps=24,subtitles=chapters.ass','-c:v','libx264','-preset','fast','-crf','23',
        '-pix_fmt','yuv420p','-an','-movflags','+faststart',str(destination)],cwd=work,check=True)
    screenshot=ROOT/'web/docs/render-libraries-browser.png'
    shutil.copy2(evidence/'render-libraries-browser.png',screenshot)
    result=dict(bundleVersion=report['bundleVersion'],sourceSha256=report['sourceSha256'],
        durationSeconds=duration,chapters=chapters,captions=True,
        source='Real hardware Chromium capture, including original Blender renders and offline reopening',
        path=str(destination.relative_to(ROOT)),bytes=destination.stat().st_size,
        sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
        screenshot=dict(path=str(screenshot.relative_to(ROOT)),sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest()))
    (ROOT/'web/docs/render-libraries-recording.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))


if __name__=='__main__':main()
