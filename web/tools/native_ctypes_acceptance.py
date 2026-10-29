#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require ctypes, worker lifetime, high-address and native comparison evidence."""
import hashlib
import json
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(base, manifest):
    read = lambda path: json.loads(path.read_text())
    unit = read(base / 'python-check.json')
    for name, expected in unit['sourceHashes'].items():
        assert digest(ROOT / name) == expected, name
    library = unit['ctypes']
    assert library['reusedWorkerCallbacks'] == 240 and library['pythonThreadCallbacks'] == 8
    assert library['existingWorkers'] == 4 and library['signatureReuseIterations'] == 60
    assert not library['unraisableErrors']
    installed = read(base / 'deps/python-extensions/installed.json')
    assert '_ctypes' in installed['modules']
    assert installed['ctypesPythonSymbols'] >= 1200 and installed['ctypesLibcSymbols'] == 9
    ffi = read(base / 'deps/ffi/installed.json')
    assert ffi['source']['VERSION'] == '3.5.2'
    assert ffi['patch'] == digest(ROOT / 'web/native/patches/ffi.patch')
    assert ffi['ffiBuilder'] == digest(ROOT / 'web/tools/native_ffi.py')
    browser_ffi = read(base / 'ffi-browser-evidence/acceptance.json')
    assert browser_ffi['crossOriginIsolated']
    for name in ('ffi-workers', 'ffi-high-address'):
        proof = read(base / (name + '-check.json'))
        for path, expected in proof['sourceHashes'].items():
            assert digest(ROOT / path) == expected, path
        browser = browser_ffi['results'][name + '-check']
        assert browser['wasmSha256'] == digest(base / f'checks/{name}-check.wasm')
        assert browser['jsSha256'] == digest(base / f'checks/{name}-check.js')
        assert not browser['browserErrors']
        assert browser['result'] == {key: value for key, value in proof.items() if key != 'sourceHashes'}
    workers = browser_ffi['results']['ffi-workers-check']['result']
    assert workers['calls'] == 9341 and workers['signatureReuseIterations'] == 1000
    assert workers['nestedRetirement'] and workers['threadExitDuringCallback']
    high = browser_ffi['results']['ffi-high-address-check']['result']
    assert high['heapBytes'] == 2418016256 and high['highStack'] and high['aggregateCallbacks']
    source = digest(ROOT / 'web/native/tests/python-ctypes.py')
    runner = digest(ROOT / 'web/tests/python-ctypes.cjs')
    verifier = digest(ROOT / 'web/tools/native_ctypes_verify.py')
    variants, native = [], []
    for suffix, hardware, fallback in (('', False, False), ('-ui', False, True), ('-hardware', True, False)):
        directory = base / ('python-ctypes-evidence' + suffix)
        report = read(directory / 'acceptance.json')
        verified = read(directory / 'native-verification.json')
        assert report['bundleVersion'] == verified['bundleVersion'] == manifest['version']
        assert report['hardware'] == hardware and report['fallback'] == fallback
        assert report['sourceSha256'] == verified['sourceSha256'] == source
        assert report['runnerSourceSha256'] == runner and verified['verifierSourceSha256'] == verifier
        assert report['offlineReopen'] and report['offlineRepeatedChecks']
        assert not report['browserErrors'] and not report['startupPythonErrors']
        assert report['report'] == library
        assert verified['savedBlendVerified'] and verified['exactFunctionalResults']
        assert verified['originalNativeModule'] and verified['libffiVersion'] == '3.5.2'
        assert verified['architectureDifferences'] == ['longBytes', 'pointerBytes']
        assert manifest['blenderRevision'].startswith(verified['blenderRevision'])
        assert report['blend'] == verified['blend']
        assert digest(directory / 'ctypes.blend') == report['blend']['sha256']
        variants.append(report)
        native.append(verified)
    recording = read(ROOT / 'web/docs/python-ctypes-recording.json')
    assert recording['bundleVersion'] == manifest['version'] and recording['sourceSha256'] == source
    for entry in (recording, recording['screenshot']):
        assert digest(ROOT / entry['path']) == entry['sha256']
    return dict(libraryChecks=library, originalSourceHashes=installed['ctypesSourceHashes'],
                pythonNamespaceSymbols=installed['ctypesPythonSymbols'],
                libffi=ffi, workerBrowserChecks=browser_ffi, browserVariants=variants,
                nativeReadback=native, recording=recording,
                limitations=['External dynamic libraries and arbitrary dynamic Wasm extensions are unsupported.',
                             'The sparse high-address test does not establish multi-gigabyte scene capacity.',
                             'The application remains wasm32 with a 4 GiB heap limit.'])
