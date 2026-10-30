#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require compiler checks, browser OSL queries, and independent native readback."""
import hashlib
import json
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(base, manifest):
    read = lambda path: json.loads(path.read_text())
    compiler = read(base / 'compiler-check.json')
    library = read(base / 'osl-python-check.json')
    for report in (compiler, library):
        for name, expected in report['sourceHashes'].items():
            assert digest(ROOT / name) == expected, name
        for name, entry in report['artifacts'].items():
            artifact = base / name
            assert artifact.stat().st_size == entry['bytes'] and digest(artifact) == entry['sha256'], name
    assert compiler['llvm']['compiledModules'] == 3
    assert compiler['llvm']['invalidIRRejected'] and compiler['llvm']['invalidLinkRecovered']
    assert compiler['osl']['nativeMetadataExact'] and compiler['osl']['parallelQueries'] == 128
    assert compiler['osl']['hashPodBytes'] == 8 and compiler['osl']['hashHighBitsPreserved']
    assert compiler['invalidShaderRejected'] and compiler['compilerRecovery']
    for name, expected in compiler['installedDependencyStamps'].items():
        assert digest(base / f'deps/{name}/installed.json') == expected
    assert library['nativeAPIsEqual'] and library['cleanInterpreterShutdown']
    assert library['bindingInstallationSha256'] == digest(base / 'deps/python-bindings/installed.json')
    assert library['result']['version'] == '1.15.3.0'
    source = digest(ROOT / 'web/native/tests/osl-python.py')
    runner = digest(ROOT / 'web/tests/osl-python.cjs')
    verifier = digest(ROOT / 'web/tools/native_osl_python_verify.py')
    fixture = digest(ROOT / 'web/native/tests/fixtures/osl-query/web_query.oso')
    variants, native = [], []
    for suffix, hardware, fallback in (('', False, False), ('-ui', False, True), ('-hardware', True, False)):
        directory = base / ('osl-python-evidence' + suffix)
        report = read(directory / 'acceptance.json')
        verified = read(directory / 'native-verification.json')
        assert report['bundleVersion'] == verified['bundleVersion'] == manifest['version']
        assert report['hardware'] == hardware and report['fallback'] == fallback
        assert report['sourceSha256'] == verified['sourceSha256'] == source
        assert report['runnerSourceSha256'] == verified['runnerSourceSha256'] == runner
        assert verified['verifierSourceSha256'] == verifier
        assert report['fixtureSha256'] == fixture
        assert report['result'] == library['result']
        assert report['offlineReopen'] and not report['browserErrors'] and not report['startupPythonErrors']
        assert verified['nativeAPIsEqual'] and verified['savedBlendVerified'] and verified['unicodeFileReadback']
        assert report['files'] == verified['files']
        for name, entry in report['files'].items():
            artifact = directory / name
            assert artifact.stat().st_size == entry['bytes'] and digest(artifact) == entry['sha256'], name
        variants.append(report)
        native.append(verified)
    recording = read(ROOT / 'web/docs/osl-python-recording.json')
    assert recording['bundleVersion'] == manifest['version'] and recording['sourceSha256'] == source
    for entry in (recording, recording['screenshot']):
        assert digest(ROOT / entry['path']) == entry['sha256']
    return dict(compilers=compiler, libraryChecks=library, browserVariants=variants,
                nativeReadback=native, recording=recording,
                cyclesOSLRenderer=bool(manifest['buildOptions'].get('WITH_CYCLES_OSL', False)))
