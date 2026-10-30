# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare Wasm-compiled Cycles node metadata with independently native builds."""
import json
import hashlib
from pathlib import Path
import sys

import _cycles
import bpy
import oslquery

root = Path(__file__).resolve().parents[3]
assert bpy.app.build_hash.decode() == json.loads((root / 'web/upstream.json').read_text())['blenderRevision'][:12]
assert oslquery.VERSION_STRING == '1.15.3.0'
work = Path(sys.argv[sys.argv.index('--') + 1])
inventory = json.loads((work / 'inventory.json').read_text())
native_headers = Path(bpy.utils.system_resource('SCRIPTS')) / 'addons_core/cycles/shader'
local_headers = Path(next(iter(inventory.values()))['source']).parent
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
assert digest(native_headers / 'stdcycles.h') == digest(local_headers / 'stdcycles.h')
native = work / 'native'
native.mkdir(exist_ok=True)


def parameter(item):
    return dict(name=item.name, type=str(item.type), output=item.isoutput,
                variableArray=item.varlenarray, structure=item.isstruct,
                closure=item.isclosure, value=item.value,
                metadata=[parameter(value) for value in item.metadata])


def inspect(path):
    query = oslquery.OSLQuery(str(path))
    assert not query.geterror(), path
    return dict(name=query.shadername(), type=query.shadertype(),
                parameters=[parameter(value) for value in query],
                metadata=[parameter(value) for value in query.metadata])


results = {}
for name, item in inventory.items():
    output = native / (Path(name).stem + '.oso')
    output.unlink(missing_ok=True)
    assert _cycles.osl_compile(item['source'], str(output)), name
    expected = inspect(output)
    actual = inspect(item['wasmOutput'])
    assert json.dumps(actual, sort_keys=True) == json.dumps(expected, sort_keys=True), name
    results[name] = actual
(work / 'native-verification.json').write_text(json.dumps(dict(
    blenderRevision=bpy.app.build_hash.decode(), oslVersion=oslquery.VERSION_STRING,
    nativePreludeSha256=digest(native_headers / 'stdcycles.h'),
    shaderCount=len(results), parameterCount=sum(len(item['parameters']) for item in results.values()),
    compiledMetadataEqual=True, shaders=results), indent=2) + '\n')
print('CYCLES_OSL_SHADERS_NATIVE:', len(results), flush=True)
