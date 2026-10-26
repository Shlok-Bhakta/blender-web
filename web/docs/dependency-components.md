# Dependency component audit

This audit compares the pinned Blender 5.2.2 LTS sources and the installed native
5.2.2 build with published browser bundle `8f28615dc99fd90616b7`. All 100 local
validation stages, both source CI builds and sixteen deployment groups passed.
A fresh hardware run on Pages also passed Python, public HTTPS downloads, offline
reopening and independent native readback. See the
[live verification](python-foundation-live.json). This audit supplements the
[build flag report](dependency-status.json). Enabling a top-level library does
not establish that its Python bindings, plugins, devices or every file format
work. Full dependency parity has not been reached.

| Component | Browser status | Remaining work or evidence |
| --- | --- | --- |
| Base dependency recipes | 60 recipes | Original pinned sources and checksums; operation coverage is in [FEATURES.md](../native/FEATURES.md) |
| Embree, OpenPGL and OIDN CPU | Verified for tested cases | Original Cycles integration, compositor denoising, surface/volume guiding, standalone numerical checks and independent native readback |
| CPython compression, XML, decimal and core hashes | Available | Existing embedded Python checks cover zlib, bz2, XML, decimal and SHA-256 |
| CPython `_sqlite3`, `_lzma`, `_ssl`, `_hashlib` | Verified for tested cases | Original extensions in Blender; SQLite WAL/FTS/callbacks, LZMA files, TLS memory-BIO handshakes and certificate rejection, hash vectors and native file readback. Browser HTTP uses browser TLS; raw SSL sockets remain unavailable |
| CPython `_ctypes` and libffi | Missing in this bundle | Isolated libffi checks pass scalar, struct, varargs and same-worker closures. A separate real dynamic-Wasm prototype propagates typed callbacks to existing/new workers, including 64-bit values above 53 bits. Integrating it with libffi needs closure lifetime and bounded-resource tests; it is not yet a ctypes port |
| Native Python site packages | Verified for tested cases | NumPy and pxr bindings, sixteen pinned pure Python wheels, bundled pip, Python Zstandard and the original Cython compiler. Package versions match native Blender. Native Cython accelerators, subprocess build tools and general extension installation remain pending |
| OpenImageIO, PyOpenColorIO, OpenVDB and MaterialX Python bindings | Missing in this bundle | All four import in native Blender. Their C++ libraries are present, but Python bindings remain a separate port. An isolated PyOpenColorIO prototype now passes its original package wrapper, CPU transforms, shader generation and process cleanup; it has not been integrated into the candidate |
| libheif AVIF through OpenImageIO | Missing | The native recipe requires libheif 1.20.2 with AOM encoding and decoding. AOM is already ported. libde265, dav1d and the other optional libheif backends are disabled by the native recipe too |
| USD OpenVDB and Hydra image support | Partial | Native USD enables OpenVDB and its OpenImageIO plugin. Browser USD disables these components. Existing exchange checks cover scene schemas and MaterialX graph export |
| MaterialX generators and render utilities | Partial | Native MaterialX enables rendering utilities and ships OSL/MDL generators. The browser build disables those components; graphics renderers also need browser backends |
| LLVM and Open Shading Language | Missing | Requires a Wasm-compatible compiler/runtime and the original Cycles integration. Native OSL Python bindings are also part of this dependency |
| Hydra graphics and OpenSubdiv GPU evaluation | Missing | Native graphics APIs require WebGPU adapters and evaluator checks. CPU imaging adapters and CPU subdivision already work |
| NumPy and OpenColorIO SIMD | Optimization work | The browser uses compiler vectorization but disables these libraries' explicit ISA dispatch. Their native SIMD implementations need Wasm equivalents and performance measurements |
| Browser platform services | Partial | Audaspace output, persistent OPFS and buffered browser HTTP work. Original conditional and background downloads pass, including overlapping requests, CORS/timeout recovery and shutdown. General multiprocessing, marketplace installation, raw sockets, microphone/multichannel input, device files, additional windows, NDOF and XR remain pending |

The native NumPy `__config__.py` reports BLAS and LAPACK both not found. Its
bundled linear algebra routines are therefore the appropriate parity baseline;
adding BLAS remains an optional optimization. The native Ceres binary identifies
itself as `no_lapack` and `eigensparse`, and reports that SuiteSparse support was
not compiled in. Those libraries are not missing native dependencies for this
release.

The native OpenVDB recipe disables AX; its Python bindings enable all grid types.
The native USD recipe also disables OSL, Ptex and the OpenColorIO plugin. The
native TIFF recipe disables LZMA, JBIG, Zstd, WebP and LERC. These disabled
subcomponents do not become parity requirements merely because their upstream
projects support them. HarfBuzz and FriBidi are already ported, but the pinned
Blender font renderer does not call them; connecting them is a separate text
rendering improvement.

Native shared libraries and subprocesses cannot be loaded as desktop machine
code by a browser. Static Python bindings, Wasm extensions and browser service
adapters must preserve the supported operations. A flag that suppresses an
error or returns placeholder results does not satisfy this audit. The production
application still has a 4 GiB Wasm32 heap limit; the separate memory64 probe is
not evidence that the application can exceed that limit.

The comparison uses the pinned recipes in
[`blender/build_files/build_environment/cmake`](../../blender/build_files/build_environment/cmake),
the target CPython Makefile, native module imports and native NumPy/Ceres build
information. Host build tools, disabled optional backends, browser service gaps
and performance work are distinguished from missing portable runtime libraries.
