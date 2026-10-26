# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise original Python dependency APIs in both unit and browser checks."""
import concurrent.futures
import hashlib
import hmac
import importlib.metadata
import io
import json
from pathlib import Path
import sqlite3
import ssl
import lzma
import zstandard


def run(directory):
    work = Path(directory)
    work.mkdir(parents=True, exist_ok=True)
    from _bpy_ghost_web_download import downloader_context, mutex_lock_and_open
    context = downloader_context()
    parent, child = context.Pipe()
    options = {'values': [1]}
    def worker(channel, settings):
        channel.send((settings, channel.recv()))
    thread = context.Process(name='Dependency channel check', target=worker,
                             args=(child, options), daemon=True)
    thread.start()
    options['values'].append(2)
    message = {'copied': [7]}
    parent.send(message)
    message['copied'].append(8)
    assert parent.poll(5) and parent.recv() == ({'values': [1]}, {'copied': [7]})
    thread.join(5)
    assert not thread.is_alive() and parent.poll(0)
    try:
        parent.recv()
    except EOFError:
        pass
    else:
        raise AssertionError('Downloader channel did not signal EOF')
    parent.close()
    lock_path = work / 'metadata-lock-test.bin'
    locked, unlock = mutex_lock_and_open(lock_path, 'wb')
    assert locked is not None
    locked.write(b'preserved')
    assert mutex_lock_and_open(lock_path, 'wb') == (None, None)
    unlock(locked)
    locked, unlock = mutex_lock_and_open(lock_path, 'rb')
    assert locked.read() == b'preserved'
    unlock(locked)
    source = ('Blender browser database 日本語\n' * 10003).encode()
    for format in (lzma.FORMAT_XZ, lzma.FORMAT_ALONE):
        compressed = lzma.compress(source, format=format)
        assert lzma.decompress(compressed, format=format) == source
        try:
            lzma.decompress(compressed[:-13], format=format)
        except lzma.LZMAError:
            pass
        else:
            raise AssertionError('Truncated LZMA stream accepted')
    filters = [{'id': lzma.FILTER_DELTA, 'dist': 4}, {'id': lzma.FILTER_LZMA2, 'preset': 3}]
    assert lzma.decompress(lzma.compress(source, format=lzma.FORMAT_RAW, filters=filters),
                           format=lzma.FORMAT_RAW, filters=filters) == source
    with lzma.open(work / 'python-roundtrip.xz', 'wb') as file:
        file.write(source)
    with lzma.open(work / 'python-roundtrip.xz', 'rb') as file:
        assert file.read() == source
    cctx = zstandard.ZstdCompressor(level=5, threads=2, write_checksum=True)
    compressed = cctx.compress(source)
    assert zstandard.ZstdDecompressor().decompress(compressed) == source
    assert zstandard.get_frame_parameters(compressed).has_checksum
    corrupted = bytearray(compressed)
    corrupted[-1] ^= 1
    try:
        zstandard.ZstdDecompressor().decompress(corrupted)
    except zstandard.ZstdError:
        pass
    else:
        raise AssertionError('Corrupt Zstandard checksum accepted')
    (work / 'python-roundtrip.zst').write_bytes(compressed)
    output = io.BytesIO()
    with cctx.stream_writer(output, closefd=False) as writer:
        writer.write(source[:123])
        writer.write(source[123:])
    with zstandard.ZstdDecompressor().stream_reader(output.getvalue()) as reader:
        assert reader.read() == source

    connection = sqlite3.connect(':memory:')
    options = sorted(row[0] for row in connection.execute('pragma compile_options'))
    connection.execute('create table assets(id integer primary key,name text,payload blob)')
    connection.executemany('insert into assets(name,payload) values(?,?)',
        [(f'object {i} 日本語', bytes(range(256))) for i in range(50)])
    connection.commit()
    connection.create_function('reverse_text', 1, lambda value: value[::-1], deterministic=True)
    assert connection.execute("select reverse_text('Blender')").fetchone() == ('rednelB',)
    for version in (3, 4, 5):
        name = f'search{version}'
        connection.execute(f'create virtual table {name} using fts{version}(body)')
        connection.execute(f"insert into {name} values('browser rendering dependencies')")
        assert connection.execute(f"select count(*) from {name} where {name} match 'render*'").fetchone() == (1,)
    connection.execute('create virtual table spatial using rtree(id,minx,maxx,miny,maxy)')
    connection.execute('insert into spatial values(1,0,1,0,1)')
    assert connection.execute('select id from spatial where minx>=0').fetchall() == [(1,)]
    assert connection.execute("select json_extract('{\"a\":[7]}','$.a[0]')").fetchone() == (7,)
    assert connection.execute('select median(column1) from (values(1),(3),(2))').fetchone() == (2.0,)
    assert connection.execute("select geopoly_area('[[0,0],[2,0],[2,2],[0,2],[0,0]]')").fetchone() == (4.0,)
    assert connection.execute('select count(*) from sqlite_stmt').fetchone()[0] > 0
    connection.commit()
    connection.execute("insert into assets values(100,'rolled back',x'00')")
    connection.rollback()
    assert connection.execute('select count(*) from assets').fetchone() == (50,)
    with connection.blobopen('assets', 'payload', 1) as blob:
        assert blob.read() == bytes(range(256))
    copy = sqlite3.connect(':memory:')
    copy.deserialize(connection.serialize())
    assert copy.execute('select name from assets where id=1').fetchone() == ('object 0 日本語',)
    file = sqlite3.connect(work / 'python-roundtrip.sqlite')
    connection.backup(file)
    assert file.execute('pragma integrity_check').fetchone() == ('ok',)
    assert file.execute('pragma journal_mode=wal').fetchone() == ('wal',)
    reader = sqlite3.connect(work / 'python-roundtrip.sqlite', timeout=0)
    file.execute('begin immediate')
    file.execute("update assets set name='not committed' where id=1")
    assert reader.execute('select name from assets where id=1').fetchone() == ('object 0 日本語',)
    try:
        reader.execute("update assets set name='conflict' where id=1")
    except sqlite3.OperationalError as error:
        assert 'locked' in str(error)
    else:
        raise AssertionError('Concurrent SQLite writer acquired an occupied lock')
    reader.close()
    file.rollback()
    assert file.execute('pragma wal_checkpoint(truncate)').fetchone()[0] == 0
    file.close()
    copy.close()
    connection.close()

    assert hashlib.pbkdf2_hmac('sha256', b'password', b'salt', 1).hex() == '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b'
    assert hashlib.scrypt(b'', salt=b'', n=16, r=1, p=1, dklen=64).hex() == '77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906'
    assert hmac.digest(b'key', b'The quick brown fox jumps over the lazy dog', 'sha256').hex() == 'f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8'
    assert hashlib.sha256.__module__ == '_hashlib'
    assert ssl.RAND_bytes(64) != ssl.RAND_bytes(64)
    cert, key = work / 'test.cert.pem', work / 'test.key.pem'

    def tls(version, trust=True, hostname='blender-web.test'):
        serverctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        serverctx.load_cert_chain(cert, key)
        clientctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        for ctx in (serverctx, clientctx):
            ctx.minimum_version = ctx.maximum_version = version
        if trust:
            clientctx.load_verify_locations(cafile=cert)
        ci, co, si, so = (ssl.MemoryBIO() for _ in range(4))
        client = clientctx.wrap_bio(ci, co, server_hostname=hostname)
        server = serverctx.wrap_bio(si, so, server_side=True)
        done = [False, False]
        try:
            for _ in range(100):
                for i, peer in enumerate((client, server)):
                    if not done[i]:
                        try:
                            peer.do_handshake()
                            done[i] = True
                        except ssl.SSLWantReadError:
                            pass
                if co.pending:
                    si.write(co.read())
                if so.pending:
                    ci.write(so.read())
                if all(done):
                    break
        except ssl.SSLCertVerificationError:
            assert not trust or hostname != 'blender-web.test'
            return 'rejected'
        assert trust and hostname == 'blender-web.test' and all(done)
        assert client.getpeercert()['subjectAltName'] == (('DNS', 'blender-web.test'),)
        assert client.write(source[:2048]) == 2048
        si.write(co.read())
        assert server.read(4096) == source[:2048]
        return client.version()

    assert tls(ssl.TLSVersion.TLSv1_2) == 'TLSv1.2'
    assert tls(ssl.TLSVersion.TLSv1_3) == 'TLSv1.3'
    assert tls(ssl.TLSVersion.TLSv1_3, trust=False) == 'rejected'
    assert tls(ssl.TLSVersion.TLSv1_3, hostname='wrong.test') == 'rejected'
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        assert list(pool.map(tls, [ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3])) == ['TLSv1.2', 'TLSv1.3']

    import attrs, cattrs, fastjsonschema, requests, certifi, idna, charset_normalizer
    import tomli_w, autopep8, pycodestyle, typing_extensions, packaging.version
    from docutils.core import publish_parts
    from Cython.Compiler.Main import compile as compile_cython
    assert idna.encode('日本語.jp').decode() == 'xn--wgv71a119e.jp'
    assert str(charset_normalizer.from_bytes('Blender 日本語'.encode()).best()) == 'Blender 日本語'
    request = requests.Request('POST', 'https://blender-web.test/api', json={'cube': 1}).prepare()
    assert request.body == b'{"cube": 1}' and request.headers['Content-Type'] == 'application/json'
    assert ssl.create_default_context(cafile=certifi.where()).cert_store_stats()['x509_ca'] > 100
    @attrs.define
    class Asset:
        name: str
        vertices: int
    asset = cattrs.structure({'name': 'Cube', 'vertices': 8}, Asset)
    assert cattrs.unstructure(asset) == {'name': 'Cube', 'vertices': 8}
    validate = fastjsonschema.compile({'type': 'object', 'properties': {'vertices': {'type': 'integer'}}, 'required': ['vertices']})
    assert validate({'vertices': 8}) == {'vertices': 8}
    assert 'vertices = 8' in tomli_w.dumps({'vertices': 8})
    assert autopep8.fix_code('x=1\n') == 'x = 1\n'
    assert '<strong>Cube</strong>' in publish_parts('**Cube**', writer='html')['fragment']
    assert packaging.version.Version('1.2') < packaging.version.Version('1.3')
    pyx = work / 'browser_generated.pyx'
    pyx.write_text('cpdef int sum_values(int a, int b):\n    return a + b\n')
    generated = work / 'browser_generated.c'
    compiled = compile_cython(str(pyx), output_file=str(generated), quiet=True,
                              compiler_directives={'language_level': 3})
    assert compiled.num_errors == 0 and 'PyInit_browser_generated' in generated.read_text()
    packages = {name: importlib.metadata.version(name) for name in
        ('setuptools', 'packaging', 'pyproject-metadata', 'idna', 'charset-normalizer',
         'urllib3', 'certifi', 'requests', 'autopep8', 'pycodestyle', 'docutils',
         'attrs', 'cattrs', 'fastjsonschema', 'typing-extensions', 'tomli-w', 'pip', 'Cython', 'zstandard')}
    report = dict(sqlite=sqlite3.sqlite_version, sqliteOptions=options, sqliteCallbacks=True,
        sqliteSerialization=True, sqliteWAL=True, sqliteWriterLock=True,
        lzmaFormats=3, zstandard=zstandard.__version__,
        zstandardThreads=2, openssl=ssl.OPENSSL_VERSION, tls12=True, tls13=True,
        untrustedRejected=True, hostnameRejected=True, parallelTLS=2,
        scrypt=True, pbkdf2=True, hmac=True, packages=packages, cythonCodeGeneration=True,
        downloaderWorkerChannel=True, metadataLock=True,
        roundtripSHA256=hashlib.sha256(source).hexdigest(), roundtripBytes=len(source))
    (work / 'python-foundation.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def check_network(url, directory):
    import base64
    import requests
    payload = bytes((i * 17 + i // 251) % 256 for i in range(262147))
    response = requests.get(url + '/data', timeout=5)
    assert response.status_code == 200 and response.content == payload
    with requests.get(url + '/data', stream=True, timeout=5) as response:
        assert b''.join(response.iter_content(7919)) == payload
    response = requests.post(url + '/echo', data=payload, timeout=5,
                             headers={'X-Blender-Proof': 'browser-worker'})
    echoed = response.json()
    assert base64.b64decode(echoed['body']) == payload
    assert echoed['proof'] == 'browser-worker'
    assert 'Chrome' in echoed['userAgent']
    assert requests.get(url + '/redirect', timeout=5).content == payload
    assert requests.get(url + '/missing', timeout=5).status_code == 404
    try:
        requests.get(url + '/slow', timeout=.05)
    except requests.exceptions.Timeout:
        pass
    else:
        raise AssertionError('HTTP timeout was ignored')
    try:
        requests.get(url + '/cors-denied', timeout=5)
    except requests.exceptions.ConnectionError:
        pass
    else:
        raise AssertionError('CORS rejection did not reach Python')
    assert requests.get(url + '/data', timeout=5).content == payload
    from _bpy_internal.http.downloader import (
        ConditionalDownloader, BackgroundDownloader, DownloaderOptions,
        MetadataProviderFilesystem, QueueingReporter)
    import time
    work = Path(directory)
    provider = MetadataProviderFilesystem(work / 'http-metadata')
    direct = ConditionalDownloader(provider)
    direct.timeout = 5
    reporter = QueueingReporter()
    direct.add_reporter(reporter)
    target = work / 'download-direct.bin'
    direct.download_to_file(url + '/data', target)
    assert target.read_bytes() == payload
    direct.download_to_file(url + '/data', target)
    events = []
    while True:
        try:
            events.append(reporter.pop()[0])
        except IndexError:
            break
    assert 'download_finished' in events and 'already_downloaded' in events
    done, callback_errors = [], []
    options = DownloaderOptions(metadata_provider=provider, timeout=5,
                                max_disk_size_bytes=len(payload) + 1, num_parallel_downloads=2)
    background = BackgroundDownloader(options, lambda *args: callback_errors.append(str(args)))
    background.start()
    try:
        for name in ('one', 'two'):
            background.queue_download(url + '/data?resource=' + name,
                work / ('download-' + name + '.bin'),
                lambda request, file: done.append(file.name))
        deadline = time.monotonic() + 30
        while len(done) < 2 and time.monotonic() < deadline:
            background.update()
            assert not background.num_downloads_error and not callback_errors
            time.sleep(.01)
        assert sorted(done) == ['download-one.bin', 'download-two.bin']
        for name in done:
            assert (work / name).read_bytes() == payload
    finally:
        background.shutdown()
    assert background.is_shutdown_complete and not background.is_subprocess_alive
    return dict(binaryGET=True, binaryPOST=True, responseChunks=True, browserDecompression=True,
                customHeader=True, redirect=True, status404=True, timeout=True,
                corsRejected=True, recovery=True, networkStreaming=False,
                originalConditionalDownloader=True, conditional304=True,
                originalBackgroundDownloader=True, parallelDownloads=2, workerShutdown=True)


def verify_files(directory):
    work = Path(directory)
    report = json.loads((work / 'python-foundation.json').read_text())
    for name, decoded in [('xz', lzma.decompress((work / 'python-roundtrip.xz').read_bytes())),
                          ('zst', zstandard.ZstdDecompressor().decompress((work / 'python-roundtrip.zst').read_bytes()))]:
        assert len(decoded) == report['roundtripBytes']
        assert hashlib.sha256(decoded).hexdigest() == report['roundtripSHA256'], name
    with sqlite3.connect(work / 'python-roundtrip.sqlite') as connection:
        assert connection.execute('pragma integrity_check').fetchone() == ('ok',)
        assert connection.execute('select count(*) from assets').fetchone() == (50,)
        assert connection.execute('select name,payload from assets where id=1').fetchone() == ('object 0 日本語', bytes(range(256)))
    return dict(sqlite=True, xz=True, zstandard=True)


def browser_check(mode, directory, network_url=None):
    import os
    import traceback
    try:
        if mode == 'core':
            report = run(directory)
        elif mode == 'network':
            report = json.loads((Path(directory) / 'python-foundation.json').read_text())
            report['network'] = check_network(network_url, directory)
            (Path(directory) / 'python-foundation.json').write_text(json.dumps(report, indent=2) + '\n')
        elif mode == 'offline':
            import bpy
            report = verify_files(directory)
            assert all(report.values())
            bpy.ops.wm.open_mainfile(filepath=str(Path(directory) / 'python-foundation.blend'))
        else:
            raise ValueError(mode)
        os.write(1, ('PYTHON_FOUNDATION_' + mode.upper() + '\n').encode())
        return report
    except BaseException:
        os.write(2, ('PYTHON_FOUNDATION_FAILED:' + traceback.format_exc()).encode())
        raise
