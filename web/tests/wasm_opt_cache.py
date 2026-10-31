#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check Binaryen result reuse with a stand-in wasm-opt, without Emscripten."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import native_wasm_opt_cache as cache_module

# Appends a marker to the module and counts real optimizer runs.
TOOL = '''#!/bin/sh
echo run >> "$(dirname "$0")/runs"
while [ "$#" -gt 0 ]; do
  case "$1" in -o) out=$2; shift 2;; -*) shift;; *) in=$1; shift;; esac
done
cat "$in" > "$out.tmp" && printf '%s' "optimized" >> "$out.tmp" && mv "$out.tmp" "$out"
echo "metadata for $out"
'''


class WasmOptCache(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.tool = self.root / 'binaryen/wasm-opt'
        self.tool.parent.mkdir()
        self.tool.write_text(TOOL)
        self.tool.chmod(0o755)
        self.cache = self.root / 'cache'
        self.cache.mkdir()

    def tearDown(self):
        self.directory.cleanup()

    def module(self, name, size=cache_module.MINIMUM_BYTES, marker=b'a'):
        path = self.root / name
        with open(path, 'wb') as stream:
            stream.truncate(size - 1)
            stream.seek(size - 1)
            stream.write(marker)
        return path

    def optimize(self, module, *args):
        output = module.with_suffix('.out')
        result = subprocess.run([sys.executable, cache_module.__file__, str(self.tool), str(self.cache),
                                 *args, str(module), '-o', str(output), '--enable-threads'],
                                capture_output=True, check=True)
        return output, result.stdout

    def runs(self):
        path = self.tool.parent / 'runs'
        return len(path.read_text().splitlines()) if path.exists() else 0

    def entries(self):
        return sorted(path.name for path in self.cache.iterdir() if path.is_dir())

    def test_identical_link_reuses_result_and_stdout(self):
        first, first_stdout = self.optimize(self.module('render.wasm'), '-O3')
        second, second_stdout = self.optimize(self.module('ui.wasm'), '-O3')
        self.assertEqual(self.runs(), 1)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(first_stdout, second_stdout)

    def test_different_module_or_arguments_run_again(self):
        self.optimize(self.module('render.wasm'), '-O3')
        self.optimize(self.module('changed.wasm', marker=b'b'), '-O3')
        self.optimize(self.module('render.wasm'), '-O2')
        self.assertEqual(self.runs(), 3)
        self.assertEqual(len(self.entries()), cache_module.KEPT_ENTRIES)

    def test_binaryen_environment_is_part_of_the_key(self):
        self.optimize(self.module('render.wasm'), '-O3')
        os.environ['BINARYEN_PASS_DEBUG'] = '1'
        try:
            self.optimize(self.module('render.wasm'), '-O3')
        finally:
            del os.environ['BINARYEN_PASS_DEBUG']
        self.assertEqual(self.runs(), 2)

    def test_small_modules_and_source_maps_are_not_stored(self):
        self.optimize(self.module('tool.wasm', size=1024), '-O3')
        self.optimize(self.module('mapped.wasm'), '-O3', '--output-source-map=mapped.map')
        self.assertEqual(self.entries(), [])
        self.assertEqual(self.runs(), 2)


if __name__ == '__main__':
    unittest.main()
