#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Publish acceptance evidence only when every browser test used the same bundle."""
import gzip
import hashlib
import json
import re
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
    from native_python_acceptance import validate as validate_python
    python_foundation = validate_python(base, manifest)
    from native_render_libraries_acceptance import validate as validate_render_libraries
    render_libraries = validate_render_libraries(base, manifest)
    embree_names=['embree-evidence','embree-evidence-ui','embree-evidence-hardware']
    embree=[read(base/name/'acceptance.json') for name in embree_names]
    embree_native=[read(base/name/'native-verification.json') for name in embree_names]
    embree_library=read(base/'embree-check.json')
    for name,expected in embree_library['sourceHashes'].items():
        assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==expected
    assert embree_library['version']=='4.4.1' and embree_library['parallelRays']==65536
    assert embree_library['workers'] > 1
    assert all(embree_library[name] for name in ('rayPacket4','maskedMiss','occlusion','instance','motion','spherePoint','roundCurve','filter','invalidGeometryRecovery'))
    embree_source=hashlib.sha256((ROOT/'web/tests/embree.py').read_bytes()).hexdigest()
    assert manifest['buildOptions']['WITH_CYCLES_EMBREE']
    for name,variant,verified in zip(embree_names,embree,embree_native):
        assert variant['bundleVersion']==verified['bundleVersion']==version
        assert variant['sourceSha256']==verified['sourceSha256']==embree_source
        assert variant['scene']==verified['scene'] and verified['savedBlendVerified']
        assert variant['scene']['withEmbree'] and variant['scene']['instances']==64 and variant['scene']['animatedObjects']==8
        assert variant['scene']['curvePoints']==56 and variant['scene']['spherePoints']==10 and variant['scene']['motionBlur']
        assert variant['offlineReopen'] and not variant['browserErrors']
        assert verified['blendSha256']==variant['blend']['sha256']==hashlib.sha256((base/name/'embree.blend').read_bytes()).hexdigest()
        assert verified['nativeRender']['meanAbsoluteError']<1.5 and verified['nativeRender']['outlierFraction']<.005
        assert verified['nativeRender']['sha256']==hashlib.sha256((base/name/verified['nativeRender']['path']).read_bytes()).hexdigest()
        if not variant['fallback']:
            assert len(variant['backends'])==4
            assert {entry['actual'] for entry in variant['backends']}=={'BVH2','EMBREE'}
            assert all(entry['backend']==entry['actual'] for entry in variant['backends'])
            assert set(variant['images'])=={'bvh2-1.png','bvh2-3.png','embree-1.png','embree-3.png'}
            for image_name,entry in variant['images'].items():
                assert entry['sha256']==hashlib.sha256((base/name/image_name).read_bytes()).hexdigest()
                assert entry['meanAbsoluteError']<1.5 and entry['outlierFraction']<.005
                assert set(entry['regions'])=={'curves','points'}
                assert all(region['meanAbsoluteError']<2 and region['outlierFraction']<.02 for region in entry['regions'].values())
        for fixture,expected in variant['fixtureHashes'].items():
            path=ROOT/'web/tests/embree.py' if fixture=='check.py' else ROOT/'web/tests/fixtures/embree'/fixture
            assert hashlib.sha256(path.read_bytes()).hexdigest()==expected
    embree_recording=read(docs/'embree-recording.json')
    assert embree_recording['bundleVersion']==version and embree_recording['sourceSha256']==embree_source
    for entry in (embree_recording,embree_recording['screenshot']):
        assert hashlib.sha256((ROOT/entry['path']).read_bytes()).hexdigest()==entry['sha256']
    embree_performance=read(docs/'embree-performance.json')
    assert embree_performance['bundleVersion']==version and embree_performance['sourceSha256']==embree_source
    for backend,probe in embree_performance['tinyCubeProbe']['backends'].items():
        path=base/('embree-cube-'+backend.lower())/'acceptance.json'
        assert probe['reportSha256']==hashlib.sha256(path.read_bytes()).hexdigest()
        assert read(path)['bundleVersion']==version and read(path)['bvhBackend']==backend
    embree_profile=read(base/'embree-profile/acceptance.json')
    assert embree_profile['bundleVersion']==version and embree_profile['profiled'] and not embree_profile['videoCaptured']
    assert embree_performance['profiling']['wasmThreadsSampled'] > 1
    assert embree_performance['profiling']['traceSha256']==hashlib.sha256((base/'embree-profile/browser-trace-0.json').read_bytes()).hexdigest()
    gltf_names = ['gltf-evidence', 'gltf-evidence-ui', 'gltf-evidence-hardware']
    gltf = [read(base / name / 'acceptance.json') for name in gltf_names]
    gltf_native = [read(base / name / 'native-verification.json') for name in gltf_names]
    gltf_library = read(base / 'gltf-check.json')
    for name,expected in gltf_library['sourceHashes'].items():
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == expected
    assert gltf_library['draco'] and gltf_library['meshoptVertexVersions'] == [0, 1]
    assert gltf_library['legacyQuaternionStride']
    assert gltf_library['capsuleCleanup'] and gltf_library['truncatedDataRejected']
    assert len(gltf_library['rejectedCases']) >= 16
    gltf_source_hash = hashlib.sha256((ROOT / 'web/tests/gltf.py').read_bytes()).hexdigest()
    for variant, verified in zip(gltf, gltf_native):
        assert variant['bundleVersion'] == verified['bundleVersion'] == version
        assert variant['sourceSha256'] == verified['sourceSha256'] == gltf_source_hash
        assert variant['offlineReopen'] and not variant['browserErrors']
        assert not variant['developmentOverlay']
        assert variant['bridge'] == {key:value for key,value in gltf_library.items() if key != 'sourceHashes'}
        assert variant['colorFilter']['knownColors'] == 4
        assert variant['colorFilter']['maximumUnorm8Error'] <= 2
        assert variant['colorFilter']['cachedBufferReused']
        assert set(variant['cases']) == set(verified['cases']) == {'draco', 'meshopt-ext', 'meshopt-khr'}
        assert variant['exports'] == verified['exports'] and verified['savedBlendVerified']
        assert variant['blend']['sha256'] == verified['blendSha256']
        assert variant['saved'] == variant['reopened']
        assert len(variant['malformed']['rejectedFiles']) == 3 and variant['malformed']['validImportAfterFailures']
        assert variant['referenceSha256'] == hashlib.sha256((ROOT / 'web/tests/fixtures/gltf/results.json').read_bytes()).hexdigest()
        for name, expected in variant['fixtureHashes'].items():
            path = (ROOT / 'web/tests/gltf.py' if name == 'check.py' else
                    ROOT / 'web/native/tests/gltf.py' if name == 'bridge-check.py' else
                    ROOT / 'web/tests/fixtures/gltf' / name)
            assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
        for name, entry in variant['cases'].items():
            assert entry['nativeImportError'] < .005 and entry['roundtripError'] < .005
            assert verified['cases'][name]['maximumPositionError'] < .005
            assert entry['snapshot']['triangles'] == 528 and entry['snapshot']['shapeKeys'] == 2
    gltf_recording = read(docs / 'gltf-recording.json')
    assert gltf_recording['bundleVersion'] == version
    for entry in (gltf_recording, gltf_recording['screenshot']):
        assert hashlib.sha256((ROOT / entry['path']).read_bytes()).hexdigest() == entry['sha256']
    tracking = [read(base / name / 'acceptance.json') for name in
                ['tracking-evidence', 'tracking-evidence-ui', 'tracking-evidence-hardware']]
    tracking_native = [read(base / name / 'native-verification.json') for name in
                       ['tracking-evidence', 'tracking-evidence-ui', 'tracking-evidence-hardware']]
    tracking_libraries = read(base / 'tracking-check.json')
    assert tracking_libraries['denseQR'] and tracking_libraries['eigenSparse']
    assert tracking_libraries['requestedThreads'] == 8
    assert tracking_libraries['threads'] == min(8, tracking_libraries['hardwareThreads'] or 8)
    assert tracking_libraries['maximumParameterError'] < 1e-7
    tracking_reference = read(ROOT / 'web/tests/fixtures/tracking/results.json')
    tracking_source_hash = hashlib.sha256((ROOT / 'web/tests/tracking.py').read_bytes()).hexdigest()
    assert tracking_reference['sourceSha256'] == tracking_source_hash
    for variant, verified in zip(tracking, tracking_native):
        assert variant['bundleVersion'] == verified['bundleVersion'] == version
        assert variant['sourceSha256'] == verified['sourceSha256'] == tracking_source_hash
        assert variant['libmv'] and variant['offlineReopen'] and not variant['browserErrors']
        assert variant['maximumTrackErrorPixels'] < .15 and variant['maximumNativeMarkerErrorPixels'] < .05
        assert variant['solve']['valid'] and variant['solve']['cameras'] == 12 and variant['solve']['bundles'] == 32
        assert variant['solve']['averageErrorPixels'] < .05
        assert variant['solve']['maximumTrajectoryRatioError'] < .01
        assert variant['maximumNativeTrajectoryRatioError'] < .001
        assert variant['saved'] == variant['reopened'] == verified['desktop']
        assert variant['blend']['sha256'] == verified['blendSha256']
        assert variant['referenceSha256'] == hashlib.sha256((ROOT / 'web/tests/fixtures/tracking/results.json').read_bytes()).hexdigest()
        for name, expected in variant['fixtureHashes'].items():
            path = ROOT / 'web/tests/tracking.py' if name == 'check.py' else ROOT / 'web/tests/fixtures/tracking' / name
            assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
    tracking_recording = read(docs / 'tracking-recording.json')
    assert tracking_recording['bundleVersion'] == version
    for entry in [tracking_recording, tracking_recording['screenshot']]:
        assert hashlib.sha256((ROOT / entry['path']).read_bytes()).hexdigest() == entry['sha256']
    text = [read(base / name / 'acceptance.json') for name in
            ['text-evidence', 'text-evidence-ui', 'text-evidence-hardware']]
    text_libraries = read(base / 'text-check.json')
    assert text_libraries['arabicGlyphs'] == 3 and text_libraries['devanagariGlyphs'] == 2
    assert text_libraries['bidiMixedDirection'] and text_libraries['unicodeKeys']
    assert text_libraries['signedCatalogIndices']
    text_reference = read(ROOT / 'web/tests/fixtures/text/results.json')
    for variant in text:
        assert variant['bundleVersion'] == version
        assert variant['offlineReopen'] and variant['preferencesRestored'] and not variant['browserErrors']
        assert variant['buildOptions']['international']
        assert variant['translations'] == text_reference['translations']
        assert variant['catalogs'] == text_reference['catalogs'] and len(variant['catalogs']) == 49
        assert variant['fontPixels']['maximumError'] <= 4 and variant['fontPixels']['meanError'] < .05
        assert variant['unicodeInput']['typed'] == variant['unicodeInput']['reopened'] == 'Café 日本 😀'
        assert variant['unicodeInput']['deadKeyIgnored'] and variant['unicodeInput']['trustedBrowserEvents']
        assert variant['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/text.py').read_bytes()).hexdigest()
        for name, expected in variant['fixtureHashes'].items():
            path = ROOT / 'web/tests/text.py' if name == 'check.py' else ROOT / 'web/tests/fixtures/text' / name
            assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
    text_recording = read(docs / 'text-recording.json')
    assert text_recording['bundleVersion'] == version
    for entry in [text_recording, text_recording['screenshot']]:
        assert hashlib.sha256((ROOT / entry['path']).read_bytes()).hexdigest() == entry['sha256']
    cmake = dict(re.findall(r'^(\w+):(?:BOOL|STRING)=(.*)$',
                           (base / 'editor/CMakeCache.txt').read_text(), re.MULTILINE))
    wasm_build = {'cxxReleaseFlags': cmake['CMAKE_CXX_FLAGS_RELEASE'],
                  'linkOptimization': cmake['BLENDER_WEB_LINK_OPTIMIZATION'],
                  'debugFunctionNames': cmake['BLENDER_WEB_DEBUG_NAMES'] == 'ON',
                  'extraBinaryenInlining': cmake['BLENDER_WEB_BINARYEN_INLINING'] == 'ON'}
    assert wasm_build['linkOptimization'] == '3'
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
    media = [read(base / name / 'acceptance.json') for name in
             ['media-evidence', 'media-evidence-ui', 'media-evidence-hardware']]
    media_native = [read(base / name / 'native-verification.json') for name in
                    ['media-evidence', 'media-evidence-ui', 'media-evidence-hardware']]
    media_recording = read(docs / 'media-recording.json')
    media_performance = read(docs / 'media-performance.json')
    assert media_performance['bundleVersion'] == version
    assert media_performance['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/media.py').read_bytes()).hexdigest()
    media_libraries = read(base / 'media-check.json')
    for name, expected in media_libraries['sourceHashes'].items():
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == expected
    assert media_libraries['rubberBandOutputFrames'] == 48000
    assert media_libraries['videoEncoders'] == ['libaom-av1', 'libx265']
    assert media_libraries['videoFrames'] == 24 and media_libraries['decoderThreads'] == 4
    assert media_libraries['cpuCores'] <= 1 or media_libraries['automaticEncoderThreads'] > 1
    for variant, verified in zip(media, media_native):
        assert variant['bundleVersion'] == verified['bundleVersion'] == version
        assert variant['offlineReopen'] and not variant['browserErrors']
        assert all(variant['buildOptions'][name] for name in ('audaspace', 'codec_ffmpeg', 'codec_sndfile'))
        assert set(variant['stretch']) == set(verified['stretch']) == {'slow', 'fast', 'pitch'}
        if not variant['fallback']:
            assert set(verified['videos']) == {'mp4', 'webm', 'av1.mkv', 'hevc.mkv'}
        assert len(variant['audio']) == 7 and len(variant['movies']) == 3
        assert variant['playback']['state'] == variant['offlinePlayback']['state'] == 'running'
        assert verified['playbackCapture']['stereoPeakHz'] == [440, 660]
        assert 22 < variant['playback']['timelineFps'] < 26
        assert variant['previewPixelsMatched'] > 1000
        assert variant['interactiveSelectionReadback'] == (not variant['fallback'])
        assert variant['sourceSha256'] == hashlib.sha256((ROOT / 'web/tests/media.py').read_bytes()).hexdigest()
        for name, expected in variant['fixtureHashes'].items():
            path = ROOT / 'web/tests/media.py' if name == 'check.py' else ROOT / 'web/tests/fixtures/media' / name
            assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
    assert media_recording['bundleVersion'] == version
    assert hashlib.sha256((ROOT / media_recording['path']).read_bytes()).hexdigest() == media_recording['sha256']
    assert hashlib.sha256((ROOT / media_recording['screenshot']['path']).read_bytes()).hexdigest() == media_recording['screenshot']['sha256']
    for name in ('usd', 'materialx', 'x264', 'harfbuzz', 'embree'):
        installed = read(base / 'deps' / name / 'installed.json')
        patch = ROOT / 'web/native/patches' / (name + '.patch')
        assert installed['patch'] == hashlib.sha256(patch.read_bytes()).hexdigest()
    assert read(base / 'usd-python-check.json')['sphereAdapterPoints'] == 92
    assert read(base / 'materialx-check.json')['openPBRGraphValid']
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
    assert hashlib.sha256((ROOT / usd_recording['screenshot']['path']).read_bytes()).hexdigest() == usd_recording['screenshot']['sha256']
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
        file['name'].endswith(('/blender.data', f'/{variant}.js', f'/{variant}.wasm', '/audio-worklet.js')))
    native = {path.stem:read(path) for path in sorted(base.glob('*-check.json'))}
    report = {'bundleVersion':version, **read(ROOT / 'web/upstream.json'),
              'featureVariants':features, 'uiChecks':ui, 'eevee':eevee, 'cycles':cycles,
              'renderLibraries':render_libraries,
              'pythonFoundation':python_foundation,
              'nativeChecks':native, 'recording':recording, 'wasmBuild':wasm_build,
              'oceanAlembic':{'browserVariants':oceans,'browserExportInDesktop':desktop_ocean},
              'openvdbNanoVDB':{'browserVariants':volumes},
              'imageVector':{'browserVariants':image_vectors,'browserExportsInDesktop':desktop_images},
              'freestyle':{'browserVariants':freestyles,'browserSceneInDesktop':desktop_freestyle},
              'dependencyRecording':dependency_recording,
              'dependencyStatus':dependency_status,
              'usdMaterialX':{'browserVariants':usd,'browserExportsInDesktop':usd_native,
                              'recording':usd_recording},
              'textTranslation':{'browserVariants':text,'libraryChecks':text_libraries,
                                  'recording':text_recording},
              'embree':dict(libraries=embree_library,variants=embree,nativeReadback=embree_native,recording=embree_recording,performance=embree_performance,profile=embree_profile),
              'gltf':{'browserVariants':gltf,'browserFilesInDesktop':gltf_native,
                      'libraryChecks':gltf_library,'recording':gltf_recording},
              'tracking':{'browserVariants':tracking,'browserFilesInDesktop':tracking_native,
                          'libraryChecks':tracking_libraries,'recording':tracking_recording},
              'media':{'browserVariants':media,'independentNativeVerification':media_native,
                       'recording':media_recording,'performance':media_performance},
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
              'dependencyPatchHashes':{path.name:hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in sorted((ROOT / 'web/native/patches').glob('*.patch'))},
              'limitations':[
                  'Cycles CPU acceptance uses the factory cube at 128x128 and 16 samples, with denoising and adaptive sampling disabled.',
                  'Cycles GPU devices, progressive display and rendered viewport are not supported. CPU denoising and path guiding have separate authored-scene acceptance.',
                  'Final rendering requires the JSPI bundle in current desktop Chromium.',
                  'Production Blender uses wasm32 with a 4 GiB linear-memory ceiling. The memory64 result is a separate sparse-addressing probe, not Blender scene-capacity evidence.',
                  'NumPy has no BLAS; native subprocesses, arbitrary dynamic extension modules and the extension marketplace remain unsupported.',
                  'Python HTTP uses browser TLS and CORS with buffered responses. Tests cover original conditional and two-worker background downloads, not unrestricted sockets, proxy configuration or marketplace installation. Cython code generation works; compiling and loading its generated extensions in the browser remains pending.',
                  'Device file selection, browser downloads, additional windows and remaining dependency groups are still pending.',
                  'Embree acceptance covers 64 linked icospheres, round curves, sphere points, transparency and motion blur at 256x192 with 16 samples. Original C++ logs verify BVH2/Embree selection; standalone tests also cover ray packets, masks, occlusion, instances, motion, filters and error recovery. SYCL/GPU and ISPC paths are disabled. Large scenes, subdivision primitives and arbitrary user geometry remain untested.',
                  'Compressed glTF acceptance covers small authored meshes, UV/color/normal moments, morph targets and animation in Draco and EXT/KHR meshoptimizer. Skinning, external asset collections and large-scene stress tests remain pending.',
                  'All 49 catalogs match native files; translated strings are sampled in six languages. Unicode keyboard input covers one scalar per event. HarfBuzz/FriBidi are verified library ports, not integrated into this release\'s original font renderer. Full shaping and IME composition remain pending.',
                  'Tracking acceptance uses one translated textured patch over twelve 256x192 frames and a synthetic twelve-camera, thirty-two-point reconstruction. Real footage, lens refinement, object/tripod solves and long production clips are not covered. Ceres uses Eigen dense/sparse solvers; CUDA, SuiteSparse and LAPACK are disabled.',
                  'Ocean acceptance uses resolution 5 and 12 animation frames. Ocean disk-cache baking is not covered.',
                  'OpenVDB acceptance covers float sphere grids, ZIP/Blosc IO, NanoVDB conversion and mesh/volume modifiers. Mantaflow acceptance covers four resolution-16 gas frames; liquid simulation, long bakes and the Python OpenVDB module are not covered.',
                  'Image acceptance covers small WebP lossless, JP2 16-bit, Cineon 10-bit and DPX 16-bit files. Other format variants, large images and animation are not covered.',
                  'Grease Pencil viewport drawing requires the WebGPU dual-source-blending feature; SwiftShader validates image/vector IO with the authored object hidden. Hardware checks cover drawing in both runtime variants.',
                  'Vector acceptance covers authored paths, fills and a circle, SVG/PDF export and one-image Potrace tracing. This does not establish every SVG feature or multi-frame vector export.',
                  'Freestyle acceptance covers a 256x256, 16-sample Cycles CPU cube with red contour strokes. Other style modules and large scenes are not covered.',
                  'USD acceptance covers animated mesh, UV, camera/light and environment-texture exchange in USDA/USDC/USDZ, eight implicit shapes, static pxr Python modules and USD hooks. MaterialX checks cover OpenPBR graph export. Hydra rendering, MaterialX Python bindings and arbitrary USD production assets are not covered.',
                  'Media acceptance uses one-second stereo tones and 128x96 video, with 512x512 AV1 exports. Rubber Band checks cover constant time/pitch ratios and stereo tones, including the native real-time processing tail. AV1 and 8-bit HEVC exports are independently decoded. A separate three-frame float-gradient fixture covers 8/10/12-bit HEVC, with native sample comparisons and bit-exact codec checks. Microphone capture, multichannel devices, external video devices and large production timelines are not covered.',
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
