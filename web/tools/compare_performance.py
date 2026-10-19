#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize matched native/browser CPU timings and Linux process-tree memory."""
import datetime
import hashlib
import json
import platform
import statistics
import subprocess
from pathlib import Path
from prepare import ROOT


def stats(values):
    assert len(values) == 3 and all(value > 0 for value in values)
    return dict(seconds=values, medianSeconds=statistics.median(values),
                firstSeconds=values[0], warmMedianSeconds=statistics.median(values[1:]),
                minSeconds=min(values), maxSeconds=max(values))


def main():
    base = ROOT / 'build/native'
    read = lambda path: json.loads(path.read_text())
    native = read(base / 'performance-native/acceptance.json')
    browser = read(base / 'performance-browser/acceptance.json')
    cycles_native = read(base / 'performance-native/cycles.json')
    cycles_browser = read(base / 'cycles-evidence/acceptance.json')
    eevee_native = read(base / 'performance-native/eevee.json')
    eevee_browser = read(base / 'performance-eevee/acceptance.json')
    manifest = read(base / 'dist/manifest.json')
    profile = read(base / 'performance-eevee-profile/profile-summary.json')
    assert profile['bundleVersion'] == manifest['version'] and profile['browserErrors'] == 0
    assert browser['bundleVersion'] == cycles_browser['bundleVersion'] == eevee_browser['bundleVersion'] == manifest['version']
    assert native['blenderVersion'] == browser['blenderVersion']
    assert native['numpyVersion'] == browser['numpyVersion']
    assert not cycles_browser['videoCaptured'], 'Disable recording for comparison timings'
    assert not eevee_browser['videoCaptured']
    assert not eevee_browser.get('shaderOverrideEntries'), 'Benchmark must use the packaged runtime shaders'
    assert native['rounds'] == browser['rounds'] == 3
    assert native['cpuCount'] == browser['cpuCount'] == 8
    comparisons = []
    for n in (128, 256, 384):
        nr = [r for r in native['grids'] if r['grid'] == n]
        br = [r for r in browser['grids'] if r['grid'] == n]
        assert all(a['evaluatedFaces'] == b['evaluatedFaces'] and a['evaluatedVertices'] == b['evaluatedVertices'] for a,b in zip(nr,br))
        for task, key in [('Mesh construction','createSeconds'),('CPU subdivision','subdivisionSeconds')]:
            a, b = stats([r[key] for r in nr]), stats([r[key] for r in br])
            comparisons.append(dict(task=task, grid=n, faces=nr[0]['evaluatedFaces'],
                vertices=nr[0]['evaluatedVertices'], native=a, browser=b,
                browserOverNative=b['medianSeconds']/a['medianSeconds']))
    for task, a, b in [
        ('Ocean 12-frame evaluation',stats([r['seconds'] for r in native['oceans']]),stats([r['seconds'] for r in browser['oceans']])),
        ('Cycles CPU render',stats(cycles_native['handlerSeconds']),stats(cycles_browser['handlerSeconds'])),
        ('Eevee GPU render',stats(eevee_native['handlerSeconds']),stats(eevee_browser['handlerSeconds'])),
    ]:
        comparisons.append(dict(task=task,native=a,browser=b,browserOverNative=b['medianSeconds']/a['medianSeconds']))
    pixel_comparison = json.loads(subprocess.check_output(['node','-e', '''
const fs=require('fs'),{PNG}=require('./web/tests/node_modules/pngjs');
const a=PNG.sync.read(fs.readFileSync('build/native/performance-native/eevee.png'));
const b=PNG.sync.read(fs.readFileSync('build/native/performance-eevee/eevee-default.png'));
if(a.width!==b.width||a.height!==b.height)throw Error('Image dimensions differ');
let sum=0,outliers=0,count=0;
for(let i=0;i<a.data.length;i++){if(i%4===3)continue;const d=Math.abs(a.data[i]-b.data[i]);sum+=d;if(d>20)outliers++;count++;}
console.log(JSON.stringify({width:a.width,height:a.height,meanAbsoluteChannelError:sum/count,outlierFraction:outliers/count}));
'''], cwd=ROOT, text=True))
    def memory(report, directory):
        stages = {point['stage']:point['pssKiB']/1024 for point in report['memoryStages']}
        timestamps = {point['stage']:point['monotonic'] for point in report['memoryStages']}
        points = [json.loads(line) for line in (directory/'memory.jsonl').read_text().splitlines()]
        peak = max([point['pssKiB']/1024 for point in points if timestamps['baseline']<=point['monotonic']<=timestamps['after-cleanup']] + list(stages.values()))
        return dict(peakPssMiB=peak,lifecyclePeakPssMiB=report['peakPssMiB'],baselinePssMiB=stages['baseline'],
                    peakAboveBaselineMiB=peak-stages['baseline'],
                    afterCleanupPssMiB=stages['after-cleanup'], stagesPssMiB=stages)
    measured = max(path.stat().st_mtime for path in [base/'performance-native/acceptance.json',
        base/'performance-browser/acceptance.json',base/'performance-native/cycles.json',
        base/'cycles-evidence/acceptance.json',base/'performance-native/eevee.json',
        base/'performance-eevee/acceptance.json'])
    report = dict(bundleVersion=manifest['version'],blenderVersion=native['blenderVersion'],
        measuredAt=datetime.datetime.fromtimestamp(measured,datetime.timezone.utc).isoformat(),
        generatedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        host=dict(cpu=next(line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
                  logicalCPUs=native['cpuCount'],system=platform.platform(),
                  memTotalKiB=int(Path('/proc/meminfo').read_text().splitlines()[0].split()[1]),
                  gpu=browser['adapter']),
        comparisons=comparisons,memory=dict(native=memory(native,base/'performance-native'),browser=memory(browser,base/'performance-browser'),
            wasmHeapCapacityBytes=browser['wasmHeapCapacityBytes'],wasmMaximumBytes=browser['wasmMaximumBytes']),
        eevee=dict(nativeAdapter=eevee_native['adapter'],browserAdapter=eevee_browser['adapter'],pixelComparison=pixel_comparison),
        profiling=dict(measuredSeparately=True,timingsExcludedFromBaseline=True,
            runs=[dict(hotspotsCpuMs=run['hotspotsCpuMs'],sampledRenderCpuMs=run['sampledRenderCpuMs'],
                       gpuBusyPercent=run.get('gpuBusyPercent'),
                       queueSubmits=run['apiCpuCalls']['submit']['count']) for run in profile['runs']]),
        method=dict(repeats=3,renderThreads=8,subdivisionLevel=2,subdivisionEvaluator='CPU',
                    cyclesSize=[128,128],cyclesSamples=16,cyclesDenoising=False,cyclesAdaptiveSampling=False,
                    eeveeSize=[1920,1080],eeveeSamples=64,
                    memory='Linux proportional set size, entire Chromium tree versus native Blender tree; 200ms sampling',
                    timers='Python perf_counter for both builds; render_pre to render_post for both renderers',
                    videoDuringTimings=False,
                    limitations=['Native uses background mode; browser includes its UI, GPU process and JavaScript workers. Baseline and incremental memory are reported separately.',
                        'Mesh timings cover CPU construction and evaluation, not interactive viewport frame rate.',
                        'Each case runs three times in one process, in fixed order. First and warm timings are included; these are small local baselines, not a broad production-scene benchmark.',
                        'Both use the same pinned Blender release and scene settings. Native CPU ISA/accelerators and the Wasm build differ.',
                        'First-run means the first render in that process. Global GPU driver caches are not cleared. Native Eevee and browser WebGPU use different graphics APIs on the same hardware.',
                        'Physical memory sampling has overhead and may miss sub-200ms peaks. Wasm capacity is not physical usage, and its linear memory cannot shrink after growth.',
                        'PSS does not fully account for driver-managed GPU memory; the scene-size test measures CPU geometry memory, not total VRAM.',
                        'The 4 GiB Wasm32 limit is configured for this build. Larger desktop-scale allocations have not been ported or validated.']),
        workloadSHA256={name:hashlib.sha256((ROOT/'web/tests'/name).read_bytes()).hexdigest()
                        for name in ('performance.py','cycles_timing.py','eevee_timing.py')})
    (ROOT / 'web/docs/native-web-performance.json').write_text(json.dumps(report,indent=2)+'\n')
    (ROOT / 'web/docs/native-web-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    document = ROOT / 'web/docs/native-web-performance.md'
    before, tail = document.read_text().split('<!-- PERFORMANCE_RESULTS -->', 1)
    _, after = tail.split('<!-- /PERFORMANCE_RESULTS -->', 1)
    table = '\n| Workload | Native median | Browser median | Browser/native |\n| --- | ---: | ---: | ---: |\n'
    for row in comparisons:
        label = row['task'] + (f", {row['grid']} by {row['grid']} grid" if 'grid' in row else '')
        table += f"| {label} | {row['native']['medianSeconds']:.3f} s | {row['browser']['medianSeconds']:.3f} s | {row['browserOverNative']:.2f}x |\n"
    table += '\n| Process-tree memory | Native | Browser |\n| --- | ---: | ---: |\n'
    for key, label in [('baselinePssMiB','Baseline'),('peakPssMiB','Workload peak'),('peakAboveBaselineMiB','Workload peak above baseline'),('afterCleanupPssMiB','After cleanup'),('lifecyclePeakPssMiB','Whole-process peak, including startup')]:
        table += f"| {label} | {report['memory']['native'][key]:.1f} MiB | {report['memory']['browser'][key]:.1f} MiB |\n"
    table += f"\nTested bundle `{manifest['version']}`, measured {report['measuredAt']}.\n"
    document.write_text(before+'<!-- PERFORMANCE_RESULTS -->'+table+'<!-- /PERFORMANCE_RESULTS -->'+after)
    print(json.dumps(report))


if __name__ == '__main__':
    main()
