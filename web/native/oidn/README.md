# OIDN's Wasm compiler bridge

OIDN 2.5.0 uses the pinned release's original CPU implementation and RT weights.
Its ISPC 1.30.0 kernels compile as generic eight-lane vectors, with sixteen lanes
for half pooling and upsampling. Emscripten lowers those vectors to Wasm SIMD128.
The normal network uses FP32 tensors and the existing threaded oneTBB library.

The official host ISPC binary has no Wasm target. The bridge requests generic
LLVM IR with 32-bit pointers and rejects native assembly and x86/ARM intrinsics.
This is limited to OIDN's interfaces, which contain no double or 64-bit integer
fields. It does not promise compatibility with arbitrary x86 LLVM modules.
The Linux x86-64 build host binary is SHA-256 pinned in `../ispc-host.json`.
The compiler version is checked against Blender's dependency definitions.

Two conversions precede Wasm code generation:

- Emscripten 4.0.23's LLVM revision `5243501` mishandles packed variable mask
  comparisons in `WebAssemblyISelLowering.cpp::performSETCCCombine`. The bridge
  expresses equality as vector XOR followed by `v128.any_true`.
- The pinned compiler cannot lower all native half-vector operations. Half
  storage stays as 16-bit integers. Conversion and comparisons use Imath's
  existing IEEE binary16 conversion, including ties-to-even rounding,
  subnormals, signed zeros, infinities and NaNs. FP32 convolution stays SIMD.

The conversion helpers compile separately so LLVM cannot recreate unsupported
half instructions before instruction selection. Unexpected half operations
fail the build. Kernel compilation has a process-group timeout.

The OIDN dependency patch selects the Wasm architecture, disables CPU affinity
and the unused dynamic-module loader, and checks image, tensor, buffer and
weight-offset sizes before 32-bit arithmetic can wrap. Browsers schedule workers;
they do not expose thread affinity. CUDA, HIP, SYCL and Metal devices are absent.
RTLightmap weights are omitted, matching Blender's native dependency recipe.

After activating the pinned SDK:

```sh
python3 web/tools/native_deps.py --jobs 4 OPENPGL OIDN
python3 web/tools/native_ispc_check.py
python3 web/tools/native_build.py --app --skip-deps --check-deps --jobs 4
```

The compiler check covers all binary16 input patterns, rounding boundaries,
random FP32 bit patterns, tail lanes, half comparisons and packed masks.
The OIDN library test runs original color-only and guided networks, half image
IO, cancellation/recovery, invalid sizes, invalid buffer ranges and a corrupt
64-bit weights offset. The OpenPGL test jointly links Embree and trains all three
original directional models for surface and volume sampling, then serializes
and reloads their fields. Its self-roundtrip does not establish cross-platform
field-file compatibility because upstream serializes native `size_t` values.

The shared Blender scene in `web/tests/render-libraries.py` covers Cycles CPU
denoising, surface and volume guiding, and the compositor Denoise node.
`native_render_libraries_baseline.py` compares standalone numerical results with
the pinned native libraries. `native_render_libraries_verify.py` independently
reopens and renders a browser-saved scene in native Blender. Library tests alone
do not establish browser integration, large-image capacity or performance parity.
