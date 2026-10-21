#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Caption the real sequencer recording and include its captured browser audio."""
import hashlib
import json
import shutil
import subprocess

from prepare import ROOT
from native_recording import timestamp


def main():
    evidence = ROOT / 'build/native/media-evidence-hardware'
    report = json.loads((evidence / 'acceptance.json').read_text())
    verified = json.loads((evidence / 'native-verification.json').read_text())
    assert report['hardware'] and report['offlineReopen'] and not report['browserErrors']
    assert verified['playbackCapture']['stereoPeakHz'] == [440, 660]
    duration = float(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
        'format=duration', '-of', 'default=noprint_wrappers=1:nokey=1', report['videoPath']], text=True))
    work = ROOT / 'build/native/media-recording'
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
''' + '\n'.join(f'Dialogue: 0,{timestamp(chapter["seconds"])},{timestamp(chapters[index+1]["seconds"] if index+1<len(chapters) else duration)},Default,,0,0,0,,{chapter["title"]}' for index,chapter in enumerate(chapters)) + '\n')
    destination = ROOT / 'web/docs/media-walkthrough.mp4'
    delay = round(report['audioCaptureOffsetSeconds'] * 1000)
    subprocess.run(['ffmpeg', '-v', 'warning', '-y', '-i', report['videoPath'],
        '-i', str(evidence / 'playback-audio.webm'), '-filter_complex',
        f'[0:v]fps=24,subtitles=chapters.ass[v];[1:a]adelay={delay}:all=1,apad[a]',
        '-map', '[v]', '-map', '[a]', '-t', str(duration), '-c:v', 'libx264',
        '-preset', 'fast', '-crf', '23', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
        '-b:a', '160k', '-movflags', '+faststart', str(destination)], cwd=work, check=True)
    screenshot = ROOT / 'web/docs/media-sequencer.png'
    shutil.copy2(evidence / 'media-sequencer.png', screenshot)
    metadata = dict(bundleVersion=report['bundleVersion'], chapters=chapters,
        durationSeconds=duration, bytes=destination.stat().st_size, captions=True,
        source='Hardware Chromium recording with captured Web Audio output',
        path=str(destination.relative_to(ROOT)), sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
        audioCaptureOffsetSeconds=report['audioCaptureOffsetSeconds'],
        screenshot=dict(path=str(screenshot.relative_to(ROOT)),sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest()))
    (ROOT / 'web/docs/media-recording.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
