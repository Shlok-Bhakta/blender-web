# LLVM execution on a shared heap

The original LLVM 20.1.8 and LLD can compile code that executes against the
caller's shared Wasm heap. The [evidence](llvm-shared-probe.json) checks Node
and offline Chromium independently.

The proof compiles three position-independent modules at runtime. Each reads
and writes caller-owned buffers, calls a typed C++ host function, performs
scalar and SIMD arithmetic, and preserves 64-bit integer results. Four C++
workers exist before compilation and loading. Three additional workers start
after loading. Across these calls, 15,360 atomic updates produce unique old
counter values on the same shared allocation.

The SDK's real PIC startup object supplies TLS initialization for each loaded
module. Omitting it caused worker loading to fail with
`tlsInitFunc is not a function`; the proof does not replace TLS with a stub.

Module reclamation remains unverified. The SDK retains loaded side modules,
and its `dlclose` does not unload them. This finite proof records table growth
and reports `reclamationVerified: false`. Production integration needs ownership,
cleanup and cancellation checks before it can compile arbitrary numbers of
shader groups. It also needs regression checks alongside the ctypes callback
registry and the application's WasmFS build.

After building the [original LLVM proof](llvm-compiler-probe.md), activate the
pinned SDK and run:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_llvm_shared_probe.py --jobs 4
```

`--llvm-build-dir` selects the LLVM build. `--build-dir` selects this separate
executable and evidence directory. The command records the startup object,
source and artifact hashes. It does not modify Blender or the published bundle.
