# Original OSL shader execution in the browser

Original OSL 1.15.3.0 now compiles and executes a shader in offline Chromium.
Watch the [recording](osl-runtime-walkthrough.mp4), inspect the
[screenshot](osl-runtime-browser.png), or read the [evidence](osl-runtime-probe.json).
This is a standalone execution proof. The published Blender application remains
`81aa3ca51ec1b21db65c`, with 62 production dependency recipes.

The browser runs the original OSL compiler and Clang, then loads its bytecode
through the original `ShadingSystem`. OSL builds and optimizes the LLVM IR.
A guarded GHOST execution-engine adapter emits position-independent Wasm with
original LLVM 20.1.8, links it with original LLD, and installs it through the
pinned SDK's loader. Shader execution uses generated code and the original
typed C++ renderer helpers.

Each Node and browser run checks 768 shading points. Four C++ threads exist
before compilation; another starts after loading. Together with the main
thread, their six contexts each evaluate 128 distinct coordinates. The shader
uses varying float and string userdata, color arithmetic, dot products, sine,
cosine, string comparisons, prefix checks, string length and string output.
All 1,536 renderer callbacks execute. Numeric outputs match independent
expressions within `1e-6`; full string hashes match exactly. Context cleanup
also completes successfully.

An independently compiled native executable runs the same C++ checks against
Blender's installed OSL and OpenImageIO libraries. It verifies native-authored,
Node-authored and browser-authored bytecode separately. All three produce the
same operation-check results. The earlier [compiler proof](osl-compiler-probe.md)
also checks exact native Cycles pixels for browser-authored bytecode.

The varying tests caught two wasm32 bugs. Generated string slots needed eight
bytes for their hashes, while OSL's public string type contains a four-byte
pointer. The original allocation corrupted allocator metadata and crashed during
context cleanup. The renderer-to-shader copy path also truncated hashes to four
bytes. A GHOST helper and [guarded runtime patch](../native/patches/osl-runtime.patch)
correct the tested storage and copy paths. Native behavior stays unchanged.
Worker tests additionally caught an unresolved `osl_bind_interpolated_param`;
the proof now exports 418 original typed renderer helpers consistently.

The helper bitcode is generated with original Clang 20.1.8 and the SDK's actual
Wasm C++ headers. Feeding the SDK compiler's LLVM 22 bitcode to the LLVM 20
runtime is not a compatible replacement. The build uses no fabricated compiler
version or ABI macros.

This finite experiment retains generated modules. The SDK's `dlclose` does not
unload them, so the report explicitly says `reclamationVerified: false`.
Production integration still needs module memory and table ownership, retirement,
cancellation, concurrent compilation checks, WasmFS support and a mixed regression
with ctypes callbacks. Textures, closures, interactive parameters and the full
Cycles OSL adapter are not covered by this shader. Python query bindings remain
pending. These gaps prevent calling the application dependency-complete.

The standalone executable links with `-O0` and function names for debugging.
Its size, startup behavior and timings are not production performance results.
It uses a 128 MiB initial heap, a 2 GiB ceiling, an 8 MiB stack and twelve
preloaded worker slots.

First reproduce the [OSL compiler proof](osl-compiler-probe.md). With the pinned
SDK, native Blender, a native C++ compiler, Flex, Bison and m4 available, run:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_osl_runtime_probe.py \
    --blender /path/to/blender --jobs 4
```

`--compiler-build-dir`, `--llvm-build-dir`, `--prefix`, `--flex`, `--bison` and
`--m4` select prerequisites. `--build-dir` selects an independent source tree and
runtime build so its bitcode settings cannot alter the compiler proof. The
command builds matching host bitcode tools if needed, applies both pinned-source
patches, runs offline browser compilation and execution, then checks all three
bytecode variants with native OSL. Source, library and artifact hashes are written
to `acceptance.json` alongside the logs, bytecode, screenshot and video.
