# Dependency component audit

This audit compares candidate `81aa3ca51ec1b21db65c` with the pinned Blender
5.2.2 LTS sources and installed native build. All 118 local stages passed,
including the integrated original ctypes module, Python bindings, AVIF and
USD CPU image and volume plugins. Hardware, software and fallback runtimes,
offline reopening and independent native readback passed. The editor source CI
passed; the full application source CI and publication checks are pending.
The public Pages release remains `f5bc7678d055b2921075`.

See [the ctypes integration and recording](ctypes-integration.md),
[its acceptance evidence](python-ctypes.acceptance.json),
[render comparisons](render-optimization.md), and
[recorded background load](ctypes-host-load.json).
This audit supplements the [build flag report](dependency-status.json). A top-level
library flag does not prove its Python bindings, plugins, devices or every format
work. Full dependency parity has not been reached.

| Component | Browser status | Remaining work or evidence |
| --- | --- | --- |
| Base dependency recipes | 62 recipes | Original pinned sources and checksums; operation coverage is in [FEATURES.md](../native/FEATURES.md) |
| Embree, OpenPGL and OIDN CPU | Verified for tested cases | Original Cycles integration, compositor denoising, surface/volume guiding, standalone numerical checks and independent native readback |
| CPython compression, XML, decimal and core hashes | Available | Existing embedded Python checks cover zlib, bz2, XML, decimal and SHA-256 |
| CPython `_sqlite3`, `_lzma`, `_ssl`, `_hashlib` | Verified for tested cases | Original extensions in Blender; SQLite WAL/FTS/callbacks, LZMA files, TLS memory-BIO handshakes and certificate rejection, hash vectors and native file readback. Browser HTTP uses browser TLS; raw SSL sockets remain unavailable |
| CPython `_ctypes` and libffi | Verified for tested cases | Original module integrated in both Blender application variants. Python API and data symbols, struct arguments and returns, variadic formatting, NumPy views, 240 callbacks across four existing workers, eight newly created Python threads, offline reopening and three native comparisons pass. GHOST owns the static process namespace and worker callback bridge. Named external libraries, arbitrary dynamic Wasm extensions and general package installation remain unsupported. See [the integration checks](ctypes-integration.md) |
| Native Python site packages | Verified for tested cases | NumPy and pxr bindings, sixteen pinned pure Python wheels, bundled pip, Python Zstandard and the original Cython compiler. Package versions match native Blender. Native Cython accelerators, subprocess build tools and general extension installation remain pending |
| OpenImageIO, PyOpenColorIO, OpenVDB and MaterialX Python bindings | Verified for tested cases | Eleven original extensions, CPU color transforms, NumPy buffers, image operations, all nine grid types, exact large integers, four shader generators, threaded calls, offline persistence and native readback. See [the binding checks](python-bindings.md) |
| libheif AVIF through OpenImageIO | Verified for tested cases | Original libheif 1.20.2 and AOM encode/decode. Nine Blender exports cover 8/10/12-bit BW/RGB/RGBA, invalid-input recovery and native pixel readback. The Draco bridge isolates its opaque types to avoid static destructor collisions. Other optional backends are disabled by the native recipe too |
| USD CPU image and volume plugins | Verified for tested cases | Original hioOiio and hioOpenVDB factory operations, four grid types, TIFF IO and exact native readback. Both load in the optimized original application on hardware, software and fallback runtimes, with scene exchange and offline reopening. See [the checks](usd-cpu-imaging-checks.md) |
| MaterialX generators and render utilities | Partial | Original GLSL/MSL/OSL/MDL generators, core image/mesh utilities and all eight Python extensions work for tested cases. Shader text matches native exactly. Graphics renderers and their compiler/runtime integration remain pending |
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
