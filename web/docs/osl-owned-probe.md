# Original OSL module ownership and workers

Watch the [browser recording](osl-owned-walkthrough.mp4), inspect the
[screenshot](osl-owned-browser.png), or read the [evidence](osl-owned-probe.json).
Original OSL 1.15.3.0 compiles and executes its shader in offline Chromium,
then releases generated modules through its original native memory owner.
This is a standalone test. The published Blender application remains
`81aa3ca51ec1b21db65c`, with 62 production dependency recipes.

The earlier [runtime experiment](osl-runtime-probe.md) used the SDK loader,
whose `dlclose` does not unload modules. This adapter uses the SDK's actual
metadata parser and creates Wasm instances itself. It owns aligned static-data
allocations and function-table ranges. Retirement clears those table entries,
frees the allocation, and removes its code and instance references. Repeated
owners reuse the reserved table capacity. Browser garbage collection controls
when unreachable compiled Wasm objects are physically collected.

OSL's existing `MCJITMemoryManager` allocates a small ownership token. A guarded
hook before its original mapped-memory release retires programs with tokens in
that range. This follows OSL's native lifetime: its shared JIT memory can survive
individual groups and remains alive while native shading-system owners exist.
The test does not claim immediate reclamation on every shader-group deletion.

Workers share memory but have private function tables. Two guarded calls at
OSL's generated-code entry points reconcile a worker before it executes a shader.
An atomic epoch comparison avoids repeating installation at every shading point.
Compiler helper threads and unrelated busy workers do not install shader code.
Publication synchronizes the runtime thread without waiting for busy workers.
The existing GHOST pthread/futex registry supplies worker registration, notices
and exit cleanup. Callback changes retain their original synchronous publication
contract and use a separate epoch, so shader updates do not reinstall callbacks.

Each Node and offline Chromium run recreates eight original shading systems.
The original compiler produces fresh bytecode in each environment. Across the
eight cycles, the checks cover 6,144 shading points, 12,288 renderer calls and
6,144 calls through genuine libffi closures. Compilation occurs on a C++ worker
while an unrelated worker remains busy. Four workers already exist before
compilation; another starts afterward. A live callback is replaced while the
compiled shader remains alive. Varying numeric outputs match independent
expressions within `1e-6`; full string hashes match exactly.

After each native owner is destroyed, program records and owned static-data
allocations are empty. Function-table capacity stays constant after warmup.
Temporary LLVM object and linked Wasm files are removed. Execution uses WasmFS.
Chromium reports no page errors. Independent native OSL runs native-, Node- and
browser-authored bytecode through the original numerical/string checks.

The shared registry also passes the existing 9,341-case libffi suite and sparse
high-address checks in Node and Chromium. See the
[regression evidence](worker-resources-regression.json). The sparse heap test
covers callback metadata, closure allocation, stacks and aggregate arguments
above 2 GiB; it is not a large Blender scene benchmark.

The ownership executable uses a 128 MiB initial heap, a 4 GiB Wasm32 ceiling,
twelve worker slots and an unoptimized debug link. Artifact size and run time
are not production performance results. External side-module dependencies and
TLS definitions are rejected. Texture/closure coverage, concurrent compilation,
cancellation, variable-size module reuse, Python query bindings and the full
Cycles adapter still require qualification. Full dependency parity is pending.

Reproduce the [compiler proof](osl-compiler-probe.md) first. With the pinned SDK,
native Blender, a native C++ compiler, Flex, Bison and m4 available, run:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_osl_owned_probe.py \
    --blender /path/to/blender --jobs 4
```

`--compiler-build-dir`, `--llvm-build-dir`, `--prefix`, `--flex`, `--bison` and
`--m4` select prerequisites. `--build-dir` selects an independent original-source
build. The tool checks source archives and guarded patch hashes, builds matching
Clang helper bitcode, records offline browser execution and performs native
bytecode readback. Logs, source hashes, artifact hashes and the recording are
saved alongside `acceptance.json`.
