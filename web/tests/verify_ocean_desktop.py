# SPDX-License-Identifier: GPL-2.0-or-later
"""Open a browser-exported Alembic in the pinned desktop Blender release.

blender -b --factory-startup --python-exit-code 1 --python \
  web/tests/verify_ocean_desktop.py -- build/native/ocean-evidence-hardware
"""
import bpy
import hashlib
import json
import runpy
import sys
from pathlib import Path

directory = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
checks = runpy.run_path(str(Path(__file__).with_name('ocean_alembic.py')), run_name='checks')
obj = checks['import_cache'](directory / 'browser-ocean.abc', 'NativeOcean')
maximum = checks['compare'](checks['samples'](obj), json.loads((directory / 'samples.json').read_text()))
report = dict(blenderVersion=bpy.app.version_string,
              blenderRevision=bpy.app.build_hash.decode(),
              maxCoordinateError=maximum, frames=list(checks['FRAMES']),
              browserArchiveSHA256=hashlib.sha256((directory / 'browser-ocean.abc').read_bytes()).hexdigest())
(directory / 'desktop-verification.json').write_text(json.dumps(report, indent=2) + '\n')
print('OCEAN_DESKTOP_VERIFIED:' + json.dumps(report))
