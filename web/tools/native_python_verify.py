#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read browser SQLite/compression outputs and the saved .blend in native Blender."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from prepare import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--blender', default='blender')
    args = parser.parse_args()
    directory = args.directory.resolve()
    report = json.loads((directory / 'acceptance.json').read_text())
    source = ROOT / 'web/native/tests/python-foundation.py'
    assert report['sourceSha256'] == hashlib.sha256(source.read_bytes()).hexdigest()
    for name, entry in report['files'].items():
        data = (directory / name).read_bytes()
        assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'], name
    output = directory / 'native'
    output.mkdir(exist_ok=True)
    script = output / 'verify.py'
    script.write_text('''import bpy, hashlib, importlib.metadata, json, lzma, sqlite3, zstandard
from pathlib import Path
import sys
directory=Path(sys.argv[sys.argv.index('--')+1])
report=json.loads((directory/'acceptance.json').read_text())
expected=('Blender browser database 日本語\\n'*10003).encode()
assert lzma.decompress((directory/'python-roundtrip.xz').read_bytes())==expected
assert zstandard.ZstdDecompressor().decompress((directory/'python-roundtrip.zst').read_bytes())==expected
assert hashlib.sha256(expected).hexdigest()==report['report']['roundtripSHA256']
network_expected=bytes((i*17+i//251)%256 for i in range(262147))
for name in ('download-direct.bin','download-one.bin','download-two.bin'):
    assert (directory/name).read_bytes()==network_expected, name
with sqlite3.connect(directory/'python-roundtrip.sqlite') as connection:
    assert connection.execute('pragma integrity_check').fetchone()==('ok',)
    rows=connection.execute('select id,name,payload from assets order by id').fetchall()
    assert rows==[(i+1,f'object {i} 日本語',bytes(range(256))) for i in range(50)]
    for version in (3,4,5):
        name=f'search{version}'
        assert connection.execute(f"select count(*) from {name} where {name} match 'render*'").fetchone()==(1,)
    native_sqlite_options=sorted(row[0] for row in connection.execute('pragma compile_options'))
    functional=lambda options:{option for option in options if not option.startswith(('COMPILER=','MAX_MMAP_SIZE='))}
    assert functional(native_sqlite_options)==functional(report['report']['sqliteOptions'])
native_packages={name:importlib.metadata.version(name) for name in report['report']['packages']}
assert native_packages==report['report']['packages']
generated=(directory/'browser_generated.c').read_text()
assert 'PyInit_browser_generated' in generated and 'sum_values' in generated
bpy.ops.wm.open_mainfile(filepath=str(directory/'python-foundation.blend'))
saved=json.loads(bpy.data.texts['Browser Python dependency checks'].as_string())
assert saved==report['report']
result=dict(sqliteReadback=True,lzmaReadback=True,zstandardReadback=True,
            savedBlendVerified=True,cythonCodeInspected=True,httpDownloadsVerified=True,nativePackageVersions=native_packages,
            nativeSQLiteOptions=native_sqlite_options,sqliteOptionsMatchExceptMmapAndCompiler=True)
(directory/'native/results.json').write_text(json.dumps(result,indent=2)+'\\n')
''')
    with (output / 'readback.log').open('w') as log:
        subprocess.run([args.blender, '--background', '--factory-startup', '--python-exit-code', '1',
            '--python', str(script), '--', str(directory)], stdout=log, stderr=subprocess.STDOUT, check=True)
    # This readback checks the archived certificate signature. Its short-lived
    # fixture validity is checked by the actual browser TLS handshakes.
    subprocess.run(['openssl', 'verify', '-no_check_time', '-check_ss_sig', '-CAfile', str(directory / 'test.cert.pem'),
                    str(directory / 'test.cert.pem')], check=True, capture_output=True)
    result = json.loads((output / 'results.json').read_text())
    result.update(bundleVersion=report['bundleVersion'], sourceSha256=report['sourceSha256'],
        verifierSourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        files=report['files'], tlsCertificateSignature=True)
    (directory / 'native-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in
                     ('bundleVersion', 'sqliteReadback', 'lzmaReadback', 'zstandardReadback', 'savedBlendVerified')}))


if __name__ == '__main__':
    main()
