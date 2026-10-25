#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the same BVH2/Embree scene within each binary and against native Blender."""
import hashlib
import json
import statistics
from collections import Counter
from prepare import ROOT


def profile_summary(base, version):
    report=json.loads((base/'acceptance.json').read_text())
    assert report['bundleVersion']==version and report['profiled'] and not report['videoCaptured']
    trace=base/'browser-trace-0.json'
    events=json.loads(trace.read_text())['traceEvents']
    nodes={}
    samples=Counter()
    for event in events:
        if event['name']!='ProfileChunk':
            continue
        key=(event['pid'],event['tid'],event['id'])
        profile=event['args']['data'].get('cpuProfile',{})
        known=nodes.setdefault(key,{})
        known.update({node['id']:node['callFrame'] for node in profile.get('nodes',[])})
        for node in profile.get('samples',[]):
            if known.get(node,{}).get('url','').endswith('.wasm'):
                samples[key]+=1
    assert len(samples)>1, 'Expected Wasm samples from multiple worker threads'
    resources=json.loads((base/'resources-0.json').read_text())
    assert len(resources)>10
    return dict(traceSha256=hashlib.sha256(trace.read_bytes()).hexdigest(),
                wasmSampleCount=sum(samples.values()),wasmThreadsSampled=len(samples),
                samplesPerThread=sorted(samples.values(),reverse=True),
                resourceSamples=len(resources),
                nativeFunctionNamesAvailable=False,
                limitations='Separate instrumented run of both backends. Wasm samples retain function indices, not C++ names. Sample counts do not measure CPU utilization or establish simultaneous worker activity. Resource counters describe the shared host; WebGPU activity includes Blender UI drawing.')


def main():
    native=json.loads((ROOT/'web/tests/fixtures/embree/reference.json').read_text())
    web=json.loads((ROOT/'build/native/embree-benchmark/acceptance.json').read_text())
    assert web['hardware'] and not web['videoCaptured'] and not web['profiled']
    source=hashlib.sha256((ROOT/'web/tests/embree.py').read_bytes()).hexdigest()
    assert native['sourceSha256']==web['sourceSha256']==source
    cases={}
    for backend in ['BVH2','EMBREE']:
        n=native['benchmark'][backend]['2'];w=web['benchmark'][backend]['2']
        assert len(n['runs'])==len(w['runs'])==3
        cases[backend]=dict(nativeSeconds=n['seconds'],browserSeconds=w['seconds'],
                           browserToNative=w['seconds']/n['seconds'],nativeRuns=n['runs'],browserRuns=w['runs'])
    tiny={}
    for backend in ['BVH2','EMBREE']:
        path=ROOT/'build/native'/('embree-cube-'+backend.lower())/'acceptance.json'
        probe=json.loads(path.read_text())
        assert probe['bundleVersion']==web['bundleVersion'] and probe['bvhBackend']==backend
        assert len(probe['handlerSeconds'])==len(probe['backendLogs'])==6
        assert not probe['videoCaptured'] and not probe['errors']
        assert not probe['softwareDisplay'] and probe['threads']==8
        assert probe['size']==[128,128] and probe['samples']==16
        assert all('Using '+backend+' layout.' in line for line in probe['backendLogs'])
        tiny[backend]=dict(runs=probe['handlerSeconds'],firstSeconds=probe['handlerSeconds'][0],
                           reportSha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                           warmMedianSeconds=statistics.median(probe['handlerSeconds'][1:]))
    result=dict(bundleVersion=web['bundleVersion'],sourceSha256=source,cases=cases,
                browserEmbreeSpeedup=cases['BVH2']['browserSeconds']/cases['EMBREE']['browserSeconds'],
                nativeEmbreeSpeedup=cases['BVH2']['nativeSeconds']/cases['EMBREE']['nativeSeconds'],
                adapter=web['adapter'],scene=web['scene'],
                tinyCubeProbe=dict(backends=tiny,method='Separate fresh hardware browser per backend, six original F12 renders at 128x128 and 16 samples with eight CPU threads. Timed with render_pre/render_post, without PNG output, video or profiling. Original C++ logs confirm each backend. First render and the median of five subsequent renders are reported. This does not establish the cause of differences from older release baselines.'),
                profiling=profile_summary(ROOT/'build/native/embree-profile',web['bundleVersion']),
                methodology='Three sequential render/export runs per backend at frame 2, after rendering frames 1 and 3 with each backend. Both BVH2 and Embree run within the same binary, selected through original Cycles debug flags and verified in C++ logs. Scene uses 64 linked icospheres, true round curves, sphere points, transparency and motion blur, 256x192 pixels, 16 samples and eight CPU threads. Persistent data, adaptive sampling and denoising are disabled. Timings include scene synchronization, BVH construction, tracing and PNG output. Browser timings exclude recording and profiling. Runs were sequential on a shared host, without controlled CPU affinity or cleared system caches. These small-scene timings do not establish large-scene performance or memory capacity.')
    (ROOT/'web/docs/embree-performance.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))

if __name__=='__main__':main()
