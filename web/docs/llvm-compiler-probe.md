# Original LLVM browser compiler proof

The pinned LLVM 20.1.8 and LLD libraries now compile LLVM IR into executable
WebAssembly in Node and Chromium. Chromium completes compilation and execution
after networking is disabled. The [evidence](llvm-compiler-probe.json) records
source hashes, compiler binary hashes, SDK version and browser results.

Three independent modules return different folded constants, then each passes
4,096 integer cases, 4,096 signed 64-bit cases, 4,096 scalar shader values and
4,096 SIMD values. Generated shaders call a host function and write through
linear-memory pointers. Malformed IR and a failed link are rejected, followed
by successful compilation and execution in the same compiler instance.
The builder also disassembles the generated module and requires actual
`f32x4.mul` and `f32x4.add` instructions.

The standalone compiler Wasm is 27,980,873 bytes. Its initial heap is 128 MiB
and its configured maximum is 2 GiB. Eight pthread workers are preloaded;
LLD uses four threads. Compilation durations in the evidence are functional
run timings taken alongside other builds, rather than controlled benchmarks.

The published Blender bundle remains `81aa3ca51ec1b21db65c`. This compiler is a
separate dependency proof. Original OSL compilation, the Cycles execution
adapter, shared application memory, worker symbol installation and shader
lifetime checks remain pending. The application still uses a wasm32 heap
limited to 4 GiB.

With the pinned Emscripten SDK activated and the repository's Playwright
prerequisites installed:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_llvm_probe.py --jobs 4
```

The builder reads the LLVM version and checksum from Blender's pinned recipes,
extracts the original source, builds native TableGen and Wasm libraries, then
runs the Node and offline Chromium checks. Its build directory is separate
from the production dependency prefix. `--skip-libraries` repeats the compiler
link and checks using an existing stamped source tree and built libraries.

The implementation is in [the compiler fixture](../native/tests/llvm-compiler.cc),
[its CMake target](../native/llvm-probe/CMakeLists.txt),
[the generated-code checks](../tests/llvm-generated.cjs),
[the Chromium runner](../tests/llvm-compiler.cjs), and
[the builder](../tools/native_llvm_probe.py).
