#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Caption the hardware browser's actual ocean/Alembic recording."""
import hashlib
import json
import subprocess
from prepare import ROOT
from native_recording import timestamp


def main():
    base = ROOT / 'build/native/ocean-evidence-hardware'
    story = json.loads((base / 'acceptance.json').read_text())
    assert story['hardware'] and story['offlineReopen'] and not story['errors']
    duration = float(subprocess.check_output(['ffprobe', '-v', 'error',
        '-show_entries', 'format=duration', '-of', 'default=noprint_wrappers=1:nokey=1',
        story['videoPath']], text=True))
    subtitles = base / 'ocean.ass'
    subtitles.write_text('''[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 800
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,DejaVu Sans,20,&H00FFFFFF,&H00FFFFFF,&H00222222,&H99222222,0,0,0,0,100,100,0,0,3,2,0,8,20,20,35,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
''' + '\n'.join(f'Dialogue: 0,{timestamp(chapter["seconds"])},{timestamp(story["chapters"][i+1]["seconds"] if i+1<len(story["chapters"]) else duration)},Default,,0,0,0,,{chapter["title"]}'
                for i, chapter in enumerate(story['chapters'])) + '\n')
    destination = ROOT / 'web/docs/ocean-alembic-walkthrough.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-y', '-i', story['videoPath'],
        '-vf', 'fps=24,subtitles=ocean.ass', '-an', '-c:v', 'libx264', '-preset', 'fast',
        '-crf', '23', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', str(destination)],
        cwd=base, check=True)
    report = dict(bundleVersion=story['bundleVersion'], chapters=story['chapters'],
                  durationSeconds=duration, bytes=destination.stat().st_size,
                  source='Hardware Chromium browser recording', captions=True,
                  path=str(destination.relative_to(ROOT)),
                  sha256=hashlib.sha256(destination.read_bytes()).hexdigest())
    (ROOT / 'web/docs/ocean-recording.json').write_text(json.dumps(report, indent=2) + '\n')
    coverage = ROOT / 'web/native/FEATURES.md'
    start, end = '<!-- OCEAN_RECORDING_CHAPTERS -->', '<!-- /OCEAN_RECORDING_CHAPTERS -->'
    before, tail = coverage.read_text().split(start, 1)
    _, after = tail.split(end, 1)
    table = '\n| Time | What to inspect |\n| --- | --- |\n'
    table += '\n'.join(f'| {int(chapter["seconds"])//60}:{int(chapter["seconds"])%60:02} | {chapter["title"]} |'
                       for chapter in story['chapters']) + '\n'
    coverage.write_text(before + start + table + end + after)
    print(json.dumps(report))


if __name__ == '__main__':
    main()
