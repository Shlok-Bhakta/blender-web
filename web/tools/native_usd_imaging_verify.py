#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently read original USD CPU plugin exports in native Blender."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from prepare import ROOT


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--blender', default='blender')
    args = parser.parse_args()
    directory = args.directory.resolve()
    unit = directory / 'unit-report.json'
    report = json.loads(unit.read_text())
    assert report['volumeTypes'] == 4 and not report['gpuRequired']
    for name, expected in report['sourceHashes'].items():
        assert digest(ROOT / name) == expected, name
    names = ['hio-roundtrip.tif', 'hio-written.tif', 'hio-roundtrip.vdb']
    names += [name + suffix for name in ('float', 'double', 'vec3f', 'vec3d')
              for suffix in ('.vdb', '.raw')]
    hashes = {name: digest(directory / name) for name in names}
    output = directory / 'native'
    output.mkdir(exist_ok=True)
    script = output / 'verify.py'
    script.write_text('''import bpy, json, sys
from pathlib import Path
import numpy as np
import OpenImageIO as oiio
import openvdb
from pxr import Plug
root=Path(sys.argv[sys.argv.index('--')+1])
reader=oiio.ImageInput.open(str(root/'hio-roundtrip.tif'));assert reader
image=reader.read_image(format=oiio.FLOAT);assert reader.close()
expected=np.arange(7*5*4,dtype=np.float32).reshape(5,7,4)/256
expected[:,:,3]=1
assert np.array_equal(image,expected)
reader=oiio.ImageInput.open(str(root/'hio-written.tif'));assert reader
written=reader.read_image(format=oiio.FLOAT);assert reader.close()
assert np.array_equal(written,expected)
grids,metadata=openvdb.readAll(str(root/'hio-roundtrip.vdb'))
assert len(grids)==1
grid=grids[0]
assert grid.name=='density' and grid.activeVoxelCount()==2
assert grid.getAccessor().getValue((1,2,3))==1.25
assert grid.getAccessor().getValue((2,2,3))==2.5
assert grid.transform.indexToWorld((2,4,6))==(1.,2.,3.)
for name,dtype,expected in (
    ('float','<f4',[1.25,2.5]),('double','<f8',[1.25,2.5]),
    ('vec3f','<f4',[[1.25,2.5,3.75],[-1.,-2.,-3.]]),
    ('vec3d','<f8',[[1.25,2.5,3.75],[-1.,-2.,-3.]])):
    grids,metadata=openvdb.readAll(str(root/(name+'.vdb')))
    assert len(grids)==1 and grids[0].name==name
    grid=grids[0]
    assert grid.activeVoxelCount()==2
    assert grid.transform.indexToWorld((4,8,12))==(1.,2.,3.)
    raw=np.fromfile(root/(name+'.raw'),dtype=dtype).reshape(np.shape(expected))
    assert np.array_equal(raw,np.array(expected,dtype=dtype))
    for coord,value in zip(((-2,3,4),(-1,3,4)),expected):
        assert np.array_equal(grid.getAccessor().getValue(coord),value)
plugins={}
for name in ('hioOiio','hioOpenVDB'):
    plugin=Plug.Registry().GetPluginWithName(name)
    assert plugin and plugin.Load() and plugin.isLoaded,name
    plugins[name]=True
result=dict(nativePixelsExact=True,nativeHioWrittenPixelsExact=True,nativeVoxelsExact=True,
            nativeTransformExact=True,volumeTypes=4,nativePlugins=plugins,
            blenderVersion=bpy.app.version_string,blenderRevision=bpy.app.build_hash.decode())
(root/'native/results.json').write_text(json.dumps(result,indent=2)+'\\n')
''')
    with (output / 'readback.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
            '--python', str(script), '--', str(directory)], stdout=log, stderr=subprocess.STDOUT, check=True)
    assert hashes == {name: digest(directory / name) for name in names}
    result = json.loads((output / 'results.json').read_text())
    pins = json.loads((ROOT / 'web/upstream.json').read_text())
    assert result['blenderVersion'].split()[0] == pins['blenderVersion']
    assert pins['blenderRevision'].startswith(result['blenderRevision'])
    result.update(files=hashes, unitReportSha256=digest(unit),
                  verifierSourceSha256=digest(Path(__file__)))
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in
                     ('nativePixelsExact', 'nativeVoxelsExact', 'nativeTransformExact', 'volumeTypes')}))


if __name__ == '__main__':
    main()
