#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the complete browser build, desktop exchange and local benchmarks."""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import time
from prepare import ROOT

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender', default='blender', help='Pinned native Blender executable')
    parser.add_argument('--candidate-url', default='http://127.0.0.1:8093/')
    parser.add_argument('--control-url', default='http://127.0.0.1:8098/')
    parser.add_argument('--list', action='store_true', help='Print the checks without running them')
    args = parser.parse_args()
    if not args.list and (not shutil.which(args.blender)):
        parser.error('Native Blender is required; provide --blender /path/to/blender')
    root = ROOT
    base = root / 'build/native'
    env = dict(os.environ, WEB_TEST_URL=args.candidate_url)
    for flag in ('WEB_TEST_HARDWARE', 'WEB_TEST_UI_FALLBACK', 'WEB_RENDER_SOFTWARE'):
        env.pop(flag, None)
    status = []
    if not args.list:
        link = (base / 'editor/source/creator/CMakeFiles/blender.dir/link.txt').read_text()
        assert '-O3' in link and ' -O0' not in link, 'Release optimization is required'
        assert json.loads((base / 'dist/manifest.json').read_text())['uiFallback'], 'Final package must include both variants'

    def run(name, command, **extra):
        if args.list:
            print(name + ': ' + shlex.join(command))
            return
        print(name, flush=True)
        start = time.time()
        with (base / f'validate-{name}.log').open('w') as log:
            result = subprocess.run(command, cwd=root, env=dict(env, **extra), stdout=log, stderr=subprocess.STDOUT)
        status.append(dict(name=name, seconds=time.time() - start, exitCode=result.returncode))
        (base / 'validation-status.json').write_text(json.dumps(status, indent=2) + '\n')
        if result.returncode:
            raise SystemExit(f'{name} failed: see validate-{name}.log')
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'text-{suffix}', ['node', 'web/tests/text.cjs'], **flags)
    run('text-video', ['python3', 'web/tools/native_text_recording.py'])
    run('audio-worklet', ['node', 'web/tests/audio-worklet.cjs'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'media-{suffix}', ['node', 'web/tests/media.cjs'], **flags)
        directory = 'build/native/media-evidence' + ('-hardware' if suffix == 'hardware' else '-ui' if suffix == 'ui' else '')
        run(f'media-native-{suffix}', ['python3', 'web/tools/native_media_verify.py', directory])
    run('media-video', ['python3', 'web/tools/native_media_recording.py'])
    run('media-benchmark', ['node', 'web/tests/media.cjs'], WEB_TEST_HARDWARE='1',
        WEB_MEDIA_BENCHMARK='1', WEB_MEDIA_OUTPUT='build/native/media-benchmark')
    run('media-performance', ['python3', 'web/tools/native_media_performance.py'])
    run('media-profile', ['node', 'web/tests/media.cjs'], WEB_TEST_HARDWARE='1',
        WEB_MEDIA_PROFILE='1', WEB_MEDIA_OUTPUT='build/native/media-profile')
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'usd-{suffix}', ['node', 'web/tests/usd-exchange.cjs'], **flags)
    run('desktop-usd', [args.blender, '--background', '--factory-startup', '--python-exit-code', '1', '--python', 'web/tests/usd_exchange.py', '--', 'native-crosscheck', 'build/native/usd-evidence-hardware'])
    run('usd-video', ['python3', 'web/tools/native_usd_recording.py'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'}), ('hardware-ui', {'WEB_TEST_HARDWARE': '1', 'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'image-vector-{suffix}', ['node', 'web/tests/image-vector.cjs'], **flags)
    run('freestyle-hardware', ['node', 'web/tests/freestyle.cjs'])
    run('freestyle-software', ['node', 'web/tests/freestyle.cjs'], WEB_RENDER_SOFTWARE='1', WEB_FREESTYLE_OUTPUT='build/native/freestyle-evidence-software')
    run('desktop-freestyle', [args.blender, '--background', '--factory-startup', '--python-exit-code', '1', '--python', 'web/tests/verify_freestyle_desktop.py', '--', 'build/native/freestyle-evidence'])
    run('desktop-images', [args.blender, '--background', '--factory-startup', '--python-exit-code', '1', '--python', 'web/tests/verify_image_vector_desktop.py', '--', 'build/native/image-vector-evidence-hardware'])
    run('dependency-status', ['python3', 'web/tools/native_dependency_status.py', '--blender', args.blender])
    run('dependency-video', ['python3', 'web/tools/native_dependency_recording.py'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'volume-{suffix}', ['node', 'web/tests/volume.cjs'], **flags)
    for script in ('native', 'features', 'ocean-alembic'):
        for suffix, flags in [('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'}), ('hardware', {'WEB_TEST_HARDWARE': '1'})]:
            run(f'{script}-{suffix}', ['node', f'web/tests/{script}.cjs'], **flags)
    for suffix, flags in [('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'loading-{suffix}', ['node', 'web/tests/loading.cjs'], **flags)
    run('cache', ['node', 'web/tests/cache.cjs'])
    run('eevee-resize', ['node', 'web/tests/render.cjs'], WEB_RENDER_REFERENCE='web/docs/eevee-default-scene-render.png', WEB_RENDER_DEVICE_SCALE='2', WEB_RENDER_RESIZE_DURING_RENDER='1')
    run('desktop-ocean', [args.blender, '--background', '--factory-startup', '--python-exit-code', '1', '--python', 'web/tests/verify_ocean_desktop.py', '--', 'build/native/ocean-evidence-hardware'])
    run('volume-video', ['python3', 'web/tools/native_ocean_recording.py', '--kind', 'volume'])
    run('render-optimization', ['python3', 'web/tools/native_render_optimization.py', '--candidate-url', args.candidate_url, '--control-url', args.control_url])
    for name, workload_args in [('mesh', []), ('cycles', ['--cycles']), ('eevee', ['--eevee'])]:
        run(f'native-performance-{name}', ['python3', 'web/tools/native_performance.py', '--blender', args.blender, *workload_args])
    run('browser-performance-mesh', ['node', 'web/tests/performance.cjs'])
    run('browser-performance-cycles', ['node', 'web/tests/cycles.cjs'], WEB_CYCLES_BENCHMARK='1', WEB_CYCLES_REPEATS='3')
    run('eevee-profile', ['node', 'web/tests/render.cjs'], WEB_RENDER_PROFILE='1', WEB_RENDER_GPU_TIMESTAMPS='1', WEB_RENDER_TIMING='1', WEB_RENDER_REPEATS='2', WEB_RENDER_BENCHMARK='1', WEB_RENDER_OUTPUT='build/native/performance-eevee-profile', WEB_RENDER_REFERENCE='web/docs/eevee-default-scene-render.png')
    run('profile-summary', ['python3', 'web/tools/summarize_render_profile.py', 'build/native/performance-eevee-profile'])
    run('compare-performance', ['python3', 'web/tools/compare_performance.py'])
    run('acceptance', ['python3', 'web/tools/native_acceptance.py'])
    if not args.list:
        print('All validation complete', flush=True)
if __name__ == '__main__':
    main()
