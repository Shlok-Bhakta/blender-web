#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require current Python library, browser, native readback and recording proofs."""
import hashlib
import json
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(base, manifest):
    read = lambda path: json.loads(path.read_text())
    library = read(base / 'python-check.json')
    for name, expected in library['sourceHashes'].items():
        assert digest(ROOT / name) == expected, name
    assert library['pythonVersion'] == '3.13.13' and library['numpyCompiledModules']
    foundation = library['foundation']
    source = digest(ROOT / 'web/native/tests/python-foundation.py')
    runner = digest(ROOT / 'web/tests/python-foundation.cjs')
    verifier = digest(ROOT / 'web/tools/native_python_verify.py')
    core_flags = ('sqliteCallbacks', 'sqliteSerialization', 'sqliteWAL', 'sqliteWriterLock',
                  'tls12', 'tls13', 'untrustedRejected', 'hostnameRejected', 'scrypt',
                  'pbkdf2', 'hmac', 'cythonCodeGeneration', 'downloaderWorkerChannel', 'metadataLock')
    network_flags = ('binaryGET', 'binaryPOST', 'responseChunks', 'browserDecompression',
                     'customHeader', 'redirect', 'status404', 'timeout', 'corsRejected',
                     'recovery', 'originalConditionalDownloader', 'conditional304',
                     'originalBackgroundDownloader', 'workerShutdown')
    assert all(foundation[key] for key in core_flags)
    assert foundation['lzmaFormats'] == 3 and foundation['zstandardThreads'] == 2
    assert foundation['parallelTLS'] == 2 and len(foundation['packages']) == 19
    variants, native = [], []
    for suffix, hardware, fallback in (('', False, False), ('-ui', False, True), ('-hardware', True, False)):
        directory = base / ('python-foundation-evidence' + suffix)
        report = read(directory / 'acceptance.json')
        verified = read(directory / 'native-verification.json')
        assert report['bundleVersion'] == verified['bundleVersion'] == manifest['version']
        assert report['hardware'] == hardware and report['fallback'] == fallback
        assert report['sourceSha256'] == verified['sourceSha256'] == source
        assert report['runnerSourceSha256'] == runner and verified['verifierSourceSha256'] == verifier
        assert report['offlineReopen'] and not report['browserErrors'] and not report['startupPythonErrors']
        assert all(report['report'][key] for key in core_flags)
        assert report['report']['packages'] == foundation['packages'] == verified['nativePackageVersions']
        assert all(report['report']['network'][key] for key in network_flags)
        assert report['report']['network']['parallelDownloads'] == 2
        assert report['peakBackgroundDownloads'] == 2
        assert report['report']['network']['networkStreaming'] is False
        assert all(verified[key] for key in ('sqliteReadback', 'lzmaReadback', 'zstandardReadback',
            'savedBlendVerified', 'cythonCodeInspected', 'httpDownloadsVerified',
            'sqliteOptionsMatchExceptMmapAndCompiler', 'tlsCertificateSignature'))
        assert report['files'] == verified['files']
        for name, entry in report['files'].items():
            assert (directory / name).stat().st_size == entry['bytes']
            assert digest(directory / name) == entry['sha256'], name
        variants.append(report)
        native.append(verified)
    recording = read(ROOT / 'web/docs/python-foundation-recording.json')
    assert recording['bundleVersion'] == manifest['version'] and recording['sourceSha256'] == source
    for entry in (recording, recording['screenshot']):
        assert digest(ROOT / entry['path']) == entry['sha256']
    return dict(libraryChecks=library, browserVariants=variants, nativeReadback=native, recording=recording)
