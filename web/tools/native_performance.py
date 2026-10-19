#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the shared benchmark with the installed desktop Blender and sample its memory."""
import argparse
import json
from pathlib import Path
import subprocess
from prepare import ROOT
from process_memory import sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender', default='blender')
    parser.add_argument('--cycles', action='store_true')
    parser.add_argument('--eevee', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'build/native/performance-native'
    output.mkdir(parents=True, exist_ok=True)
    if args.cycles or args.eevee:
        engine = 'eevee' if args.eevee else 'cycles'
        command = [args.blender, '--background', '--factory-startup', '--threads', '8',
            '--python-exit-code', '1', '--python-expr',
            f"import runpy,bpy; runpy.run_path('web/tests/{engine}_timing.py')['native_{engine}_timing'](); bpy.data.images['Render Result'].save_render({str(output / f'{engine}.png')!r},scene=bpy.context.scene)"]
        result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (output / f'{engine}.log').write_text(result.stdout)
        result.check_returncode()
        report = json.loads(next(line.split(':', 1)[1] for line in result.stdout.splitlines()
                                 if line.startswith(engine.upper()+'_NATIVE_COMPLETE:')))
        (output / f'{engine}.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report))
        return
    command = [args.blender, '--background', '--factory-startup', '--threads', '8',
               '--python-exit-code', '1', '--python', str(ROOT / 'web/tests/performance.py')]
    blender = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sampler = subprocess.Popen(['python3', str(ROOT / 'web/tools/process_memory.py'),
        '--root', str(blender.pid), '--output', str(output / 'memory.jsonl')])
    stages, report, failed = [], None, None
    try:
        with (output / 'browser.log').open('w') as log:
            for line in blender.stdout:
                log.write(line)
                if line.startswith('PERF_MEMORY:'):
                    stages.append({**json.loads(line.split(':', 1)[1]), **sample(blender.pid)})
                if line.startswith('PERF_COMPLETE:'):
                    report = json.loads(line.split(':', 1)[1])
                if line.startswith('PERF_FAILED:'):
                    failed = line
        assert blender.wait() == 0 and report and not failed, failed or 'Native benchmark failed'
    finally:
        sampler.terminate()
        sampler.wait()
        if blender.poll() is None:
            blender.terminate()
            blender.wait()
    points = [json.loads(line) for line in (output / 'memory.jsonl').read_text().splitlines()]
    report.update(command=command, memoryStages=stages,
                  peakPssMiB=max(point['pssKiB'] for point in points)/1024,
                  memoryMetric='Linux proportional set size; process tree; 200ms sampling')
    (output / 'acceptance.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
