# Python dependency checks

[Watch the actual browser recording](python-foundation-walkthrough.mp4).

Published bundle `f5bc7678d055b2921075` passed the Python foundation checks on hardware
WebGPU, software WebGPU and the fallback runtime, with independent native
Blender 5.2.2 LTS readback. All 108 local stages, both source CI builds and all
17 deployment groups passed. A fresh hardware run on the [live Pages site](https://shlok-bhakta.github.io/blender-web/)
passed the foundation libraries, public HTTPS downloads and offline reopening,
followed by independent native readback. Both original background downloads
overlapped at the fixture server. See the [live proof](combined-dependencies-live.json)
and [validation record](combined-dependencies-validation.json).

The recording and foundation checks use this published bundle. The combined
release also includes the image, color, volume and material Python bindings,
AVIF and original USD CPU plugins.

| Added component | Verified operations |
| --- | --- |
| SQLite 3.51.3 | Transactions, rollback, Python callbacks, FTS3/4/5, RTREE, JSON, GEOPOLY, median, blobs, backup, serialization, WAL read isolation and writer locks |
| XZ/LZMA 5.2.5 | XZ, ALONE and raw filter formats, truncated input rejection and native file readback |
| OpenSSL 3.5.6 | Original CPython SSL/hash modules, TLS 1.2/1.3 memory-BIO handshakes, parallel sessions, certificate and hostname rejection, PBKDF2, scrypt and HMAC vectors |
| Python Zstandard 0.25.0 | Two compression workers, streaming API, checksum rejection and native file readback |
| Native Python runtime packages | Nineteen package versions match native Blender, including the sixteen locked pure Python distributions, bundled pip, Cython and Zstandard |
| Cython 3.0.11 | Original pure Python compiler generates C source for an authored extension |
| Browser HTTP | Binary GET/POST, custom headers, redirects, gzip, chunk iteration, 404, timeout, CORS rejection and recovery |
| Original Blender downloader | Conditional gzip download, ETag/304 reuse, metadata locking, two requests overlapping at the server, background callbacks and worker shutdown |
| Persistence | Database and compressed files survive an offline reload; native Blender opens the saved `.blend` and compares its proof text with the browser report |

The selected runtime is 428.13 MiB before transfer compression. The service
worker caches the selected runtime files by content hash. The existing fallback
selection and persistent OPFS filesystem also cover these Python packages.

The GHOST adapter runs HTTP on dedicated workers with the GIL released.
The browser controls TLS, CORS, redirects and restricted request headers.
Responses are buffered; chunk iteration does not stream from the network.
OpenSSL's separate TLS tests exercise its original memory-BIO APIs.

The downloader context implements the operations used by Blender's downloader
with threads and copied message channels. It does not supply general Python
multiprocessing or native subprocesses. Marketplace installation and cross-tab
metadata locking remain unverified. SQLite file mmap is disabled, and loading
SQLite extensions still needs dynamic Wasm linking. Cython's native accelerators
and loading its generated extensions remain pending. Full dependency parity
has not been reached.

See the [acceptance evidence](python-foundation.acceptance.json),
[recording metadata](python-foundation-recording.json),
[build and test instructions](../native/README.md) and
[remaining component audit](dependency-components.md).

![Dependency results in Blender's original Text Editor](python-foundation-browser.png)
