#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Caption the actual image/vector and Freestyle browser recordings."""
import hashlib
import json
import subprocess
from prepare import ROOT
from native_recording import timestamp


def main():
    base=ROOT/'build/native'
    image=json.loads((base/'image-vector-evidence-hardware/acceptance.json').read_text())
    freestyle=json.loads((base/'freestyle-evidence/acceptance.json').read_text())
    assert image['hardware'] and image['offlineReopen'] and not image['browserErrors']
    assert not freestyle['softwareDisplay'] and not freestyle['browserErrors']
    assert image['bundleVersion']==freestyle['bundleVersion']
    stories=[image,freestyle];chapters=[];offset=0
    for story in stories:
        duration=float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration',
            '-of','default=noprint_wrappers=1:nokey=1',story['videoPath']],text=True))
        chapters.extend(dict(seconds=offset+chapter['seconds'],title=chapter['title']) for chapter in story['chapters'])
        offset+=duration
    work=base/'dependency-recording';work.mkdir(exist_ok=True)
    subtitles=work/'chapters.ass'
    subtitles.write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 800
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,DejaVu Sans,20,&H00FFFFFF,&H00FFFFFF,&H00222222,&H99222222,0,0,0,0,100,100,0,0,3,2,0,8,20,20,35,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
'''+ '\n'.join(f'Dialogue: 0,{timestamp(c["seconds"])},{timestamp(chapters[i+1]["seconds"] if i+1<len(chapters) else offset)},Default,,0,0,0,,{c["title"]}' for i,c in enumerate(chapters))+'\n')
    destination=ROOT/'web/docs/image-vector-freestyle-walkthrough.mp4'
    subprocess.run(['ffmpeg','-hide_banner','-y','-i',image['videoPath'],'-i',freestyle['videoPath'],
        '-filter_complex','[0:v]fps=24,setpts=PTS-STARTPTS[v0];[1:v]fps=24,setpts=PTS-STARTPTS[v1];[v0][v1]concat=n=2:v=1:a=0,subtitles=chapters.ass[v]',
        '-map','[v]','-an','-c:v','libx264','-preset','fast','-crf','23','-pix_fmt','yuv420p',
        '-movflags','+faststart',str(destination)],cwd=work,check=True)
    report=dict(bundleVersion=image['bundleVersion'],chapters=chapters,durationSeconds=offset,
        bytes=destination.stat().st_size,source='Hardware Chromium browser recordings',captions=True,
        path=str(destination.relative_to(ROOT)),sha256=hashlib.sha256(destination.read_bytes()).hexdigest())
    (ROOT/'web/docs/image-vector-recording.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
