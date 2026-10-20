#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare packaged builds in fresh browser sessions without recording overhead."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import urllib.request
from prepare import ROOT


def manifest(url):
    with urllib.request.urlopen(url.rstrip('/') + '/manifest.json', timeout=30) as response:
        return json.load(response)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control-url', default='http://127.0.0.1:8098/')
    parser.add_argument('--candidate-url', default='http://127.0.0.1:8093/')
    args = parser.parse_args()
    manifests = {name:manifest(url) for name,url in
                 (('control',args.control_url),('candidate',args.candidate_url))}
    assert manifests['control']['blenderRevision'] == manifests['candidate']['blenderRevision']
    assert manifests['candidate']['shaderSeed']['entries'] > 50
    base = ROOT / 'build/native/render-optimization'
    base.mkdir(parents=True, exist_ok=True)
    results = {'control':[], 'candidate':[]}
    for session in range(3):
        for name,url in (('control',args.control_url),('candidate',args.candidate_url)):
            output = (ROOT / 'build/native/performance-eevee' if name == 'candidate' and session == 0
                      else base / f'{name}-{session}')
            output.mkdir(parents=True, exist_ok=True)
            env = dict(os.environ)
            for key in ('WEB_RENDER_PROFILE', 'WEB_RENDER_GPU_TIMESTAMPS',
                        'WEB_RENDER_SHADER_OVERRIDES', 'WEB_RENDER_DUMP_GLSL',
                        'WEB_RENDER_EXPORT_SHADER_CACHE', 'WEB_RENDER_RESIZE_DURING_RENDER',
                        'WEB_RENDER_SOFTWARE'):
                env.pop(key, None)
            env.update(WEB_TEST_URL=url, WEB_RENDER_OUTPUT=str(output), WEB_RENDER_TIMING='1',
                       WEB_RENDER_REPEATS='3', WEB_RENDER_BENCHMARK='1', WEB_RENDER_DEVICE_SCALE='1',
                       WEB_RENDER_REFERENCE=str(ROOT / 'web/docs/eevee-default-scene-render.png'))
            print(f'{name}: fresh session {session+1}/3', flush=True)
            with (output / 'command.log').open('w') as log:
                subprocess.run(['node', str(ROOT / 'web/tests/render.cjs')], cwd=ROOT, env=env,
                               stdout=log, stderr=subprocess.STDOUT, check=True)
            report = json.loads((output / 'acceptance.json').read_text())
            assert report['bundleVersion'] == manifests[name]['version']
            assert not report['videoCaptured'] and not report['shaderOverrideEntries']
            assert report['referencePixelsEqual'] and report['browserErrors'] == 0
            assert len(report['handlerSeconds']) == 3
            assert not any(term in json.dumps(report['adapter']).lower()
                           for term in ('swiftshader','llvmpipe','lavapipe'))
            results[name].append(report)
            (base / 'sessions.json').write_text(json.dumps(results, indent=2) + '\n')
    defaults = results['control'][0]['defaults']
    assert all(run['defaults'] == defaults for runs in results.values() for run in runs)
    assert len({run['browserVersion'] for runs in results.values() for run in runs}) == 1
    summary = {}
    for name,runs in results.items():
        cold = [run['handlerSeconds'][0] for run in runs]
        warm = [value for run in runs for value in run['handlerSeconds'][1:]]
        summary[name] = dict(bundleVersion=manifests[name]['version'], coldSeconds=cold,
                             warmSeconds=warm, coldMedian=statistics.median(cold),
                             warmMedian=statistics.median(warm), shaderSeed=manifests[name].get('shaderSeed'))
    improvement = {mode:100*(1-summary['candidate'][mode+'Median']/summary['control'][mode+'Median'])
                   for mode in ('cold','warm')}
    report = dict(bundleVersion=manifests['candidate']['version'], sessions=results,
                  summary=summary, reductionPercent=improvement,
                  workloadSHA256=hashlib.sha256((ROOT / 'web/tests/eevee_timing.py').read_bytes()).hexdigest(),
                  method='Three fresh Chromium sessions per build, alternated control/candidate. Three renders per session, no video/tracing, original factory settings. First-in-process engine handler time; global GPU driver caches are not cleared. This compares complete packaged builds: enabled dependencies, debug-name metadata and compiler configuration may differ, so it does not isolate a single cache or optimizer change.')
    (ROOT / 'web/docs/render-optimization.json').write_text(json.dumps(report, indent=2) + '\n')
    lines = ['# Render optimization measurements', '', report['method'], '',
             '| Build | First render, median | Warm render, median |',
             '| --- | ---: | ---: |']
    lines += [f'| {name} | {row["coldMedian"]:.3f} s | {row["warmMedian"]:.3f} s |'
              for name,row in summary.items()]
    lines += ['', f'First-render time reduction: {improvement["cold"]:.1f}%. '
              f'Warm-render median change: {-improvement["warm"]:+.1f}%, where positive means slower.', '',
              'Every run retained the original scene settings and matched the prior browser reference pixels exactly.', '',
              'This is a comparison of complete packaged builds, including their dependency and compiler configuration differences. '
              'It does not isolate a single optimization. The candidate bundles common original shader translations in its preload data. '
              'Compiler/source fingerprints guard the seed; unseen variants still compile and cache at runtime.', '',
              'Queue batching and shaderc performance optimization experiments were rejected. '
              'See [experiment evidence](render-optimization-experiments.json) and '
              '[full measurements](render-optimization.json).', '']
    (ROOT / 'web/docs/render-optimization.md').write_text('\n'.join(lines))
    print(json.dumps({'summary':summary,'reductionPercent':improvement}, indent=2))


if __name__ == '__main__':
    main()
