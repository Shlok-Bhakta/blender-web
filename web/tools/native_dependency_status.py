#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare actual desktop/browser build flags, without equating them to feature parity."""
import argparse
import json
from pathlib import Path
import subprocess
from prepare import ROOT
from native_deps import RECIPES

GROUPS = {
    'portableLibraries': {'audaspace','codec_ffmpeg','codec_sndfile','international','libmv','usd'},
    'rendererIntegration': {'cycles_osl'},
    'browserPlatform': {'input_ndof','jack','openal','pulseaudio','xr_openxr'},
}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender',default='blender')
    parser.add_argument('--browser-report',type=Path,default=ROOT/'build/native/image-vector-evidence-hardware/acceptance.json')
    args=parser.parse_args()
    base=ROOT/'build/native'
    target=base/'desktop-build-options.json'
    expression=("import bpy,json,pathlib; pathlib.Path("+repr(str(target))+ ").write_text(json.dumps({"
        "'version':bpy.app.version_string,'revision':bpy.app.build_hash.decode(),"
        "'options':{n:getattr(bpy.app.build_options,n) for n in dir(bpy.app.build_options) "
        "if not n.startswith('_') and isinstance(getattr(bpy.app.build_options,n),bool)}},indent=2))")
    with (base/'desktop-build-options.log').open('w') as log:
        subprocess.run([args.blender,'--background','--factory-startup','--python-exit-code','1',
            '--python-expr',expression],stdout=log,stderr=subprocess.STDOUT,check=True)
    desktop=json.loads(target.read_text())
    browser=json.loads(args.browser_report.read_text())
    manifest=json.loads((base/'dist/manifest.json').read_text())
    assert browser['bundleVersion']==manifest['version']
    assert desktop['version'].split()[0]==manifest['blenderVersion']
    assert manifest['blenderRevision'].startswith(desktop['revision'])
    native=desktop['options'];web=browser['buildOptions']
    assert set(native)==set(web),'Compare the same release API fields'
    missing=sorted(name for name,enabled in native.items() if enabled and not web[name])
    classified=set().union(*GROUPS.values())
    assert not set(missing)-classified, f'Unclassified build gap: {set(missing)-classified}'
    libraries=[]
    for name in RECIPES:
        installed=json.loads((base/'deps'/name.lower()/'installed.json').read_text())
        source=installed['source']
        libraries.append(dict(name=name,version=source['VERSION'],archiveHash=source['HASH'],
                              hashType=source['HASH_TYPE'],source=source['URI']))
    report=dict(bundleVersion=manifest['version'],blenderVersion=desktop['version'],
        desktopRevision=desktop['revision'],desktopBuildOptions=native,browserBuildOptions=web,
        desktopEnabledCount=sum(native.values()),
        matchedEnabledFlags=sorted(name for name,enabled in native.items() if enabled and web[name]),
        missingEnabledFlags=missing,
        remainingGroups={name:sorted(set(missing)&members) for name,members in GROUPS.items()},
        remainingGroupsScope='Only the coarse bpy.app.build_options flags above. An empty portableLibraries group does not establish full dependency parity.',
        disabledLibraryOptions={name:description for name,description in {
            'WITH_DRACO':'Compressed glTF Draco bridge',
            'WITH_MESHOPTIMIZER':'Compressed glTF meshoptimizer bridge',
            'WITH_RUBBERBAND':'Audio time stretching',
            'WITH_OPENIMAGEDENOISE':'Render denoising',
            'WITH_CYCLES_EMBREE':'Cycles Embree acceleration',
            'WITH_CYCLES_PATH_GUIDING':'Cycles OpenPGL path guiding',
            'WITH_CYCLES_OSL':'Cycles Open Shading Language integration',
            'WITH_LLVM':'LLVM compiler infrastructure',
            'WITH_HYDRA':'Hydra rendering integration',
        }.items() if not manifest['buildOptions'].get(name,False)},
        additionalLibraryGaps=['NumPy BLAS acceleration', 'MaterialX Python bindings',
            'HarfBuzz/FriBidi integration into Blender font rendering', 'OpenSubdiv GPU evaluation'],
        baseLibraryRecipes=libraries,
        baseLibraryRecipeScope='The base static-library builder only; excludes CPython/NumPy, shader compilers, bundled Blender code and platform SDKs.',
        browserReportSourceSha256=browser['sourceSha256'],
        limitations=[
            'Build flags are configuration evidence, not proof of every operation or performance parity.',
            'Desktop audio drivers require browser audio integration instead of loading JACK or PulseAudio into Wasm.',
            'Cycles GPU, OpenSubdiv GPU and other subfeatures are not represented by these coarse Python API flags.',
            'Some enabled dependencies have disabled subcomponents; see FEATURES.md for actual operation coverage.'])
    destination=ROOT/'web/docs/dependency-status.json'
    destination.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({key:report[key] for key in ('bundleVersion','desktopEnabledCount','missingEnabledFlags','remainingGroups')}))


if __name__=='__main__':main()
