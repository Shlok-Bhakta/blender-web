#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently read browser binding exports in the pinned native Blender."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from prepare import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--blender', default='blender')
    args = parser.parse_args()
    directory = args.directory.resolve()
    report = json.loads((directory / 'acceptance.json').read_text())
    source = ROOT / 'web/native/tests/python-bindings.py'
    avif_source = ROOT / 'web/native/tests/avif.py'
    assert report['sourceSha256'] == hashlib.sha256(source.read_bytes()).hexdigest()
    assert report['avifSourceSha256'] == hashlib.sha256(avif_source.read_bytes()).hexdigest()
    for name, entry in report['files'].items():
        data = (directory / name).read_bytes()
        assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'], name
    output = directory / 'native'
    output.mkdir(exist_ok=True)
    script = output / 'verify.py'
    script.write_text('''import bpy, hashlib, json, pathlib, runpy, sys
import numpy as np
import OpenImageIO as oiio
import PyOpenColorIO as ocio
import openvdb
import MaterialX as mx
directory=pathlib.Path(sys.argv[sys.argv.index('--')+1])
source=pathlib.Path(sys.argv[sys.argv.index('--')+2])
avif_source=pathlib.Path(sys.argv[sys.argv.index('--')+3])
report=json.loads((directory/'acceptance.json').read_text())
expected=report['report']
baseline=runpy.run_path(str(source))['run'](directory/'native/reference')
# JSON transports the original vector tuples as lists.
baseline=json.loads(json.dumps(baseline))
assert baseline==expected, 'Native and browser API results differ'
images=[]
for extension in ('exr','tif','png'):
    browser=oiio.ImageBuf(str(directory/('image.'+extension))).get_pixels(oiio.FLOAT)
    native=oiio.ImageBuf(str(directory/'native/reference'/('image.'+extension))).get_pixels(oiio.FLOAT)
    assert np.array_equal(browser,native),extension
    images.append(dict(format=extension,nativeDecodedPixelsExact=True))
grids,metadata=openvdb.readAll(str(directory/'volume.vdb'))
assert metadata['fixture']=='original Python bindings'
assert len(grids)==10
by_name={g.name:g for g in grids}
for item in expected['volume']['gridTypes']:
    value=item['value']
    if isinstance(value,list):value=tuple(value)
    if item['name']=='Int64Grid':value=int(value)
    assert by_name[item['name']].getAccessor().getValue((-1,2,3))==value,item['name']
voxels=np.arange(5*7*9,dtype=np.float32).reshape(5,7,9)/8
restored=np.zeros_like(voxels);by_name['density'].copyToArray(restored)
assert np.array_equal(voxels,restored)
assert by_name['density']['source']=='browser-python'
assert by_name['density'].transform.indexToWorld((2,4,6))==(1.,2.,3.)
config=ocio.Config.CreateFromFile(str(directory/'config.ocio'))
assert list(config.getColorSpaceNames())==list(ocio.Config.CreateRaw().getColorSpaceNames())
document=mx.createDocument();mx.readFromXmlFile(document,str(directory/'material.mtlx'))
assert document.getNode('browser_color').getInputValue('value')==mx.Color3(.25,.5,.75)
assert document.getOutput('browser_output').getConnectedNode().getName()=='browser_color'
shader_files=['color-transform.glsl']+['shader-'+item['target']+'.txt' for item in expected['material']['shaders']]
for name in shader_files:
    assert (directory/name).read_bytes()==(directory/'native/reference'/name).read_bytes(),name
avif_baseline=runpy.run_path(str(avif_source))['run'](directory/'native/avif-reference')
assert avif_baseline['truncatedInputRejected'] and avif_baseline['decoderRecovery']
avif_results=[]
for case in report['avif']['cases']:
    name=case['name'];channels=case['channels'];bits=case['bits']
    reader=oiio.ImageInput.open(str(directory/name));assert reader,oiio.geterror()
    assert reader.spec().get_int_attribute('oiio:BitsPerSample',8)==bits
    decoded=reader.read_image(format=oiio.FLOAT);assert reader.close()
    wasm=np.fromfile(directory/(name+'.f32'),dtype='<f4').reshape(23,37,channels)
    reader_error=float(np.max(np.abs(decoded-wasm)))
    assert reader_error<1e-6,(name,reader_error)
    reference=oiio.ImageBuf(str(directory/'native/avif-reference'/name)).get_pixels(oiio.FLOAT)
    encoder_error=float(np.max(np.abs(decoded-reference)))
    tolerance=1/255+1e-6 if bits==8 else 3/((1<<bits)-1)+1e-6
    assert encoder_error<tolerance,(name,encoder_error,tolerance)
    image=bpy.data.images.load(str(directory/name),check_existing=False)
    blender_pixels=np.array(image.pixels[:],np.float32)
    wasm_pixels=np.fromfile(directory/(name+'.blender.f32'),dtype='<f4')
    blender_error=float(np.max(np.abs(blender_pixels-wasm_pixels)))
    # Preserve native defaults here. A fresh process below checks exact scalar
    # color math, separating the native SSE approximation from codec errors.
    blender_pixels.astype('<f4').tofile(directory/'native'/(name+'.default.f32'))
    bpy.data.images.remove(image)
    avif_results.append(dict(name=name,readerMaxError=reader_error,encoderMaxError=encoder_error,
                             encoderTolerance=tolerance,blenderReaderMaxError=blender_error))
bpy.ops.wm.open_mainfile(filepath=str(directory/'python-bindings.blend'))
saved=json.loads(bpy.data.texts['Browser Python binding proof'].as_string())
assert saved==expected
result=dict(nativeAPIsEqual=True,images=images,volumeGridTypes=9,volumeNumpyExact=True,
            int64Exact=True,volumeMetadata=True,volumeTransform=True,colorConfig=True,
            materialDocument=True,shaderTextExactlyMatchesNative=True,savedBlendVerified=True,
            originalBlenderAVIF=True,avif=avif_results,
            nativeDefaultOptimizationFlags=int(ocio.OPTIMIZATION_DEFAULT),
            nativeScalarOptimizationFlags=int(ocio.OPTIMIZATION_DEFAULT)&~int(ocio.OPTIMIZATION_FAST_LOG_EXP_POW))
(directory/'native/results.json').write_text(json.dumps(result,indent=2)+'\\n')
''')
    native_env = dict(os.environ)
    native_env.pop('OCIO_OPTIMIZATION_FLAGS', None)
    with (output / 'readback.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
            '--python', str(script), '--', str(directory), str(source), str(avif_source)],
            stdout=log, stderr=subprocess.STDOUT, env=native_env, check=True)
    result = json.loads((output / 'results.json').read_text())
    scalar = output / 'scalar-color.py'
    scalar.write_text('''import bpy, hashlib, json, os, sys
from pathlib import Path
import numpy as np
directory=Path(sys.argv[sys.argv.index('--')+1])
report=json.loads((directory/'acceptance.json').read_text())
native=json.loads((directory/'native/results.json').read_text())
assert os.environ['OCIO_OPTIMIZATION_FLAGS']==str(native['nativeScalarOptimizationFlags'])
cases=[]
for case in report['avif']['cases']:
    name=case['name']
    image=bpy.data.images.load(str(directory/name),check_existing=False)
    precise=np.array(image.pixels[:],np.float32)
    bpy.data.images.remove(image)
    wasm=np.fromfile(directory/(name+'.blender.f32'),dtype='<f4')
    default=np.fromfile(directory/'native'/(name+'.default.f32'),dtype='<f4')
    assert np.array_equal(precise,wasm),(name,float(np.max(np.abs(precise-wasm))))
    difference=float(np.max(np.abs(default-precise)))
    original=next(item for item in native['avif'] if item['name']==name)
    assert difference==original['blenderReaderMaxError']
    cases.append(dict(name=name,scalarPixelsExact=True,defaultApproximationMaxError=difference,
                      nativeDefaultPixelsSha256=hashlib.sha256(default.tobytes()).hexdigest()))
result=dict(nativeScalarColorReference=True,disabledOptimization='OPTIMIZATION_FAST_LOG_EXP_POW',
            defaultFlags=native['nativeDefaultOptimizationFlags'],scalarFlags=native['nativeScalarOptimizationFlags'],cases=cases)
(directory/'native/scalar-color.json').write_text(json.dumps(result,indent=2)+'\\n')
''')
    with (output / 'scalar-readback.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
            '--python', str(scalar), '--', str(directory)], stdout=log, stderr=subprocess.STDOUT,
            env=dict(native_env, OCIO_OPTIMIZATION_FLAGS=str(result['nativeScalarOptimizationFlags'])), check=True)
    result['scalarColorReference'] = json.loads((output / 'scalar-color.json').read_text())
    result.update(bundleVersion=report['bundleVersion'], sourceSha256=report['sourceSha256'],
                  avifSourceSha256=report['avifSourceSha256'],
                  verifierSourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  files=report['files'])
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('bundleVersion', 'nativeAPIsEqual', 'volumeGridTypes',
                                                  'int64Exact', 'shaderTextExactlyMatchesNative', 'savedBlendVerified')}))


if __name__ == '__main__':
    main()
