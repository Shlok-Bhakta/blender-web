#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Caption the actual USD browser exchange and offline recording."""
import hashlib
import json
import subprocess
from prepare import ROOT
from native_recording import timestamp


def main():
    report = json.loads((ROOT / 'build/native/usd-evidence-hardware/acceptance.json').read_text())
    assert report['hardware'] and report['offlineReopen'] and not report['browserErrors']
    assert len(report['formats']) == 3 and report['hookEvents'].count('export') == 3
    duration = float(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
        'format=duration', '-of', 'default=noprint_wrappers=1:nokey=1', report['videoPath']], text=True))
    work = ROOT / 'build/native/usd-recording'
    work.mkdir(exist_ok=True)
    chapters = report['chapters']
    (work / 'chapters.ass').write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 800
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,DejaVu Sans,20,&H00FFFFFF,&H00FFFFFF,&H00222222,&H99222222,0,0,0,0,100,100,0,0,3,2,0,8,20,20,35,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
''' + '\n'.join(f'Dialogue: 0,{timestamp(c["seconds"])},{timestamp(chapters[i+1]["seconds"] if i+1<len(chapters) else duration)},Default,,0,0,0,,{c["title"]}' for i,c in enumerate(chapters)) + '\n')
    destination = ROOT / 'web/docs/usd-materialx-walkthrough.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-y', '-i', report['videoPath'],
        '-vf', 'fps=24,subtitles=chapters.ass', '-an', '-c:v', 'libx264', '-preset', 'fast',
        '-crf', '23', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(destination)],
        cwd=work, check=True)
    result = dict(bundleVersion=report['bundleVersion'], chapters=chapters,
        durationSeconds=duration, bytes=destination.stat().st_size,
        source='Hardware Chromium browser recording', captions=True,
        path=str(destination.relative_to(ROOT)), sha256=hashlib.sha256(destination.read_bytes()).hexdigest())
    (ROOT / 'web/docs/usd-recording.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
