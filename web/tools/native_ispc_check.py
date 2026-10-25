#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise generic ISPC tails, binary16 boundaries, NaNs, and packed masks."""
import argparse
import json
from pathlib import Path
import subprocess
from native_ispc import lower_masks
from prepare import ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base',type=Path,default=ROOT/'build/native')
    args=parser.parse_args()
    base=args.base.resolve();work=base/'ispc-check';work.mkdir(exist_ok=True)
    prefix=base/'deps/prefix'
    if not (base/'deps/oidn/ispc-wasm').is_file():
        from native_oidn import configure
        from native_deps import download
        configure(base/'deps', download, prefix)
    fixture=ROOT/'web/native/tests/ispc-half.ispc'
    subprocess.run([str(base/'deps/oidn/ispc-wasm'),'--arch=x86','--addressing=32',
        '--target=generic-i32x8','-O2','-h',str(work/'ispc-half.h'),
        '-o',str(work/'ispc-half.o'),str(fixture)],check=True)
    functions=[]
    for width in (4,8,16):
        for op in ('eq','ne'):
            functions.append(f'''define i32 @mask_{op}{width}(i32 %a, i32 %b) {{
  %av = trunc i32 %a to i{width}
  %bv = trunc i32 %b to i{width}
  %mask = icmp {op} i{width} %av, %bv
  %result = zext i1 %mask to i32
  ret i32 %result
}}
''')
    llvm=work/'masks.ll';llvm.write_text(lower_masks('\n'.join(functions)))
    subprocess.run(['emcc','-O3','-pthread','-msimd128','-fwasm-exceptions','-c',str(llvm),'-o',str(work/'masks.o')],check=True)
    executable=work/'check.js'
    subprocess.run(['em++','-O2','-UNDEBUG','-pthread','-msimd128','-fwasm-exceptions',
        '-I'+str(work),'-I'+str(prefix/'include'),str(ROOT/'web/native/tests/ispc-half.cc'),
        str(work/'ispc-half.o'),str(work/'masks.o'),str(ROOT/'web/native/oidn/half.cpp'),
        '-sENVIRONMENT=node','-sEXIT_RUNTIME=1','-sALLOW_MEMORY_GROWTH=1',
        '-o',str(executable)],check=True)
    output=subprocess.check_output(['node',str(executable)],text=True,timeout=60)
    report=json.loads(output)
    import hashlib
    from native_oidn import INPUTS
    paths=[*INPUTS,'web/tools/native_ispc_check.py','web/native/tests/ispc-half.cc','web/native/tests/ispc-half.ispc']
    report['sourceHashes']={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in paths}
    (base/'ispc-check.json').write_text(json.dumps(report,indent=2)+'\n')
    print(output,end='')


if __name__=='__main__':main()
