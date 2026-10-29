#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check candidate checkpoint isolation without launching Blender or a browser."""
import contextlib
import io
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import native_validate


class ValidationCheckpoint(unittest.TestCase):
    def test_new_candidate_cannot_reuse_previous_status_when_stopped(self):
        first_stage = subprocess.check_output(
            [sys.executable, native_validate.__file__, '--list'], text=True
        ).splitlines()[0].split(':', 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / 'build/native'
            link = base / 'editor/source/creator/CMakeFiles/blender.dir/link.txt'
            link.parent.mkdir(parents=True)
            link.write_text('em++ -O3')
            (base / 'dist').mkdir()
            manifest = dict(version='new-candidate', uiFallback=True)
            (base / 'dist/manifest.json').write_text(json.dumps(manifest))
            checkpoint = base / 'validation-candidate.json'
            checkpoint.write_text(json.dumps(dict(version='old-candidate')))
            status = base / 'validation-status.json'
            status.write_text(json.dumps([
                dict(name=first_stage, exitCode=0, seconds=1)]))
            arguments = ['native_validate.py', '--blender', sys.executable,
                         '--stop-before', first_stage]
            with patch.object(native_validate, 'ROOT', root), \
                    patch.object(sys, 'argv', arguments), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as stopped:
                    native_validate.main()
            self.assertEqual(stopped.exception.code, 0)
            self.assertEqual(json.loads(status.read_text()), [])
            self.assertEqual(json.loads(checkpoint.read_text()), manifest)
            arguments = ['native_validate.py', '--blender', sys.executable,
                         '--resume-from', first_stage]
            with patch.object(native_validate, 'ROOT', root), \
                    patch.object(sys, 'argv', arguments):
                with self.assertRaisesRegex(AssertionError, 'previously recorded stage'):
                    native_validate.main()

    def test_unknown_stop_does_not_change_existing_checkpoint(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / 'build/native'
            base.mkdir(parents=True)
            status = base / 'validation-status.json'
            status.write_text('[{"name":"previous","exitCode":0}]\n')
            previous = status.read_bytes()
            with patch.object(native_validate, 'ROOT', root), \
                    patch.object(sys, 'argv', ['native_validate.py', '--stop-before', 'typo']), \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as invalid:
                    native_validate.main()
            self.assertEqual(invalid.exception.code, 2)
            self.assertEqual(status.read_bytes(), previous)
            self.assertFalse((base / 'validation-candidate.json').exists())


if __name__ == '__main__':
    unittest.main()
