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
    recording = read(docs / 'ocean-recording.json')
    oceans = [read(base / name / 'acceptance.json') for name in
              ['ocean-evidence', 'ocean-evidence-ui', 'ocean-evidence-hardware']]
    desktop_ocean = read(base / 'ocean-evidence-hardware/desktop-verification.json')
    performance = read(docs / 'native-web-performance.json')
    assert performance['bundleVersion'] == version
    loading = [read(base / name / 'acceptance.json') for name in
               ['loading-evidence', 'loading-evidence-ui']]
    for report in [*features, *ui, *oceans, eevee, cycles, recording]:
        assert report['bundleVersion'] == version, 'Mixed browser build evidence'
    for report in loading:
        assert {run['mode'] for run in report['runs']} == {'cold','warm','offline','restarted-offline'}
        assert all(run['bundleVersion'] == version for run in report['runs'])
        assert not report['browserErrors']
    assert all(report['browserErrors'] == 0 for report in [*features, *ui, eevee])
    assert eevee['referencePixelsEqual']
    assert not cycles['errors']
    assert all(not report['errors'] and report['offlineReopen'] for report in oceans)
    assert desktop_ocean['browserArchiveSHA256'] == oceans[2]['alembic']['sha256']
    assert desktop_ocean['maxCoordinateError'] < 2e-4
    video = docs / 'ocean-alembic-walkthrough.mp4'
    recording['sha256'] = hashlib.sha256(video.read_bytes()).hexdigest()
    recording['path'] = str(video.relative_to(ROOT))
    selected = lambda variant: sum(file['bytes'] for file in manifest['files'] if
        file['name'].endswith(('/blender.data', f'/{variant}.js', f'/{variant}.wasm')))
    native = {path.stem:read(path) for path in sorted(base.glob('*-check.json'))}
    report = {'bundleVersion':version, **read(ROOT / 'web/upstream.json'),
              'featureVariants':features, 'uiChecks':ui, 'eevee':eevee, 'cycles':cycles,
              'nativeChecks':native, 'recording':recording,
              'oceanAlembic':{'browserVariants':oceans,'browserExportInDesktop':desktop_ocean},
              'performance':performance,
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
                  'NumPy has no BLAS; native subprocesses, arbitrary dynamic extension modules and the extension marketplace remain unsupported.',
                  'Device file selection, browser downloads, additional windows and remaining dependency groups are still pending.',
                  'Ocean acceptance uses resolution 5 and 12 animation frames. OpenVDB, fluids and ocean disk-cache baking are not covered.',
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
