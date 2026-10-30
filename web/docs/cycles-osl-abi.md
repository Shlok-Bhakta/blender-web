# Cycles and OSL data layout

The pinned Cycles headers have two wasm32 layout mismatches with the installed
OSL library. Cycles' add and multiply nodes occupy 16 and 24 bytes; OSL expects
12 and 20 bytes. Cycles also accepts a `size_t` string hash, which truncates the
original 64-bit hash on wasm32.

The guarded [header patch](../patches/native-osl-types.patch) calls a GHOST
alignment helper and uses a 64-bit hash type. A standalone check applies it to
a private copy of the original headers. It does not alter the pinned Blender
checkout or enable the application's OSL renderer.

The [results](cycles-osl-abi.json) record matching add/multiply sizes and
alignment, closure-component sizes, and every shared `ShaderGlobals` field
offset. The test constructs an original OSL closure tree and reads its weights,
node types and pointers through the actual Cycles types. Native and Wasm runs
also preserve the complete hash `8a7dc2d27a1bd227`, with assertions enabled.
The tool first requires the original headers to reproduce the failures.

The guarded texture adapter uses a pointer to Cycles' existing texture-map
record, which already holds its `ImageHandle`. It adds no allocation or ID
limit. Tests exercise all four texture types with zero, positive IDs,
`INT_MAX`, negative UDIM indices and `INT_MIN`, plus the original null-handle
decoding. These 24 cases use fixture records with Cycles' actual field types;
the test also instantiates the macro with Cycles' complete record type.

The three original OSL kernel source files, `closures.cpp`, `globals.cpp` and
`services.cpp`, compile to actual wasm32 objects with the shims. All 80 predefined
device-string assertions remain active and pass using OIIO's original
`strhash64` function. The report records the source and object hashes. Compiling
these objects does not link or qualify the Cycles OSL renderer.

```sh
# Activate the pinned SDK and build OSL/OpenImageIO first.
python3 web/tools/native_cycles_osl_abi.py --blender /path/to/native/blender
```

Renderer initialization, worker ownership, texture-map teardown and cached
shader lifetimes still need application tests. Actual Cycles OSL renders remain
required before enabling the application option.
The [full node-library compilation check](cycles-osl-shaders.md) independently
compares all 103 original shaders and their 719 parameters with native Blender.
