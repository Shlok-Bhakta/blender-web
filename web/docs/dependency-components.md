# Dependency component audit

This audit compares the pinned Blender 5.2.2 LTS sources and the installed native
5.2.2 build with browser bundle `087a1025b9389b80e57d`. It supplements the
[build flag report](dependency-status.json). Enabling a top-level library does
not establish that its Python bindings, plugins, devices or every file format
work. Full dependency parity has not been reached.

| Component | Browser status | Remaining work or evidence |
| --- | --- | --- |
| Base static libraries | 57 recipes | Original pinned sources and checksums; operation coverage is in [FEATURES.md](../native/FEATURES.md) |
| Embree, OpenPGL and OIDN CPU | Verified for tested cases | Original Cycles integration, compositor denoising, surface/volume guiding, standalone numerical checks and independent native readback |
| CPython compression, XML, decimal and core hashes | Available | Existing embedded Python checks cover zlib, bz2, XML, decimal and SHA-256 |
| CPython `_sqlite3`, `_lzma`, `_ssl`, `_hashlib` | Missing in this bundle | The installed target Makefile reports these modules missing. Isolated Wasm prototypes pass their Python APIs, TLS memory-BIO verification and native file readback; they are not yet integrated into Blender |
| CPython `_ctypes` and libffi | Missing in this bundle | Isolated libffi checks pass scalar, struct, varargs and same-worker closures. Foreign-worker callbacks fail because the generated function is absent from that worker's function table. Dynamic Wasm library loading also needs integration |
| Native Python site packages | Partial | NumPy and pxr bindings work. Packages pinned by `python_site_packages.cmake`, Python zstandard and Cython still need packaging and runtime checks |
| OpenImageIO, PyOpenColorIO, OpenVDB and MaterialX Python bindings | Missing | All four import in the installed native Blender. Their C++ libraries are present in the browser, but their separate Python bindings are disabled |
| libheif AVIF through OpenImageIO | Missing | The native recipe requires libheif 1.20.2 with AOM encoding and decoding. AOM is already ported. libde265, dav1d and the other optional libheif backends are disabled by the native recipe too |
| USD OpenVDB and Hydra image support | Partial | Native USD enables OpenVDB and its OpenImageIO plugin. Browser USD disables these components. Existing exchange checks cover scene schemas and MaterialX graph export |
| LLVM and Open Shading Language | Missing | Requires a Wasm-compatible compiler/runtime and the original Cycles integration. Native OSL Python bindings are also part of this dependency |
| Hydra graphics and OpenSubdiv GPU evaluation | Missing | Native graphics APIs require WebGPU adapters and evaluator checks. CPU imaging adapters and CPU subdivision already work |
| NumPy and OpenColorIO SIMD | Optimization work | The browser uses compiler vectorization but disables these libraries' explicit ISA dispatch. Their native SIMD implementations need Wasm equivalents and performance measurements |
| Browser platform services | Partial | Audaspace output and persistent OPFS storage work. Network transport, microphone/multichannel input, device files, additional windows, NDOF and XR need browser integrations |

The native NumPy `__config__.py` reports BLAS and LAPACK both not found. Its
bundled linear algebra routines are therefore the appropriate parity baseline;
adding BLAS remains an optional optimization. The native Ceres binary identifies
itself as `no_lapack` and `eigensparse`, and reports that SuiteSparse support was
not compiled in. Those libraries are not missing native dependencies for this
release.

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
