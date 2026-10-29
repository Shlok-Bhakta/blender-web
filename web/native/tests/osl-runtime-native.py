# SPDX-License-Identifier: GPL-2.0-or-later
"""Create the independently compiled native shader for the runtime comparison."""
import json
from pathlib import Path
import sys

import _cycles
import bpy
import oslquery

root = Path(__file__).resolve().parents[3]
pins = json.loads((root / 'web/upstream.json').read_text())
assert bpy.app.build_hash.decode() == pins['blenderRevision'][:12]
assert oslquery.VERSION_STRING == '1.15.3.0'
source, directory = map(Path, sys.argv[sys.argv.index('--') + 1:])
directory.mkdir(parents=True, exist_ok=True)
assert _cycles.osl_compile(str(source), str(directory / 'web_execute.oso'))
(directory / 'native.json').write_text(json.dumps(dict(
    blenderRevision=bpy.app.build_hash.decode(), oslVersion=oslquery.VERSION_STRING), indent=2) + '\n')
