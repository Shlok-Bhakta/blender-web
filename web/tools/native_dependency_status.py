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
    from native_python_acceptance import validate as validate_python
    python_proof = validate_python(base, manifest)
    from native_ctypes_acceptance import validate as validate_ctypes
    ctypes_proof = validate_ctypes(base, manifest)
    foundation = python_proof['libraryChecks']['foundation']
    from native_bindings_acceptance import validate as validate_bindings
    bindings_proof = validate_bindings(base, manifest)
    from native_osl_python_acceptance import validate as validate_osl_python
    osl_python = validate_osl_python(base, manifest)
    from native_usd_imaging_acceptance import validate as validate_usd_imaging
    usd_imaging = validate_usd_imaging(base, manifest)
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
        additionalLibraryGaps=[
            'External ctypes libraries, general multiprocessing and raw socket transport',
            'Cython native accelerators and arbitrary dynamic Wasm extension loading',
            'MaterialX OpenGL rendering and OSL rendering compiler/runtime integration',
            'Hydra GPU rendering and browser graphics backends',
            'OpenSubdiv GPU evaluation'],
        componentAudit='web/docs/dependency-components.md',
        pythonFoundation=dict(
            verifiedBundle=manifest['version'],
            modules=['_sqlite3','_lzma','_ssl','_hashlib','zstandard.backend_c'],
            packages=foundation['packages'],
            browserVariants=len(python_proof['browserVariants']),
            independentNativeReadbacks=len(python_proof['nativeReadback']),
            originalConditionalDownloader=True, originalBackgroundDownloader=True,
            networkStreaming=False,
            evidence='web/docs/python-foundation.acceptance.json'),
        pythonBindings=dict(
            verifiedBundle=manifest['version'],
            versions=bindings_proof['libraryChecks']['versions'], modules=len(json.loads(
                (base/'deps/python-bindings/installed.json').read_text())['modules']),
            materialXGenerators=['GLSL', 'MSL', 'OSL', 'MDL'],
            openVDBGridTypes=9, openVDBAX=False, avifDepths=[8,10,12],
            browserVariants=len(bindings_proof['browserVariants']),
            independentNativeReadbacks=len(bindings_proof['nativeReadback']),
            evidence='web/docs/python-bindings.acceptance.json'),
        oslPythonQueries=dict(verifiedBundle=manifest['version'],
            version=osl_python['libraryChecks']['result']['version'],
            browserVariants=len(osl_python['browserVariants']),
            independentNativeReadbacks=len(osl_python['nativeReadback']),
            compilerLibrariesInstalled=True, originalPythonModule=True,
            cyclesOSLRenderer=osl_python['cyclesOSLRenderer'],
            evidence='web/docs/osl-python.acceptance.json'),
        pythonCtypes=dict(verifiedBundle=manifest['version'], libffiVersion='3.5.2',
            originalModule=True, pythonNamespaceSymbols=ctypes_proof['pythonNamespaceSymbols'],
            browserVariants=len(ctypes_proof['browserVariants']),
            independentNativeReadbacks=len(ctypes_proof['nativeReadback']),
            externalDynamicLibraries=False, sparseHighAddressCheck=True,
            evidence='web/docs/python-ctypes.acceptance.json'),
        usdCPUImaging=dict(verifiedBundle=manifest['version'],
            plugins=usd_imaging['installation']['components'], volumeFormats=4,
            standaloneOriginalAPIs=True, independentNativeReadback=True,
            browserRegistries=len(usd_imaging['browserRegistries']), gpuRendering=False,
            evidence='web/docs/usd-cpu-imaging.acceptance.json'),
        optimizationWork=['NumPy explicit Wasm SIMD kernels', 'OpenColorIO SIMD kernels'],
        separateEnhancements=['HarfBuzz/FriBidi integration into Blender font rendering'],
        baseLibraryRecipes=libraries,
        baseLibraryRecipeScope='The base static-library builder only; includes LLVM/OSL but excludes CPython/NumPy, WebGPU shader translators, separately built imaging components, bundled Blender code and platform SDKs.',
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
