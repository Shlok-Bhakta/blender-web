#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the native-verified hardware OSL Python recording."""
import hashlib
import json
import shutil
import subprocess
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    evidence = ROOT / 'build/native/osl-python-evidence-hardware'
    report = json.loads((evidence / 'acceptance.json').read_text())
    native = json.loads((evidence / 'native-verification.json').read_text())
    assert report['hardware'] and not report['fallback'] and report['offlineReopen']
    assert not report['browserErrors'] and not report['startupPythonErrors']
    assert native['nativeAPIsEqual'] and native['savedBlendVerified']
    assert native['bundleVersion'] == report['bundleVersion']
    destination = ROOT / 'web/docs/osl-python-walkthrough.mp4'
    subprocess.run(['ffmpeg', '-v', 'warning', '-y', '-i', report['videoPath'],
                    '-vf', 'fps=24', '-c:v', 'libx264', '-preset', 'fast', '-crf', '23',
                    '-pix_fmt', 'yuv420p', '-an', '-movflags', '+faststart', destination], check=True)
    screenshot = ROOT / 'web/docs/osl-python-browser.png'
    shutil.copy2(evidence / screenshot.name, screenshot)
    duration = float(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
        'format=duration', '-of', 'default=noprint_wrappers=1:nokey=1', destination], text=True))
    result = dict(bundleVersion=report['bundleVersion'], sourceSha256=report['sourceSha256'],
                  durationSeconds=duration, chapters=report['chapters'], captions=False,
                  source='Actual hardware Chromium capture of original Blender and OSL Python queries',
                  path=str(destination.relative_to(ROOT)), bytes=destination.stat().st_size,
                  sha256=digest(destination), screenshot=dict(path=str(screenshot.relative_to(ROOT)),
                                                           sha256=digest(screenshot)))
    (ROOT / 'web/docs/osl-python-recording.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
