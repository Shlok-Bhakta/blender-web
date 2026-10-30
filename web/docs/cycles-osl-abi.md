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

```sh
# Activate the pinned SDK and build OSL/OpenImageIO first.
python3 web/tools/native_cycles_osl_abi.py --blender /path/to/native/blender
```

Cycles' texture-handle packing still shifts a pointer-sized integer by 32 bits.
That representation needs a separate wasm32 adapter, including negative UDIM
indices and the `INT_MAX` missing-image sentinel. General JIT allocation stress,
renderer initialization, worker ownership and actual Cycles OSL renders remain
required before enabling the application option.
