#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require numerical, browser, native readback and recording evidence for these ports."""
import hashlib
import json
from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(base, manifest):
    version=manifest['version']
    read=lambda path:json.loads(path.read_text())
    libraries={name:read(base/f'{name}-check.json') for name in ('openpgl','oidn','ispc','hevc')}
    for report in libraries.values():
        assert report['sourceHashes']
        for path,expected in report['sourceHashes'].items():assert digest(ROOT/path)==expected,path
    assert libraries['oidn']['version']==20500
    assert libraries['oidn']['invalidSizes'] and libraries['oidn']['cancellationRecovery']
    assert libraries['oidn']['halfMSE']<.001
    assert len(libraries['oidn']['cases'])==2
    assert all(case['outputMSE']<case['inputMSE']*.75 for case in libraries['oidn']['cases'])
    assert [model['kind'] for model in libraries['openpgl']['models']]==[0,1,2]
    assert all(model['frontPdf']>model['backPdf'] and model['meanZ']>.7 for model in libraries['openpgl']['models'])
    assert libraries['ispc']['halfConversions']>=300000 and libraries['ispc']['halfComparisons']>=65536
    assert libraries['ispc']['packedMaskComparisons']==786432
    assert [case['depth'] for case in libraries['hevc']['bitDepths']]==[8,10,12]
    assert all(case['losslessExact'] and case['frames']==3 for case in libraries['hevc']['bitDepths'])
    standalone=read(base/'hevc-check-native-verification.json')
    for entry in standalone['exports']:
        assert entry['losslessExact']
        assert digest(base/f'hevc-check-{entry["depth"]}.hevc')==entry['sha256']
    baseline=read(base/'render-libraries-baseline.json')
    for path,expected in baseline['sourceHashes'].items():assert digest(ROOT/path)==expected,path
    assert len(baseline['oidnImages'])==2 and all(item['maxAbsError']<1e-5 for item in baseline['oidnImages'])
    source=digest(ROOT/'web/tests/render-libraries.py')
    runner=digest(ROOT/'web/tests/render-libraries.cjs')
    verifier=digest(ROOT/'web/tools/native_render_libraries_verify.py')
    assert manifest['buildOptions']['WITH_CYCLES_PATH_GUIDING'] and manifest['buildOptions']['WITH_OPENIMAGEDENOISE']
    variants=[];native=[]
    for suffix in ('','-ui','-hardware'):
        directory=base/('render-libraries-evidence'+suffix)
        report=read(directory/'acceptance.json');verified=read(directory/'native-verification.json')
        assert report['bundleVersion']==verified['bundleVersion']==version
        assert report['sourceSha256']==verified['sourceSha256']==source
        assert report['runnerSourceSha256']==runner and verified['verifierSourceSha256']==verifier
        assert report['offlineReopen'] and not report['browserErrors'] and report['pathGuiding'] and report['embree']
        assert verified['savedBlendVerified']
        assert digest(directory/'render-libraries.blend')==verified['blendSha256']==report['blend']['sha256']
        for name,expected in report['fixtureHashes'].items():
            path=ROOT/'web/tests/render-libraries.py' if name=='check.py' else ROOT/'web/tests/fixtures/render-libraries'/name
            assert digest(path)==expected,name
        for name,image in verified['images'].items():
            guided=name in ('guided','volume-guided')
            assert image['meanAbsoluteError']<(4 if guided else 1.5)
            assert image['outlierFraction']<(.035 if guided else .005)
        if not report['fallback']:
            assert set(report['images'])=={name+'.png' for name in ('noisy','denoised','guided','volume-guided','compositor')}
            for name,image in report['images'].items():assert digest(directory/name)==image['sha256']
            guiding=report['guiding']
            assert len(guiding['fieldUpdates'])>=8 and all(value==1 for value in guiding['fieldUpdates'])
            assert max(guiding['surfaceSamples'])>1024 and max(guiding['volumeSamples'])>1024
            assert set(report['hevc'])==set(verified['hevcExports'])=={'8','10','12'}
            for depth,video in verified['hevcExports'].items():
                assert video['maximumError']<=1 and video['meanAbsoluteError']<.02
                assert digest(directory/f'hevc-{depth}.mkv')==video['sha256']==report['hevc'][depth]['sha256']
        variants.append(report);native.append(verified)
    recording=read(ROOT/'web/docs/render-libraries-recording.json')
    assert recording['bundleVersion']==version and recording['sourceSha256']==source
    for entry in (recording,recording['screenshot']):assert digest(ROOT/entry['path'])==entry['sha256']
    return dict(libraries=libraries,nativeBaseline=baseline,standaloneHEVC=standalone,
                browserVariants=variants,nativeReadback=native,recording=recording)
