# Owned JIT data addresses

The experimental LLVM execution engine now resolves data symbols separately
from function-table entries. Previously, `getGlobalValueAddress()` used the
function lookup and returned zero for a real LLVM global. The new path reads
the instance's exported data offset, adds its owned allocation base and checks
the wasm32 address range. Missing symbols, functions and retired programs
return zero from data lookup.

The [recorded checks](jit-data-addresses.json) use the installed LLVM 20.1.8
libraries in Node and offline Chromium. Thirty-two programs run in four batches
of eight live owners. Their eight layouts vary data allocation sizes from 4 to
32 KiB and the number of exported functions. Each has the same data symbol names
and a separate allocation. Tests verify array
initializers, all 64 bits of an integer global, relocated data and function
pointers, 128 reads from generated code on four workers, and retirement in a
different order from creation. Remaining programs retain their data after each
retirement. Each new data allocation is zeroed and aligned to 64 bytes. Both
runs reuse a pool of nine table blocks with 124 total slots. All allocations
and active table entries retire after every batch. Users join before their
native memory owner is released.

The existing original OSL fixture also passed with the changed engine and
loader. Each Node/browser run shades 7,296 points, makes 14,592 renderer
callbacks and 7,296 libffi calls, and checks closure trees, full string hashes,
nearest EXR samples, concurrent compilation, cancellation and dynamic worker
startup. Its emitted shaders pass the independently checked native OSL
fixture. This continues to be a standalone runtime check. Cycles OSL rendering
is still disabled in the application.

Activate the pinned SDK and install the repository's Playwright browser before
running:

```sh
python3 web/tools/native_jit_data_check.py --jobs 2
```

The tool requires the installed LLVM and libffi recipes. Its report records
source, dependency and artifact hashes. The fixture covers these eight layouts;
larger mixed-lifetime workloads and other alignments still need testing before
renderer integration.
The [Cycles ABI check](cycles-osl-abi.md) separately tests the original closure
and shader-global layouts and full string hashes.
