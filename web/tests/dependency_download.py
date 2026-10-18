#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Dependency mirrors must preserve the release's archive checksum."""
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from native_deps import download


class DependencyDownload(unittest.TestCase):
    payload = b'pinned source archive'
    entry = {'FILE':'gmp-fixture.tar.xz', 'URI':'https://gmplib.org/download/gmp/gmp-fixture.tar.xz',
             'HASH_TYPE':'MD5', 'HASH':hashlib.md5(payload).hexdigest()}

    def test_bad_primary_response_uses_identical_mirror_and_then_cache(self):
        with tempfile.TemporaryDirectory() as name:
            with patch('urllib.request.urlopen', side_effect=[io.BytesIO(b'<html>bot challenge</html>'), io.BytesIO(self.payload)]) as request:
                archive = download(self.entry, Path(name))
                self.assertEqual(archive.read_bytes(), self.payload)
                self.assertEqual(request.call_args_list[1].args[0], 'https://ftp.gnu.org/gnu/gmp/gmp-fixture.tar.xz')
            with patch('urllib.request.urlopen', side_effect=AssertionError('cached download used network')):
                self.assertEqual(download(self.entry, Path(name)), archive)
            self.assertFalse(list(Path(name).glob('*.partial')))

    def test_http_error_also_uses_verified_mirror(self):
        error = urllib.error.HTTPError(self.entry['URI'], 503, 'Unavailable', {}, None)
        with tempfile.TemporaryDirectory() as name:
            with patch('urllib.request.urlopen', side_effect=[error, io.BytesIO(self.payload)]):
                self.assertEqual(download(self.entry, Path(name)).read_bytes(), self.payload)

    def test_no_source_may_bypass_checksum(self):
        with tempfile.TemporaryDirectory() as name:
            with patch('urllib.request.urlopen', side_effect=[io.BytesIO(b'wrong primary'), io.BytesIO(b'wrong mirror')]):
                with self.assertRaisesRegex(RuntimeError, 'Checksum mismatch'):
                    download(self.entry, Path(name))
            self.assertEqual(list(Path(name).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
