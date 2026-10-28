#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the complete browser build, desktop exchange and local benchmarks."""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from prepare import ROOT

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender', default='blender', help='Pinned native Blender executable')
    parser.add_argument('--candidate-url', default='http://127.0.0.1:8093/')
    parser.add_argument('--control-url', default='http://127.0.0.1:8098/')
    parser.add_argument('--list', action='store_true', help='Print the checks without running them')
    parser.add_argument('--resume-from', help='Rerun a recorded stage and continue with the same packaged candidate')
    parser.add_argument('--stop-before', help='Stop before a named stage, leaving validation incomplete for later resumption')
    args = parser.parse_args()
    if args.stop_before and not args.list:
        available = [line.split(':', 1)[0] for line in subprocess.check_output(
            [sys.executable, __file__, '--list'], text=True).splitlines()]
        if args.stop_before not in available:
            parser.error(f'Unknown stop stage: {args.stop_before}')
        if args.resume_from and (args.resume_from not in available or
                available.index(args.stop_before) <= available.index(args.resume_from)):
            parser.error('--stop-before must come after --resume-from')
    if not args.list and (not shutil.which(args.blender)):
        parser.error('Native Blender is required; provide --blender /path/to/blender')
    root = ROOT
    base = root / 'build/native'
    env = dict(os.environ, WEB_TEST_URL=args.candidate_url)
    for flag in ('WEB_TEST_HARDWARE', 'WEB_TEST_UI_FALLBACK', 'WEB_RENDER_SOFTWARE'):
        env.pop(flag, None)
    status = []
    previous = []
    resume_from = args.resume_from
    if not args.list:
        link = (base / 'editor/source/creator/CMakeFiles/blender.dir/link.txt').read_text()
        assert '-O3' in link and ' -O0' not in link, 'Release optimization is required'
        manifest = json.loads((base / 'dist/manifest.json').read_text())
        assert manifest['uiFallback'], 'Final package must include both variants'
        checkpoint = base / 'validation-candidate.json'
        if resume_from:
            assert json.loads(checkpoint.read_text()) == manifest, 'Resume requires the identical packaged candidate'
            previous = json.loads((base / 'validation-status.json').read_text())
            assert resume_from in {entry['name'] for entry in previous}, 'Resume must start at a previously recorded stage'
        else:
            # Invalidate old results before binding the checkpoint to a new
            # candidate, even when stopped before the first stage.
            (base / 'validation-status.json').write_text('[]\n')
            checkpoint.write_text(json.dumps(manifest, indent=2) + '\n')

    def run(name, command, **extra):
        nonlocal resume_from
        if args.list:
            print(name + ': ' + shlex.join(command))
            return
        if args.stop_before == name:
            print(f'Stopped before {name}; {len(status)} stages complete. Validation is incomplete.', flush=True)
            raise SystemExit(0)
        if resume_from and name != resume_from:
            entry = previous[len(status)]
            assert entry['name'] == name and entry['exitCode'] == 0, 'Only a successful unchanged prefix may be reused'
            status.append(entry)
            return
        resume_from = None
        print(name, flush=True)
        start = time.time()
        with (base / f'validate-{name}.log').open('w') as log:
            result = subprocess.run(command, cwd=root, env=dict(env, **extra), stdout=log, stderr=subprocess.STDOUT)
        status.append(dict(name=name, seconds=time.time() - start, exitCode=result.returncode))
        (base / 'validation-status.json').write_text(json.dumps(status, indent=2) + '\n')
        if result.returncode:
            raise SystemExit(f'{name} failed: see validate-{name}.log')
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'python-foundation-{suffix}', ['node', 'web/tests/python-foundation.cjs'], **flags)
        directory = 'build/native/python-foundation-evidence' + ('-hardware' if suffix == 'hardware' else '-ui' if suffix == 'ui' else '')
        run(f'python-foundation-native-{suffix}', ['python3', 'web/tools/native_python_verify.py', directory, '--blender', args.blender])
    run('python-foundation-video', ['python3', 'web/tools/native_python_recording.py'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'python-bindings-{suffix}', ['node', 'web/tests/python-bindings.cjs'], **flags)
        directory = 'build/native/python-bindings-evidence' + ('-hardware' if suffix == 'hardware' else '-ui' if suffix == 'ui' else '')
        run(f'python-bindings-native-{suffix}', ['python3', 'web/tools/native_bindings_verify.py', directory, '--blender', args.blender])
    run('python-bindings-video', ['python3', 'web/tools/native_bindings_recording.py'])
    run('hevc-native', ['python3', 'web/tools/native_hevc_verify.py'])
    run('render-libraries-baseline', ['python3', 'web/tools/native_render_libraries_baseline.py', '--blender', args.blender])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'render-libraries-{suffix}', ['node', 'web/tests/render-libraries.cjs'], **flags)
        directory='build/native/render-libraries-evidence'+('-hardware' if suffix=='hardware' else '-ui' if suffix=='ui' else '')
        run(f'render-libraries-native-{suffix}', ['python3', 'web/tools/native_render_libraries_verify.py', directory, '--blender', args.blender])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'embree-{suffix}', ['node', 'web/tests/embree.cjs'], **flags)
        directory='build/native/embree-evidence'+('-hardware' if suffix=='hardware' else '-ui' if suffix=='ui' else '')
        run(f'embree-native-{suffix}', ['python3', 'web/tools/native_embree_verify.py', directory, '--blender', args.blender])
    run('render-libraries-video', ['python3', 'web/tools/native_render_libraries_recording.py'])
    run('embree-video', ['python3', 'web/tools/native_embree_recording.py'])
    run('embree-benchmark', ['node', 'web/tests/embree.cjs'], WEB_TEST_HARDWARE='1',
        WEB_EMBREE_BENCHMARK='1', WEB_EMBREE_OUTPUT='build/native/embree-benchmark')
    run('embree-profile', ['node', 'web/tests/embree.cjs'], WEB_TEST_HARDWARE='1',
        WEB_EMBREE_PROFILE='1', WEB_EMBREE_OUTPUT='build/native/embree-profile')
    for backend in ('BVH2', 'EMBREE'):
        run('embree-cube-'+backend.lower(), ['node', 'web/tests/cycles.cjs'],
            WEB_CYCLES_BENCHMARK='1', WEB_CYCLES_REPEATS='6', WEB_CYCLES_BVH=backend,
            WEB_CYCLES_OUTPUT='build/native/embree-cube-'+backend.lower())
    run('embree-performance', ['python3', 'web/tools/native_embree_performance.py'])
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
        run(f'gltf-{suffix}', ['node', 'web/tests/gltf.cjs'], **flags)
        directory = 'build/native/gltf-evidence' + ('-hardware' if suffix == 'hardware' else '-ui' if suffix == 'ui' else '')
        run(f'gltf-native-{suffix}', ['python3', 'web/tools/native_gltf_verify.py', directory, '--blender', args.blender])
    run('gltf-video', ['python3', 'web/tools/native_gltf_recording.py'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'tracking-{suffix}', ['node', 'web/tests/tracking.cjs'], **flags)
        directory = 'build/native/tracking-evidence' + ('-hardware' if suffix == 'hardware' else '-ui' if suffix == 'ui' else '')
        run(f'tracking-native-{suffix}', ['python3', 'web/tools/native_tracking_verify.py', directory, '--blender', args.blender])
    run('tracking-video', ['python3', 'web/tools/native_tracking_recording.py'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'text-{suffix}', ['node', 'web/tests/text.cjs'], **flags)
    run('text-video', ['python3', 'web/tools/native_text_recording.py'])
    for suffix, flags in [('hardware', {'WEB_TEST_HARDWARE': '1'}), ('render', {}), ('ui', {'WEB_TEST_UI_FALLBACK': '1'})]:
        run(f'usd-{suffix}', ['node', 'web/tests/usd-exchange.cjs'], **flags)
    run('desktop-usd', [args.blender, '--background', '--factory-startup', '--python-exit-code', '1', '--python', 'web/tests/usd_exchange.py', '--', 'native-crosscheck', 'build/native/usd-evidence-hardware'])
    run('usd-imaging-native', ['python3', 'web/tools/native_usd_imaging_verify.py', 'build/native/usd-imaging-check', '--blender', args.blender])
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
