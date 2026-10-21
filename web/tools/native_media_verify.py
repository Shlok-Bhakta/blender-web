#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently decode browser exports and captured Web Audio with host FFmpeg."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import subprocess

from prepare import ROOT


def decode(path, kind):
    options = ['-an', '-pix_fmt', 'rgb24', '-f', 'rawvideo'] if kind == 'video' else [
        '-vn', '-ac', '2', '-ar', '48000', '-f', 'f32le']
    return subprocess.check_output(['ffmpeg', '-v', 'error', '-threads', '4', '-i', str(path),
                                    '-threads', '4', *options, 'pipe:1'])


def probe(path):
    return json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-count_frames',
        '-show_streams', '-of', 'json', str(path)]))['streams']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    report = json.loads((directory / 'acceptance.json').read_text())
    reference = ROOT / 'web/tests/fixtures/media'
    result = {'bundleVersion':report['bundleVersion'], 'videos':{}, 'audio':{},
              'ffmpeg':subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0]}
    if not report['fallback']:
        for extension in ['mp4', 'webm']:
            path = directory / f'export.{extension}'
            streams = probe(path)
            video = next(stream for stream in streams if stream['codec_type'] == 'video')
            audio = next(stream for stream in streams if stream['codec_type'] == 'audio')
            assert int(video['nb_read_frames']) == 24 and [video['width'], video['height']] == [128, 96]
            assert audio['codec_name'] == ('aac' if extension == 'mp4' else 'opus')
            actual, expected = decode(path, 'video'), decode(reference / path.name, 'video')
            assert len(actual) == len(expected) == 24 * 128 * 96 * 3
            errors = [abs(a - b) for a, b in zip(actual, expected)]
            maximum, mean = max(errors), sum(errors) / len(errors)
            assert maximum <= 6 and mean < 1.5, (extension, maximum, mean)
            result['videos'][extension] = dict(codec=video['codec_name'], frames=24,
                audioCodec=audio['codec_name'], maxPixelError=maximum, meanPixelError=mean)
    for extension in ['wav', 'flac', 'ogg', 'mp3']:
        path = directory / f'export.{extension}'
        actual, expected = decode(path, 'audio'), decode(reference / path.name, 'audio')
        assert len(actual) == len(expected), (extension, len(actual), len(expected))
        av, ev = array.array('f', actual), array.array('f', expected)
        maximum = max(abs(a - b) for a, b in zip(av, ev))
        assert maximum < 0.0005, (extension, maximum)
        result['audio'][extension] = dict(samples=len(av), maxSampleError=maximum,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    # A direct DFT at a small set of frequencies avoids adding a host NumPy
    # requirement to this independent acceptance tool.
    import cmath
    import math
    data = array.array('f', decode(directory / 'playback-audio.webm', 'audio'))
    frames = len(data) // 2
    active = [index for index in range(frames) if max(abs(data[index * 2]), abs(data[index * 2 + 1])) > .01]
    assert len(active) > 48000 * 2, len(active)
    first, last = active[0], active[-1]
    assert last - first > 48000 * 2.5
    start = first + 12000
    length = min(24000, last - start)
    frequencies = []
    for channel, center in enumerate([440, 660]):
        powers = {}
        for frequency in range(center - 6, center + 7, 2):
            step = cmath.exp(-2j * math.pi * frequency / 48000)
            oscillator, value = 1 + 0j, 0j
            for index in range(length):
                value += data[(start + index) * 2 + channel] * oscillator
                oscillator *= step
            powers[frequency] = abs(value)
        peak = max(powers, key=powers.get)
        assert abs(peak - center) <= 2, (channel, powers)
        frequencies.append(peak)
    result['playbackCapture'] = dict(frames=frames, nonzeroFrames=len(active),
        firstSignalSeconds=first / 48000, lastSignalSeconds=last / 48000,
        stereoPeakHz=frequencies, sha256=hashlib.sha256((directory / 'playback-audio.webm').read_bytes()).hexdigest())
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
