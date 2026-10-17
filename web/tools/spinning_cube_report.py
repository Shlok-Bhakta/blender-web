#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Produce a compact benchmark report and exportable frame-time plot."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def distribution(values):
    return dict(min=min(values), median=statistics.median(values), max=max(values),
                mean=statistics.mean(values))


def summarize(source):
    evidence = json.loads((source / 'acceptance.json').read_text())
    runs = evidence['runs']
    assert all(run['frames'] == 50 for run in runs), 'Baseline runs must contain all 50 frames'
    first = runs[0]
    for run in runs:
        assert run['bundleVersion'] == first['bundleVersion']
        assert run['settings'] == first['settings']
        assert run['settingsUnchanged'] and run['browserErrors'] == 0
        assert [row['frame'] for row in run['timings']] == list(range(1, 51))
    conditions_path = source / 'conditions.json'
    conditions = json.loads(conditions_path.read_text()) if conditions_path.exists() else {}
    output = dict(conditions=conditions, measuredAt=evidence['measuredAt'], cpu=evidence['cpu'],
                  logicalCpus=evidence['logicalCpus'], browserVersion=first['browserVersion'],
                  bundleVersion=first['bundleVersion'], adapter=first['environment']['adapter'],
                  framesPerRun=50, runCount=len(runs), method=evidence['method'], settings=first['settings'],
                  firstSavedFrameSeconds=distribution([r['firstSavedFrameSeconds'] for r in runs]),
                  firstReadbackSeconds=distribution([r['firstReadbackSeconds'] for r in runs]),
                  animationSeconds=distribution([r['renderAnimationSeconds'] for r in runs]),
                  overallFps=distribution([r['framesPerSecond'] for r in runs]),
                  steadyFps=distribution([49/(r['renderAnimationSeconds']-r['firstSavedFrameSeconds']) for r in runs]),
                  settingsUnchanged=True, browserErrors=0, runs=[])
    for run in runs:
        same = sum(a == b for a, b in zip(first['pixelHashes'], run['pixelHashes']))
        output['runs'].append({key: run[key] for key in [
            'run', 'frames', 'firstSavedFrameSeconds', 'firstReadbackSeconds',
            'renderAnimationSeconds', 'gpuJobSeconds', 'framesPerSecond',
            'steadyFrameSeconds', 'resources', 'uniqueFrameHashes', 'visibleDifferencePixels',
            'startupSeconds', 'requestToJobSeconds', 'pixelHashes', 'timings']})
        output['runs'][-1]['framesIdenticalToRun1'] = same
        resources_path = source / f"run-{run['run']}" / 'resources.json'
        if resources_path.exists():
            resource_evidence = json.loads(resources_path.read_text())
            samples = resource_evidence['samples']
            output['runs'][-1]['resources']['peakGpuSharedMemoryBytes'] = max(
                (s['gpuSharedMemoryBytes'] for s in samples if s['gpuSharedMemoryBytes'] is not None), default=None)
            cpu_peaks = []
            snapshots = resource_evidence['processes']
            for before, after in zip(snapshots, snapshots[1:]):
                elapsed = after['seconds'] - before['seconds']
                previous = {p['id']: p['cpuTime'] for p in before['processInfo']}
                used = sum(max(0, p['cpuTime'] - previous.get(p['id'], 0)) for p in after['processInfo'])
                if elapsed > 0:
                    cpu_peaks.append(used / elapsed)
            output['runs'][-1]['resources']['peakSampledCpuCores'] = max(cpu_peaks, default=None)
    return output


def plot(report, destination):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.size': 10, 'svg.fonttype': 'none'})
    fig, axes = plt.subplots(2, 1, figsize=(10, 6), layout='constrained')
    colors = ['#1b6ca8', '#d57900', '#5b7d24']
    for i, run in enumerate(report['runs']):
        times = [r['frameSeconds'] for r in run['timings']]
        for ax in axes:
            ax.plot(range(1, 51), times, label=f"Run {run['run']}", linewidth=1.4, color=colors[i % len(colors)])
    axes[0].set_title('Spinning cube: 50 native Eevee frames per fresh browser')
    axes[0].set_ylabel('Seconds per saved frame')
    axes[0].legend(loc='upper right', ncols=len(report['runs']), frameon=False)
    axes[1].set_title('Frames 2–50, including PNG encoding and file writing')
    axes[1].set_xlim(2, 50)
    steady = [r['frameSeconds'] for run in report['runs'] for r in run['timings'][1:]]
    axes[1].set_ylim(max(0, min(steady)*.95), max(steady)*1.05)
    axes[1].set_ylabel('Seconds per saved frame')
    axes[1].set_xlabel('Animation frame')
    for ax in axes:
        ax.grid(axis='y', alpha=.25)
        ax.spines[['top', 'right']].set_visible(False)
        ax.set_xticks([1, 10, 20, 30, 40, 50] if ax is axes[0] else [2, 10, 20, 30, 40, 50])
    fig.savefig(destination, metadata={'Date': None})
    plt.close(fig)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--json', type=Path, required=True)
    parser.add_argument('--plot', type=Path)
    parser.add_argument('--html', type=Path)
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    result = summarize(args.source)
    args.json.write_text(json.dumps(result, indent=2)+'\n')
    if args.csv:
        with args.csv.open('w', newline='') as file:
            fields = ['run', 'frame', 'elapsedSeconds', 'frameSeconds', 'renderAndWriteSeconds', 'keyframedRotationZ', 'bytes', 'pixelSha256']
            writer = csv.DictWriter(file, fieldnames=fields)
            writer.writeheader()
            for run in result['runs']:
                for row, digest in zip(run['timings'], run['pixelHashes']):
                    writer.writerow({**{key: row[key] for key in fields if key in row}, 'run': run['run'], 'pixelSha256': digest})
    if args.plot:
        plot(result, args.plot)
    if args.html:
        template = Path(__file__).with_suffix('.html').read_text()
        encoded = json.dumps(result).replace('<', chr(92) + 'u003c')
        args.html.write_text(template.replace('__REPORT_JSON__', encoded))
    print(json.dumps({key: result[key] for key in ['runCount', 'firstSavedFrameSeconds', 'animationSeconds', 'overallFps']}))
