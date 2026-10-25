#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare the same OpenPGL/OIDN checks with pinned native Blender libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tarfile
import subprocess
from prepare import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender',default='blender')
    args=parser.parse_args();base=ROOT/'build/native';prefix=base/'deps/prefix'
    work=base/'render-libraries-baseline';work.mkdir(exist_ok=True)
    executable=shutil.which(args.blender)
    if not executable:parser.error('Provide pinned native Blender with --blender')
    lib=Path(executable).resolve().parent/'lib'
    tbb=lib/'libtbb.so.12.17';embree=lib/'libembree4.so.4'
    assert tbb.is_file() and embree.is_file(),'Native baseline requires the pinned Linux Blender libraries'
    # Blender statically links OpenPGL. Build its pinned source against the same
    # native TBB used by Blender; keep CPU-4 fixed for both numerical checks.
    config=work/'tbb';config.mkdir(exist_ok=True)
    (config/'TBBConfig.cmake').write_text('set(TBB_FOUND TRUE)\nadd_library(TBB::tbb SHARED IMPORTED)\n'
        f'set_target_properties(TBB::tbb PROPERTIES IMPORTED_LOCATION "{tbb}" '
        f'INTERFACE_INCLUDE_DIRECTORIES "{prefix / "include"}")\n')
    # Native code comes from a separately verified, unmodified archive.
    from native_deps import download
    entry=json.loads((base/'deps/openpgl/installed.json').read_text())['source']
    archive=download(entry,base/'deps/downloads')
    source=work/'source'
    if not source.is_dir():
        temporary=work/'extract';temporary.mkdir(exist_ok=True)
        with tarfile.open(archive) as bundle:bundle.extractall(temporary,filter='data')
        roots=list(temporary.iterdir());assert len(roots)==1
        roots[0].rename(source);temporary.rmdir()
    pgl=work/'openpgl-native'
    subprocess.run(['cmake','-S',str(source),'-B',str(pgl),
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON',f'-DTBB_DIR={config}',
        '-DOPENPGL_BUILD_STATIC=ON','-DOPENPGL_BUILD_TOOLS=OFF','-DOPENPGL_BUILD_CHECK_TOOL=OFF',
        '-DOPENPGL_ISA_SSE4=ON','-DOPENPGL_ISA_AVX2=OFF','-DOPENPGL_ISA_AVX512=OFF'],check=True)
    subprocess.run(['cmake','--build',str(pgl),'--parallel','4'],check=True)
    checks={
        'openpgl':['-I'+str(source/'openpgl/include'),
            '-I'+str(pgl/'openpgl/include/openpgl'),str(pgl/'openpgl/libopenpgl.a'),str(embree),str(tbb)],
        'oidn':['-I'+str(prefix/'include'),'-L'+str(lib),'-lOpenImageDenoise'],
    }
    reports={}
    for name,flags in checks.items():
        binary=work/(name+'-check')
        subprocess.run(['g++','-O2','-UNDEBUG','-I'+str(prefix/'include'),
            str(ROOT/f'web/native/tests/{name}.cc'),*flags,'-pthread',f'-Wl,-rpath,{lib}',
            '-o',str(binary)],check=True)
        command=[str(binary)]+([str(work/'native')] if name=='oidn' else [])
        report=json.loads(subprocess.check_output(command,text=True,timeout=60))
        reports[name]={'native':report,'wasm':json.loads((base/f'{name}-check.json').read_text())}
    comparisons=[]
    for index in (0,1):
        a=(base/f'oidn-check-{index}.f32').read_bytes();b=(work/f'native-{index}.f32').read_bytes()
        assert len(a)==len(b)==61*45*3*4
        af=struct.unpack('<'+str(len(a)//4)+'f',a);bf=struct.unpack('<'+str(len(b)//4)+'f',b)
        errors=[abs(x-y) for x,y in zip(af,bf)]
        result=dict(guided=bool(index),maxAbsError=max(errors),meanAbsError=sum(errors)/len(errors))
        assert result['maxAbsError']<.00001,result;comparisons.append(result)
    for a,b in zip(reports['openpgl']['native']['models'],reports['openpgl']['wasm']['models']):
        assert a['kind']==b['kind']
        for name in ('frontPdf','backPdf','meanZ'):
            assert abs(a[name]-b[name])<=.001*max(1,abs(a[name])),(name,a,b)
    sourceHashes={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest()
        for path in [Path(__file__),ROOT/'web/native/tests/oidn.cc',ROOT/'web/native/tests/openpgl.cc']}
    result=dict(oidnImages=comparisons,libraries=reports,sourceHashes=sourceHashes,
                scope='Library numerical checks only. Timings include background load and are not performance benchmarks.')
    (base/'render-libraries-baseline.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'oidnImages':comparisons,'openpglModels':3}))


if __name__=='__main__':main()
