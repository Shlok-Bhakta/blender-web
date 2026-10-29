# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile and query the shader fixture using the pinned native Blender."""
import hashlib
import importlib
import json
from pathlib import Path
import sys

import _cycles
import bpy
import oslquery

root = Path(__file__).resolve().parents[3]
base = Path(sys.argv[sys.argv.index('--') + 1]).resolve()
pins = json.loads((root / 'web/upstream.json').read_text())
assert bpy.app.build_hash.decode() == pins['blenderRevision'][:12]
assert oslquery.VERSION_STRING == '1.15.3.0'
assert _cycles.osl_compile(str(base / 'web_query.osl'), str(base / 'web_query.oso'))
query = oslquery.OSLQuery(str(base / 'web_query.oso'))
assert not query.geterror() and query.nparams == 9


def parameter(p):
    return dict(name=p.name, type=str(p.type), output=p.isoutput,
                variableArray=p.varlenarray, structure=p.isstruct, closure=p.isclosure,
                value=p.value, metadata=[parameter(m) for m in p.metadata])


result = dict(name=query.shadername(), type=query.shadertype(),
              parameters=[parameter(p) for p in query.parameters],
              metadata=[parameter(p) for p in query.metadata])
(base / 'native-query.json').write_text(json.dumps(result, indent=2) + '\n')
failed = oslquery.OSLQuery()
invalid = failed.open_bytecode('not valid OSL bytecode\n')
invalid_error = failed.geterror()
missing = failed.open('missing-query-fixture', str(base))
missing_error = failed.geterror()
recovered = failed.open_bytecode((base / 'web_query.oso').read_text())
assert not invalid and not missing and not invalid_error and missing_error and recovered
module = Path(importlib.import_module('oslquery.oslquery').__file__)
report = dict(version=oslquery.VERSION_STRING, blenderRevision=bpy.app.build_hash.decode(),
              nativeModuleSha256=hashlib.sha256(module.read_bytes()).hexdigest(),
              invalidRejected=True, invalidQueryError=invalid_error, missingRejected=True,
              missingDiagnostic=bool(missing_error), recovered=True)
(base / 'native-behavior.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
