#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Join the real browser recordings and burn in short inspection captions."""
import json
from pathlib import Path
import subprocess
from prepare import ROOT


def timestamp(seconds):
    centiseconds=round(seconds*100)
    return f'{centiseconds//360000}:{centiseconds//6000%60:02}:{centiseconds//100%60:02}.{centiseconds%100:02}'


def main():
    base=ROOT/'build/native'
    story=json.loads((base/'walkthrough/recording.json').read_text())
    cycles=json.loads((base/'cycles-evidence/acceptance.json').read_text())
    assert story['bundleVersion'] == cycles['bundleVersion']
    duration=float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration',
                    '-of','default=noprint_wrappers=1:nokey=1',story['videoPath']],text=True))
    chapters=story['chapters'] + [{'seconds':duration,'title':'Cycles CPU render: 128 x 128, 16 samples. Checked against desktop Blender.'}]
    full_duration=duration+cycles['clipEnd']-cycles['clipStart']
    subtitles=base/'walkthrough/walkthrough.ass'
    subtitles.write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 800
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,DejaVu Sans,20,&H00FFFFFF,&H00FFFFFF,&H00222222,&H99222222,0,0,0,0,100,100,0,0,3,2,0,8,20,20,35,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
''' + '\n'.join(f'Dialogue: 0,{timestamp(chapter["seconds"])},{timestamp(chapters[i+1]["seconds"] if i+1<len(chapters) else full_duration)},Default,,0,0,0,,{chapter["title"]}' for i,chapter in enumerate(chapters))+'\n')
    destination=ROOT/'web/docs/modeling-cycles-walkthrough.mp4'
    subprocess.run(['ffmpeg','-hide_banner','-y','-i',story['videoPath'],
        '-ss',str(cycles['clipStart']),'-t',str(cycles['clipEnd']-cycles['clipStart']),'-i',cycles['videoPath'],
        '-filter_complex','[0:v]fps=24,setpts=PTS-STARTPTS[v0];[1:v]fps=24,setpts=PTS-STARTPTS[v1];[v0][v1]concat=n=2:v=1:a=0,subtitles=walkthrough.ass[v]',
        '-map','[v]','-an','-c:v','libx264','-preset','fast','-crf','23','-pix_fmt','yuv420p','-movflags','+faststart',str(destination)],
        cwd=subtitles.parent,check=True)
    full_duration=float(subprocess.check_output(['ffprobe','-v','error','-show_entries','format=duration',
        '-of','default=noprint_wrappers=1:nokey=1',str(destination)],text=True))
    report={'bundleVersion':story['bundleVersion'],'chapters':chapters,'durationSeconds':full_duration,
            'bytes':destination.stat().st_size,'source':'Playwright browser recordings','captions':True,
            'cyclesReferenceMeanAbsoluteError':cycles['meanAbsoluteError']}
    (ROOT/'web/docs/modeling-recording.json').write_text(json.dumps(report,indent=2)+'\n')
    coverage=ROOT/'web/native/FEATURES.md'
    start='<!-- WEB_RECORDING_CHAPTERS -->'
    end='<!-- /WEB_RECORDING_CHAPTERS -->'
    text=coverage.read_text()
    before,tail=text.split(start,1)
    _,after=tail.split(end,1)
    table='\n| Time | What to inspect |\n| --- | --- |\n'
    table+='\n'.join(f'| {int(chapter["seconds"])//60}:{int(chapter["seconds"])%60:02} | {chapter["title"]} |' for chapter in chapters)+'\n'
    coverage.write_text(before+start+table+end+after)
    print(json.dumps(report))


if __name__ == '__main__': main()
