#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the actual hardware Blender ctypes capture after native verification."""
import hashlib
import json
import shutil
import subprocess
from prepare import ROOT


def main():
    evidence = ROOT / 'build/native/python-ctypes-evidence-hardware'
    report = json.loads((evidence / 'acceptance.json').read_text())
    verified = json.loads((evidence / 'native-verification.json').read_text())
    assert report['hardware'] and not report['fallback']
    assert report['offlineReopen'] and report['offlineRepeatedChecks']
    assert not report['browserErrors'] and not report['startupPythonErrors']
    assert verified['savedBlendVerified'] and verified['exactFunctionalResults']
    assert report['bundleVersion'] == verified['bundleVersion']
    destination = ROOT / 'web/docs/python-ctypes-walkthrough.mp4'
    subprocess.run(['ffmpeg', '-v', 'warning', '-y', '-i', report['videoPath'],
                    '-vf', 'fps=24', '-c:v', 'libx264', '-preset', 'fast', '-crf', '23',
                    '-pix_fmt', 'yuv420p', '-an', '-movflags', '+faststart', str(destination)], check=True)
    screenshot = ROOT / 'web/docs/python-ctypes-browser.png'
    shutil.copy2(evidence / 'ctypes-browser.png', screenshot)
    duration = float(subprocess.check_output(['ffprobe', '-v', 'error', '-show_entries',
        'format=duration', '-of', 'default=noprint_wrappers=1:nokey=1', str(destination)], text=True))
    result = dict(bundleVersion=report['bundleVersion'], sourceSha256=report['sourceSha256'],
                  durationSeconds=duration, chapters=report['chapters'], captions=False,
                  source='Actual hardware Chromium capture of original Blender, ctypes and offline repeated checks',
                  path=str(destination.relative_to(ROOT)), bytes=destination.stat().st_size,
                  sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
                  screenshot=dict(path=str(screenshot.relative_to(ROOT)),
                                  sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest()))
    (ROOT / 'web/docs/python-ctypes-recording.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
