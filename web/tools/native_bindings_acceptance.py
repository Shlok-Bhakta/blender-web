#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require matching binding, AVIF, native readback and recording evidence."""
import hashlib
import json
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(base, manifest):
    read = lambda path: json.loads(path.read_text())
    library = read(base / 'python-bindings-check.json')
    avif = read(base / 'avif-check.json')
    assert avif['libheifVersion'] == '1.20.2' and avif['originalOIIOPlugin']
    assert avif['oddDimensions'] == [37, 23] and avif['invalidInputRejected'] and avif['decoderRecovery']
    assert {(item['bits'], item['channels']) for item in avif['cases']} == {
        (bits, channels) for bits in (8, 10, 12) for channels in (1, 3, 4)}
    for name, expected in avif['sourceHashes'].items():
        assert digest(ROOT / name) == expected, name
    for name, expected in library['sourceHashes'].items():
        assert digest(ROOT / name) == expected, name
    assert library['cleanInterpreterShutdown']
    assert library['versions'] == dict(OpenImageIO='3.1.13.1', PyOpenColorIO='2.5.0', MaterialX='1.39.4')
    installation = base / 'deps/python-bindings/installed.json'
    assert digest(installation) == library['bindingInstallationSha256']
    installed = read(installation)
    assert len(installed['modules']) == 11
    for name, expected in installed['artifacts'].items():
        assert digest(base / 'deps/prefix' / name) == expected, name
    source = digest(ROOT / 'web/native/tests/python-bindings.py')
    avif_source = digest(ROOT / 'web/native/tests/avif.py')
    runner = digest(ROOT / 'web/tests/python-bindings.cjs')
    verifier = digest(ROOT / 'web/tools/native_bindings_verify.py')
    variants, native = [], []
    for suffix, hardware, fallback in (('', False, False), ('-ui', False, True), ('-hardware', True, False)):
        directory = base / ('python-bindings-evidence' + suffix)
        report = read(directory / 'acceptance.json')
        verified = read(directory / 'native-verification.json')
        assert report['bundleVersion'] == verified['bundleVersion'] == manifest['version']
        assert report['hardware'] == hardware and report['fallback'] == fallback
        assert report['sourceSha256'] == verified['sourceSha256'] == source
        assert report['avifSourceSha256'] == verified['avifSourceSha256'] == avif_source
        assert report['runnerSourceSha256'] == runner and verified['verifierSourceSha256'] == verifier
        assert report['offlineReopen'] and not report['browserErrors'] and not report['startupPythonErrors']
        assert report['report'] == {key: library[key] for key in report['report']}
        result = report['report']
        assert result['originalPackageWrappers']
        assert all(result['color'][key] for key in ('serialization', 'shaderGeneration', 'invalidColorSpaceRejected'))
        assert result['color']['parallelCalls'] == result['images']['parallelCalls'] == result['material']['parallelCalls'] == 9
        assert result['volume']['parallelCalls'] == 12 and result['volume']['axDisabled']
        assert len(result['volume']['gridTypes']) == 9
        assert next(item['value'] for item in result['volume']['gridTypes'] if item['name'] == 'Int64Grid') == '18014398509481991'
        assert {item['target'] for item in result['material']['shaders']} == {'genglsl', 'genmsl', 'genosl', 'genmdl'}
        assert report['avif']['originalBlenderAVIF'] and report['avif']['oddWidth'] == 37
        assert report['avif']['truncatedInputRejected'] and report['avif']['decoderRecovery']
        assert {(item['bits'], item['mode']) for item in report['avif']['cases']} == {
            (bits, mode) for bits in (8, 10, 12) for mode in ('BW', 'RGB', 'RGBA')}
        assert all(verified[key] for key in ('nativeAPIsEqual', 'volumeNumpyExact', 'int64Exact', 'volumeMetadata',
            'volumeTransform', 'colorConfig', 'materialDocument', 'shaderTextExactlyMatchesNative',
            'savedBlendVerified', 'originalBlenderAVIF'))
        assert verified['volumeGridTypes'] == 9 and len(verified['avif']) == 9
        assert report['files'] == verified['files']
        for name, entry in report['files'].items():
            assert (directory / name).stat().st_size == entry['bytes']
            assert digest(directory / name) == entry['sha256'], name
        variants.append(report)
        native.append(verified)
    recording = read(ROOT / 'web/docs/python-bindings-recording.json')
    assert recording['bundleVersion'] == manifest['version'] and recording['sourceSha256'] == source
    assert recording['avifSourceSha256'] == avif_source
    for entry in (recording, recording['screenshot']):
        assert digest(ROOT / entry['path']) == entry['sha256']
    return dict(libraryChecks=library, standaloneAVIF=avif,
                browserVariants=variants, nativeReadback=native, recording=recording)
