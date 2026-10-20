#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publish acceptance evidence only when every browser test used the same bundle."""
import gzip
import hashlib
import json
import tarfile
from pathlib import Path
from prepare import ROOT


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def main():
    base = ROOT / 'build/native'
    docs = ROOT / 'web/docs'
    manifest = read(base / 'dist/manifest.json')
    version = manifest['version']
    features = [read(base / name / 'acceptance.json') for name in
                ['feature-evidence', 'feature-evidence-ui', 'feature-evidence-hardware']]
    ui = [read(base / name / 'acceptance.json') for name in
          ['evidence', 'evidence-ui', 'evidence-hardware']]
    eevee = read(base / 'render-evidence/acceptance.json')
    cycles = read(base / 'cycles-evidence/acceptance.json')
    recording = read(docs / 'volume-recording.json')
    volumes = [read(base / name / 'acceptance.json') for name in
               ['volume-evidence', 'volume-evidence-ui', 'volume-evidence-hardware']]
    image_vectors = [read(base / name / 'acceptance.json') for name in
                    ['image-vector-evidence', 'image-vector-evidence-ui', 'image-vector-evidence-hardware', 'image-vector-evidence-hardware-ui']]
    freestyles = [read(base / name / 'acceptance.json') for name in
                  ['freestyle-evidence', 'freestyle-evidence-software']]
    dependency_recording = read(docs / 'image-vector-recording.json')
    dependency_status = read(docs / 'dependency-status.json')
    usd = [read(base / name / 'acceptance.json') for name in
           ['usd-evidence', 'usd-evidence-ui', 'usd-evidence-hardware']]
    usd_native = read(base / 'usd-evidence-hardware/native-crosscheck.json')
    usd_recording = read(docs / 'usd-recording.json')
    for variant in usd:
        assert variant['bundleVersion'] == version and not variant['browserErrors']
        assert variant['offlineReopen'] and variant['buildOptions']['usd']
        assert len(variant['staticPythonModules']) >= 32
        assert len(variant['primitives']['objects']) == 8
        assert variant['primitives']['maximumDesktopCoordinateError'] < 2e-4
        assert variant['hookEvents'].count('export') == 3 and variant['hookEvents'].count('import') == 6
        assert variant['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/usd_exchange.py').read_bytes()).hexdigest()
        for name, expected in variant['fixtureHashes'].items():
            assert hashlib.sha256((ROOT / 'web/tests/fixtures/usd-exchange' / name).read_bytes()).hexdigest() == expected
        for extension, case in variant['formats'].items():
            assert case['desktopImportMaximumError'] < 2e-4 and case['roundtripMaximumError'] < 2e-4
            assert case['maximumMaterialXInputError'] < 2e-4
            assert case['stage']['assets'] and case['stage']['hookMetadata'] == 'original-python-hook'
            assert usd_native[extension]['maximumBrowserSceneError'] < 2e-4
            assert usd_native[extension]['maximumBrowserMaterialXInputError'] < 2e-4
    assert usd_recording['bundleVersion'] == version
    assert hashlib.sha256((ROOT / usd_recording['path']).read_bytes()).hexdigest() == usd_recording['sha256']
    desktop_images = read(base / 'image-vector-evidence-hardware/desktop-verification.json')
    desktop_freestyle = read(base / 'freestyle-evidence/desktop-verification.json')
    assert desktop_freestyle['blendSha256'] == freestyles[0]['blendSha256']
    assert desktop_freestyle['freestyle'] and desktop_freestyle['strokeColor'] == [1, 0, 0]
    assert dependency_status['bundleVersion'] == version
    assert dependency_status['browserBuildOptions'] == image_vectors[2]['buildOptions']
    oceans = [read(base / name / 'acceptance.json') for name in
              ['ocean-evidence', 'ocean-evidence-ui', 'ocean-evidence-hardware']]
    desktop_ocean = read(base / 'ocean-evidence-hardware/desktop-verification.json')
    performance = read(docs / 'native-web-performance.json')
    optimization = read(docs / 'render-optimization.json')
    assert optimization['bundleVersion'] == version
    assert optimization['workloadSHA256'] == hashlib.sha256((ROOT / 'web/tests/eevee_timing.py').read_bytes()).hexdigest()
    assert len(optimization['sessions']['candidate']) == 3
    assert all(run['bundleVersion'] == version and run['referencePixelsEqual']
               and not run['shaderOverrideEntries'] and not run['videoCaptured']
               for run in optimization['sessions']['candidate'])
    memory64 = read(docs / 'memory64.acceptance.json')
    assert memory64['sourceSha256'] == hashlib.sha256((ROOT / 'web/native/tests/memory64.c').read_bytes()).hexdigest()
    assert memory64['browser']['result']['heapBytes'] > 2**32
    assert memory64['browser']['result']['crossThreadReadWrite'] and not memory64['browserErrors']
    assert performance['bundleVersion'] == version
    loading = [read(base / name / 'acceptance.json') for name in
               ['loading-evidence', 'loading-evidence-ui']]
    for report in [*features, *ui, *oceans, *volumes, *image_vectors, *freestyles,
                   eevee, cycles, recording, dependency_recording]:
        assert report['bundleVersion'] == version, 'Mixed browser build evidence'
    for report in image_vectors:
        assert not report['browserErrors'] and report['offlineReopen']
        if report['viewportDrawing']:
            assert report['bluePixels'] > 500
        else:
            assert not report['hardware'] and 'dual-source-blending' not in report['adapter']['features']
            assert report['viewportRequirement'] and report['bluePixels'] == 0
        for case in [*report['images'].values(), *report['offlineImages'].values()]:
            assert case['maximumDesktopPixelError'] < 1e-4 and case['desktopInputPixelError'] < 1e-4
        assert report['svg']['maximumDesktopCoordinateError'] < 2e-5
        assert report['trace']['maximumDesktopCoordinateError'] < 2e-5
        assert report['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/image_vector.py').read_bytes()).hexdigest()
        for name, expected in report['fixtureHashes'].items():
            assert hashlib.sha256((ROOT / 'web/tests/fixtures/image-vector' / name).read_bytes()).hexdigest() == expected
    assert all(report['hardware'] and report['viewportDrawing'] for report in image_vectors[2:])
    assert image_vectors[3]['fallback']
    assert desktop_images['svgMaximumCoordinateError'] < 2e-5
    assert desktop_images['pdfPages'] == 1 and desktop_images['pdfRasterBluePixels'] > 100
    assert all(error < 1e-4 for error in desktop_images['imagePixelErrors'].values())
    assert desktop_images['svgSha256'] == image_vectors[2]['exportSVG']['sha256']
    assert desktop_images['pdfSha256'] == image_vectors[2]['exportPDF']['sha256']
    for report in freestyles:
        assert not report['browserErrors'] and report['redPixels'] > 500
        assert report['meanAbsoluteChannelError'] < 3 and report['outlierFraction'] < .01
        assert report['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/freestyle.py').read_bytes()).hexdigest()
        assert report['referenceImageSha256'] == hashlib.sha256((ROOT / 'web/tests/fixtures/freestyle/freestyle.png').read_bytes()).hexdigest()
    dependency_video = ROOT / dependency_recording['path']
    assert hashlib.sha256(dependency_video.read_bytes()).hexdigest() == dependency_recording['sha256']
    for report in loading:
        assert {run['mode'] for run in report['runs']} == {'cold','warm','offline','restarted-offline'}
        assert all(run['bundleVersion'] == version for run in report['runs'])
        assert not report['browserErrors']
    assert all(report['browserErrors'] == 0 for report in [*features, *ui, eevee])
    assert eevee['referencePixelsEqual']
    assert eevee['shaderSeedEntries'] == manifest['shaderSeed']['entries'] > 50
    assert not cycles['errors']
    assert all(not report['errors'] and report['offlineReopen'] for report in oceans)
    assert all(not report['browserErrors'] and report['offlineReopen']
               and report['maximumCoordinateError'] < 2e-5 and report['gasReplayFrame'] == 4
               and report['fluid']['maximumDesktopDensityError'] < 1e-3 for report in volumes)
    for report in volumes:
        for name, expected in report['fixtureHashes'].items():
            assert hashlib.sha256((ROOT / 'web/tests/fixtures/volume' / name).read_bytes()).hexdigest() == expected
        assert report['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/volume.py').read_bytes()).hexdigest()
    assert desktop_ocean['browserArchiveSHA256'] == oceans[2]['alembic']['sha256']
    assert desktop_ocean['maxCoordinateError'] < 2e-4
    video = docs / 'volume-walkthrough.mp4'
    recording['sha256'] = hashlib.sha256(video.read_bytes()).hexdigest()
    recording['path'] = str(video.relative_to(ROOT))
    selected = lambda variant: sum(file['bytes'] for file in manifest['files'] if
        file['name'].endswith(('/blender.data', f'/{variant}.js', f'/{variant}.wasm')))
    native = {path.stem:read(path) for path in sorted(base.glob('*-check.json'))}
    report = {'bundleVersion':version, **read(ROOT / 'web/upstream.json'),
              'featureVariants':features, 'uiChecks':ui, 'eevee':eevee, 'cycles':cycles,
              'nativeChecks':native, 'recording':recording,
              'oceanAlembic':{'browserVariants':oceans,'browserExportInDesktop':desktop_ocean},
              'openvdbNanoVDB':{'browserVariants':volumes},
              'imageVector':{'browserVariants':image_vectors,'browserExportsInDesktop':desktop_images},
              'freestyle':{'browserVariants':freestyles,'browserSceneInDesktop':desktop_freestyle},
              'dependencyRecording':dependency_recording,
              'dependencyStatus':dependency_status,
              'usdMaterialX':{'browserVariants':usd,'browserExportsInDesktop':usd_native,
                              'recording':usd_recording},
              'performance':performance,
              'renderOptimization':optimization,
              'memory64Probe':{'productionEnabled':False,'standalone':memory64},
              'cyclesReference':{**read(docs / 'cycles-cpu-reference.json'),
                  'imageSHA256':hashlib.sha256((docs / 'cycles-cpu-reference.png').read_bytes()).hexdigest()},
              'downloadSize':{'renderBytes':selected('blender'),
                              'uiFallbackBytes':selected('blender-ui'),
                              'files':manifest['files']},
              'patchHashes':{path.name:hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in sorted((ROOT / 'web/patches').glob('*.patch'))},
              'limitations':[
                  'Cycles CPU acceptance uses the factory cube at 128x128 and 16 samples, with denoising and adaptive sampling disabled.',
                  'Cycles GPU, denoising, progressive display and rendered viewport are not supported.',
                  'Final rendering requires the JSPI bundle in current desktop Chromium.',
                  'Production Blender uses wasm32 with a 4 GiB linear-memory ceiling. The memory64 result is a separate sparse-addressing probe, not Blender scene-capacity evidence.',
                  'NumPy has no BLAS; native subprocesses, arbitrary dynamic extension modules and the extension marketplace remain unsupported.',
                  'Device file selection, browser downloads, additional windows and remaining dependency groups are still pending.',
                  'Ocean acceptance uses resolution 5 and 12 animation frames. Ocean disk-cache baking is not covered.',
                  'OpenVDB acceptance covers float sphere grids, ZIP/Blosc IO, NanoVDB conversion and mesh/volume modifiers. Mantaflow acceptance covers four resolution-16 gas frames; liquid simulation, long bakes and the Python OpenVDB module are not covered.',
                  'Image acceptance covers small WebP lossless, JP2 16-bit, Cineon 10-bit and DPX 16-bit files. Other format variants, large images and animation are not covered.',
                  'Grease Pencil viewport drawing requires the WebGPU dual-source-blending feature; SwiftShader validates image/vector IO with the authored object hidden. Hardware checks cover drawing in both runtime variants.',
                  'Vector acceptance covers authored paths, fills and a circle, SVG/PDF export and one-image Potrace tracing. This does not establish every SVG feature or multi-frame vector export.',
                  'Freestyle acceptance covers a 256x256, 16-sample Cycles CPU cube with red contour strokes. Other style modules and large scenes are not covered.',
                  'USD acceptance covers animated mesh, UV, camera/light and environment-texture exchange in USDA/USDC/USDZ, eight implicit shapes, static pxr Python modules and USD hooks. MaterialX checks cover OpenPBR graph export. Hydra rendering, MaterialX Python bindings and arbitrary USD production assets are not covered.',
                  'Alembic acceptance covers Ogawa mesh deformation, UVs and animated transforms; HDF5 archives and other schema types are not covered.',
                  'Functional checks ran with other host activity and are not an isolated performance benchmark.']}
    archive = base / 'blender-web-pages.tar.gz'
    def normalize(member):
        member.uid = member.gid = member.mtime = 0
        member.uname = member.gname = ''
        member.mode = 0o755 if member.isdir() else 0o644
        member.pax_headers = {}
        return member
    with archive.open('wb') as output:
        with gzip.GzipFile(filename='', mode='wb', fileobj=output, mtime=0, compresslevel=6) as compressed:
            with tarfile.open(fileobj=compressed, mode='w') as bundle:
                bundle.add(base / 'dist', arcname='.', filter=normalize)
    report['deploymentArchive'] = {'name':archive.name, 'bytes':archive.stat().st_size,
                                    'sha256':hashlib.sha256(archive.read_bytes()).hexdigest()}
    write(docs / 'web-features.acceptance.json', report)
    write(docs / 'loading-cache.acceptance.json', {
        'render':loading[0], 'uiFallback':loading[1],
        'deployments':read(base / 'cache-evidence/acceptance.json'),
        'nativeRegression':ui[0],
        'limitations':['Only complete runtime files are cached; interrupted files restart.',
                       'Browsers may evict storage. Old content is not yet automatically pruned.']})
    print(json.dumps({'bundleVersion':version, 'renderMiB':selected('blender')/2**20,
                      'recordingSHA256':recording['sha256'],
                      'deploymentArchive':report['deploymentArchive']}))


if __name__ == '__main__':
    main()
