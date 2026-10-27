#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require matching USD CPU plugin installation, operation and browser evidence."""
import hashlib
import json
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(base, manifest):
    read = lambda path: json.loads(path.read_text())
    installation = base / 'deps/usd-imaging/installed.json'
    installed = read(installation)
    assert installed['components'] == ['hioOpenVDB', 'hioOiio'] and not installed['gpuEnabled']
    for name, expected in installed['artifacts'].items():
        assert digest(base / 'deps/prefix' / name) == expected, name
    for name, expected in installed['inputs'].items():
        assert digest(ROOT / name) == expected, name
    for name, expected in installed['dependencyStamps'].items():
        assert digest(base / f'deps/{name}/installed.json') == expected, name
    unit = read(base / 'usd-imaging-check.json')
    directory = base / 'usd-imaging-check'
    assert digest(base / 'usd-imaging-check.json') == digest(directory / 'unit-report.json')
    assert unit['installationSha256'] == digest(installation)
    assert unit['volumeTypes'] == 4 and not unit['gpuRequired']
    assert all(unit[key] for key in ('originalPluginRegistry', 'tiffPixelsExact',
        'vdbDenseVoxelsExact', 'volumeTransformsExact', 'missingGridRecovery', 'hioWriteReadExact'))
    for name, expected in unit['sourceHashes'].items():
        assert digest(ROOT / name) == expected, name
    native = read(directory / 'native-verification.json')
    assert native['unitReportSha256'] == digest(directory / 'unit-report.json')
    assert native['verifierSourceSha256'] == digest(ROOT / 'web/tools/native_usd_imaging_verify.py')
    assert all(native[key] for key in ('nativePixelsExact', 'nativeHioWrittenPixelsExact',
                                     'nativeVoxelsExact', 'nativeTransformExact'))
    assert native['volumeTypes'] == 4 and all(native['nativePlugins'].values())
    for name, expected in native['files'].items():
        assert digest(directory / name) == expected, name
    variants = []
    for suffix, hardware, fallback in (('', False, False), ('-hardware', True, False), ('-ui', False, True)):
        report = read(base / ('usd-evidence' + suffix) / 'acceptance.json')
        assert report['bundleVersion'] == manifest['version']
        assert report['sourceSha256'] == digest(ROOT / 'web/tests/usd_exchange.py')
        assert report['hardware'] == hardware and report['fallback'] == fallback
        assert report['offlineReopen'] and not report['browserErrors']
        for name, kind in (('hioOiio', 'HioOIIO_Image'), ('hioOpenVDB', 'HioOpenVDB_TextureData')):
            assert report['cpuImagingPlugins'][name] == dict(loaded=True, type=kind, staticLibrary=True)
        variants.append(report['cpuImagingPlugins'])
    return dict(bundleVersion=manifest['version'], installation=installed, operations=unit, nativeReadback=native,
                browserRegistries=variants)
