#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the same small media workloads on native and browser Blender."""
import hashlib
import json
from prepare import ROOT


def main():
    base = ROOT / 'build/native'
    desktop = json.loads((ROOT / 'web/tests/fixtures/media/reference.json').read_text())
    browser = json.loads((base / 'media-benchmark/acceptance.json').read_text())
    assert browser['hardware'] and not browser['profiled'] and not browser['videoCaptured']
    source_hash = hashlib.sha256((ROOT / 'web/tests/media.py').read_bytes()).hexdigest()
    assert desktop['sourceSha256'] == browser['sourceSha256'] == source_hash
    decode = {}
    for name, native in desktop['audio'].items():
        web = browser['audio'][name]
        assert len(native['decodeRuns']) == len(web['decodeRuns']) == 3
        decode[name] = dict(nativeMedianSeconds=native['decodeSeconds'],
            browserMedianSeconds=web['decodeSeconds'],
            browserToNative=web['decodeSeconds'] / native['decodeSeconds'],
            nativeRuns=native['decodeRuns'], browserRuns=web['decodeRuns'])
    export = {}
    for name, native in desktop['render']['videos'].items():
        web = browser['render']['videos'][name]
        assert len(native['runs']) == len(web['runs']) == 3
        export[name] = dict(nativeMedianSeconds=native['seconds'],
            browserMedianSeconds=web['seconds'], browserToNative=web['seconds'] / native['seconds'],
            nativeFps=24 / native['seconds'], browserFps=24 / web['seconds'],
            nativeRuns=native['runs'], browserRuns=web['runs'])
    result = dict(bundleVersion=browser['bundleVersion'], sourceSha256=source_hash,
        adapter=browser['adapter'], resources=browser['resources'], decode=decode, export=export,
        playback=browser['playback'],
        methodology='Three sequential measurements per operation on one-second stereo clips and 24-frame 128x96 video, with a 512x512 AV1 probe to satisfy upstream tiling. Original Audaspace decoding includes cache creation and the NumPy transfer. Video timings include Blender sequencer evaluation, encoding and file output. Native Blender runs in background mode; browser Blender runs its original UI. The browser benchmark disables screen recording and profiling. Playback rate counts timeline advancement under audio sync, which can skip display frames. These are small-workload baselines, not isolated codec throughput or large-timeline results.')
    (ROOT / 'web/docs/media-performance.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
