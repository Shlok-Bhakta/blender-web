#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author deterministic media fixtures with the host FFmpeg tool."""
import array
import math
from pathlib import Path
import subprocess
import wave

from prepare import ROOT


def main():
    destination = ROOT / 'web/tests/fixtures/media'
    destination.mkdir(parents=True, exist_ok=True)
    pcm = array.array('h')
    for index in range(48000):
        time = index / 48000
        envelope = min(1, index / 240, (48000 - index) / 240)
        pcm.extend(round(12000 * envelope * math.sin(2 * math.pi * frequency * time))
                   for frequency in (440, 660))
    if __import__('sys').byteorder != 'little':
        pcm.byteswap()
    with wave.open(str(destination / 'tone.wav'), 'wb') as output:
        output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        output.writeframes(pcm.tobytes())
    for suffix, codec in [('flac', 'flac'), ('ogg', 'libvorbis'), ('mp3', 'libmp3lame')]:
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(destination / 'tone.wav'),
                        '-c:a', codec, str(destination / f'tone.{suffix}')], check=True)
    # Distinct solid frames make dropped frames, color conversion and seeks
    # measurable. The source is authored directly, independent of the port.
    raw = bytearray()
    for frame in range(24):
        raw.extend(bytes(((frame * 17) % 256, (frame * 37) % 256, (frame * 61) % 256)) * (128 * 96))
    for filename, video, audio, extra in [
        ('reference.mkv', 'ffv1', 'pcm_s16le', []),
        ('reference.mp4', 'libx264', 'aac', ['-pix_fmt', 'yuv420p', '-preset', 'fast']),
        ('reference.webm', 'libvpx-vp9', 'libopus', ['-pix_fmt', 'yuv420p', '-deadline', 'realtime']),
    ]:
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                        '-s', '128x96', '-r', '24', '-i', 'pipe:0',
                        '-i', str(destination / 'tone.wav'), '-c:v', video, '-c:a', audio,
                        '-threads', '4', *extra, '-shortest', str(destination / filename)],
                       input=raw, check=True)


if __name__ == '__main__':
    main()
