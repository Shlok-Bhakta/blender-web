#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce native Flex header poisoning and compile its isolated output to Wasm."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'web/tools'))
from native_osl import isolate_flex


def main():
    flex = os.environ.get('BLENDER_WEB_FLEX') or shutil.which('flex')
    assert flex and shutil.which('em++'), 'Activate the SDK and provide a real native Flex executable'
    flex = Path(flex).resolve()
    pins = __import__('json').loads((ROOT / 'web/upstream.json').read_text())
    assert subprocess.check_output(['emcc', '-dumpversion'], text=True).strip() == pins['emscriptenVersion']
    with tempfile.TemporaryDirectory(prefix='blender-osl-parser-') as temporary:
        work = Path(temporary)
        native = work / 'native/usr/bin/flex'
        native.parent.mkdir(parents=True)
        native.write_text('#!/bin/sh\nexec ' + shlex.quote(str(flex)) + ' "$@"\n')
        native.chmod(0o755)
        # Model OSL's real /usr/bin/flex -> /usr/include inference.
        (native.parent.parent / 'include').symlink_to('/usr/include', target_is_directory=True)
        lexer = work / 'check.l'
        lexer.write_text('%option noyywrap\n%%\n[0-9]+ return 1;\n. return 2;\n%%\n')
        generated = work / 'check.cc'
        isolated = isolate_flex(native, work / 'isolated')
        expected = subprocess.check_output([flex, '--version'], text=True)
        assert subprocess.check_output([isolated, '--version'], text=True) == expected
        subprocess.run([isolated, '-o', generated, lexer], check=True)
        standard = work / 'standard.hh'
        standard.write_text('#include <algorithm>\n#include <cstdint>\n#include <locale>\n#include <string>\n')
        common = ['em++', '-std=c++17', '-pthread', '-fwasm-exceptions', '-include', standard,
                  '-c', generated, '-o', work / 'check.o']
        poisoned = subprocess.run([*common, '-I' + str(native.parent.parent / 'include')], capture_output=True, text=True)
        assert poisoned.returncode, 'The native include path must reproduce the CI failure'
        assert 'not found' in poisoned.stderr or 'unsupported' in poisoned.stderr, poisoned.stderr
        subprocess.run([*common, '-I' + str(isolated.parent.parent / 'include')], check=True)
        readobj = Path(shutil.which('emcc')).resolve().parent.parent / 'bin/llvm-readobj'
        header = subprocess.check_output([readobj, '--file-headers', work / 'check.o'], text=True)
        assert 'Format: WASM' in header and 'Arch: wasm32' in header
        print('Native include poisoning reproduced; real Flex output compiles with isolated SDK headers')


if __name__ == '__main__':
    main()
