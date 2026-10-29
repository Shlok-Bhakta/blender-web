# Original OSL query library proof

OSL 1.15.3.0's original query library now passes in Node and offline Chromium.
The [evidence](osl-query-probe.json) records the native module hash, original
source pin, guarded patch, test sources and compiled artifacts.

Native Blender compiles and queries a shader with nine parameters. The Wasm
library matches its scalar, array, color, vector and matrix types, defaults,
metadata and outputs exactly, using both file and memory input. It also passes
128 independent queries across four C++ workers, missing-parameter lookups,
invalid-input rejection and recovery.

A wasm32 portability fix keeps OSL string hashes at their actual 64-bit width.
The original `size_t` alias fails OSL's encoded-type assertions with `8 == 4`.
The corrected header compiles, and runtime checks preserve both a known hash
with high bits set and a real interned string's full hash. The patch also uses
CMake's detected Clang version because the Emscripten wrapper reports the SDK
version in its `--version` output. Both changes are guarded for Emscripten.

The native query API sends malformed-bytecode diagnostics to the global error
log. A failed search-path lookup fills `geterror()`. The proof checks these
native behaviors and uses separate objects for successful file and memory
loads. Original OSLQuery appends parameters when repeatedly loading successful
shaders into the same object.

The standalone test Wasm is 407,268 bytes. This metadata proof builds only
`oslquery`, with Python bindings and LLVM shader bitcode disabled. Production
Blender integration, original OSL compilation, shader execution and the Python
query module remain pending. The published application remains
`81aa3ca51ec1b21db65c`, with 62 production dependency recipes.

With the pinned SDK, the production Wasm dependency prefix, native Blender,
Flex, Bison and m4 available, first build the
[separate LLVM proof](llvm-compiler-probe.md), then run:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_osl_query_probe.py --blender /path/to/blender --jobs 4
```

`--llvm-build-dir`, `--prefix`, `--flex`, `--bison` and `--m4` select existing
builds and host tools. The builder downloads the pinned original source,
applies [the guarded portability patch](../native/patches/osl.patch), builds
only the query library, regenerates the independent native reference, runs the
paired hash-width control, and checks Node and offline Chromium.
