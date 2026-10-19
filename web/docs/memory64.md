# Memory beyond 4 GiB

Our published Blender binary uses wasm32 and a 4 GiB maximum linear memory.
That is its pointer address space, not a limit on the browser's total RAM.
WebGPU allocations, browser processes and caches also consume memory outside
that heap.

Memory64 is the route to a larger shared Blender heap. The current
[WebAssembly JavaScript API draft](https://webassembly.github.io/spec/js-api/index.html#limits)
specifies a 16 GiB runtime ceiling for a 64-bit memory. Actual allocations can
fail earlier because of browser policy, available address space or RAM.
[Emscripten's MEMORY64 setting](https://emscripten.org/docs/tools_reference/settings_reference.html#memory64)
changes the target ABI. Its `MEMORY64=2` mode lowers back to wasm32 and does
not remove the 4 GiB address-space limit.

## Verified probe

The isolated probe in `web/native/tests/memory64.c` compiles with our pinned
Emscripten 4.0.23. On Chromium 145.0.7632.6, without experimental browser flags,
and SDK Node 24.19.0, it:

- Uses eight-byte pointers and a shared pthread heap with an 8 GiB maximum.
- Allocates 4,295,032,832 bytes in one block.
- Grows linear memory to 4,395,958,272 bytes, about 4.09 GiB.
- Checks addresses immediately below and above 4 GiB and writes beyond that
  boundary from another pthread.

Only a few pages are touched. This verifies addressing and shared-memory
growth, not physical residency of four gigabytes or Blender scene capacity.
The production Blender build remains wasm32. The shell's older Node 22.22.2
rejects the generated BigInt memory descriptor, so use the SDK Node explicitly.

```sh
source /tmp/blender-web-emsdk/emsdk_env.sh
PLAYWRIGHT_BROWSERS_PATH=/tmp/blender-web-browsers \
  python3 web/tools/native_memory64.py \
  --node /tmp/blender-web-emsdk/node/24.19.0_64bit/bin/node
```

The command writes compiler flags, source and Wasm hashes, browser and Node
versions, and both runtime results to `build/native/memory64/acceptance.json`.

## Production migration requirements

Use a separate wasm64 build directory and dependency prefix. Every static
library, CPython module, NumPy module, shader compiler and Blender object must
agree on the pointer ABI. Changing only the final link flags cannot work.

The current toolchain and NumPy cross file explicitly name wasm32. NumPy's
static module configuration also handles its 32-bit C-long ABI. Both need a
separate wasm64 configuration. The DNA packing shim already applies only to
`__wasm32__`; native DNA generation, `.blend` round trips and ResourceScope
layout checks must pass again with 64-bit pointers.

Audit hand-written JavaScript bindings for pointer coercions and signatures,
including GHOST events, OPFS, WebGPU, Python callbacks and JSPI job entry points.
Then repeat the dependency checks, native UI checks, `.blend` and Alembic
exchange tests, NumPy comparisons, render references and memory workloads.
Measure performance as well: wider pointers increase data size and do not make
renders inherently faster.

Keep wasm32 as a separately tested fallback while establishing browser and
device coverage. Multiple Wasm memories or workers can hold separate data, but
they do not turn existing C++ pointers into a single larger Blender heap.
