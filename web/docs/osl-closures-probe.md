# Original OSL closure trees and string parameters

Watch the [recording](osl-closures-walkthrough.mp4), inspect the
[screenshot](osl-closures-browser.png), or read the
[evidence](osl-closures-probe.json).
Original OSL compiles and executes diffuse, emission and debug closures in
Node and offline Chromium. This remains a standalone test; the published
Blender application is still `81aa3ca51ec1b21db65c`, with 62 production recipes.

The shader combines three weighted closure components. C++ checks traverse the
resulting add/multiply/component tree and verify diffuse weights `(u, .25, v)`,
emission weights `(.1, .2, .3)`, debug weights `(.4, .4, .4)` and the normal
parameter. A varying string reaches both a formal debug parameter and the
optional diffuse label. Both preserve the complete 64-bit hash for `browser`
and `bravo`. Numeric comparisons use `1e-6`; string hashes match exactly.

The tests exposed another wasm32 storage bug. Public string pointers occupy
four bytes, while generated OSL strings contain eight-byte hashes. OSL rejected
a formal closure parameter whose C++ hash field occupied eight bytes. Its
formal and keyword copy paths also copied only four bytes. The
[guarded patch](../native/patches/osl-closures.patch) uses the existing GHOST
storage helper for validation and both copies. Public pointer sizes retain
their actual ABI values; native behavior stays unchanged.

A paired control executable links the earlier, qualified OSL library without
this closure patch. Its public API rejects the eight-byte hash field. The
corrected library accepts it. Both report a four-byte public string and an
eight-byte hash. This control completes normally and records its result in the
evidence, alongside hashes of both libraries and executables.

Each environment recreates eight shading systems and checks 7,296 points,
14,592 renderer calls and 7,296 genuine libffi callbacks. Two workers request
compilation concurrently. Four workers predate compilation, another starts
afterward, and a separate worker cooperatively cancels after sixteen points.
Callback replacement occurs while both compiled programs remain live. Native
OSL owners release module records and static allocations; function-table
capacity is reused. See the [ownership checks](osl-owned-probe.md) for the
worker and retirement design.

An independent native executable runs these same closure, string, numerical
and callback checks with Blender's installed OSL and OpenImageIO libraries.
It reads native-, Node- and browser-authored bytecode separately. All three
runs pass the same 7,296-point checks. The native executable uses genuine
native libffi 3.5.2 headers and code built from the pinned archive. Wasm libffi
headers cannot describe the native calling convention.

This does not establish the full Cycles closure catalog, BSDF evaluation or
image rendering. Texture operations, interactive parameters, cancellation
during compilation, variable-size module reuse, Python query bindings and the
Cycles adapter still need qualification. The debug executable uses a 128 MiB
initial heap, a 4 GiB Wasm32 ceiling and twelve preloaded worker slots. Its
size and timing are not production performance results.

Reproduce the [owned-runtime proof](osl-owned-probe.md) first, preserving its
library as the uncorrected control. Then, with the pinned SDK activated:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_osl_closures_probe.py \
    --blender /path/to/blender --jobs 4
```

`--control-build-dir` selects the qualified owned-runtime build.
`--compiler-build-dir`, `--llvm-build-dir`, `--prefix`, `--flex`, `--bison` and
`--m4` select prerequisites. `--build-dir` selects an independent source tree.
The command builds original OSL with four guarded patches, records offline
execution, builds native libffi and the independent native test, checks all
three bytecode variants and runs the paired registration control. Logs, hashes,
bytecode, images and video are saved alongside `acceptance.json`.
