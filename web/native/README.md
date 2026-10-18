# Native Blender in the browser

This build uses the pinned official Blender release and its own top-level CMake
project. The executable contains Blender's original editors, window manager,
Python API, fonts, and startup scene. The separate `web/site` fixture remains
available for platform tests. The original interface and startup cube render through WebGPU in Chromium.

Activate the SDK pinned in `web/upstream.json`. Install CMake, a C/C++ host
compiler, Make, Ninja, Git LFS, and Python 3, then run:

```sh
python3 web/tools/native_build.py --app --jobs 8 --check-deps
python3 web/tools/serve.py --directory build/native/dist --pages-mode --port 8093
```

Open `http://127.0.0.1:8093/`. The service worker supplies the isolation headers
needed by shared Wasm memory on a static host such as GitHub Pages. A first visit
reloads once after worker activation. WebGPU and browser workers are required.

`--skip-deps` resumes with the installed dependency libraries. `--configure-only`
stops after CMake generation. Without `--app`, the command builds the smaller
original editor archive checkpoint with Python and the GPU backend disabled.

Dependencies use the versions and source hashes from Blender's pinned release.
The application adds threaded CPython 3.13, shaderc, SPIR-V Tools, and Tint. Their
recipes and pins are in `shader-deps/`. Source attribution is in
[THIRD_PARTY.md](THIRD_PARTY.md). Build logs and dependency reports are stored in
`build/native`; the dependency prefix is `build/native/deps/prefix`.

The browser runs Blender's C++ main loop on a pthread. GHOST receives keyboard,
mouse, wheel, and resize events from the canvas. A WebGPU backend translates
Blender's generated GLSL through shaderc and Tint into WGSL. The frontend is a
canvas and loading status; the editors are drawn by Blender itself.

The application uses an explicit web feature configuration, including native
OBJ/STL/PLY import/export, FBX import, both IK solvers and Bullet rigid bodies.
The [feature matrix](FEATURES.md) tracks enabled systems and remaining ports,
including Cycles. Native C++ uses `-O3`, Wasm SIMD, pthreads, and Wasm exceptions.
oneTBB handles Blender's parallel tasks. The runtime sizes its initial pthread pool from the browser's reported logical
CPU count, plus two workers for platform work. It prestarts between four and 64
workers and permits additional pthreads when needed. Shared memory starts at 512 MiB and can
grow to 4 GiB, subject to browser and device limits. This first backend maps one
native window to one canvas; additional browser windows remain follow-up work.
Desktop Chromium is the initial validation target. iOS Safari needs device testing.

Runtime scripts, fonts, color-management data, and Python's standard library are
packaged into the preload data file. User files live under `/user/blender-web/home` in WasmFS's
OPFS backend. If persistent storage is unavailable, that session uses memory.
The loader caches versioned JS/Wasm/data in Cache Storage and translated WGSL in
IndexedDB. Versioned URLs keep a tab's code and data matched across deployments.

All upstream changes live in guarded patch files in `web/patches/`. New platform,
GPU, startup, and build code lives under `intern/ghost/web` and `web/native`, then
is copied into an isolated source worktree. The reference submodule stays clean.
Small ABI helpers preserve 32-bit DNA record layout and ResourceScope's two-word
size invariant. Original DNA size and member-offset assertions stay enabled.
Image-cache keys retain 64-bit storage so runtime keys above `2^32` stay
distinct from render-slot hashes on Wasm32. A shader-scope shim normalizes invalid indices before signed 64-bit indices are
compared with Wasm32 `size_t`. The expression evaluator uses a per-thread software
math-error status because Wasm has no hardware floating-point exception flags.

`datatoc`, `makesdna`, `makesrna`, and `shader_tool` run as Wasm programs under Node with host
filesystem access confined to the build tools. This preserves target pointer
sizes and alignment during DNA generation. The shader regression check also builds a host executable and compares its
output with the Wasm processor.

The dependency checks exercise actual oneTBB workers, exception propagation,
PNG round trips and error recovery through OpenImageIO, OpenColorIO, and Blender's
WOFF2 font through FreeType/Brotli. Application checks also run embedded Python
modules and Blender expression-evaluator error cases. Browser acceptance runs
with `node web/tests/native.cjs` against the static server and writes screenshots
and logs to `build/native/evidence`.

The browser acceptance check verifies original UI pixels, a real WebGPU device,
worker startup, canvas resizing, native Python Console execution, OPFS writes
that survive reload, and offline application startup from Cache Storage. Its
evidence files include screenshots and `acceptance.json`. This establishes the
startup path; it does not establish complete editor or renderer compatibility.
Remote asset downloads requiring subprocesses, extra windows, and Safari device
validation remain incomplete.


The [first Eevee render and recording](../docs/eevee-default-scene.md) use the
unchanged factory cube scene at 1920×1080 and 64 samples. The render variant uses
Emscripten JSPI for GPU readback waits. The build also links a UI variant without
JSPI, and the loader selects it when stack switching is unavailable. Rendering
on that variant reports an error through Blender. Its existence does not establish
Safari compatibility.

To repeat browser checks against the server above:

```sh
node web/tests/native.cjs
WEB_TEST_UI_FALLBACK=1 node web/tests/native.cjs
node web/tests/features.cjs
WEB_TEST_UI_FALLBACK=1 node web/tests/features.cjs
node web/tests/render.cjs
```

The render check needs a hardware WebGPU adapter with the capabilities described
in the render document. The UI checks use Chromium's software WebGPU adapter.
The render check records a browser video, saves the original Render Result as a
PNG, compares scene settings before and after, and fails on WebGPU validation or
shader translation errors. Outputs are in `build/native/render-evidence`.

The UI check also exercises rapid window resizing, canvas layout changes,
zero-sized canvas recovery, and display scales of 1, 1.25 and 2. It checks
GHOST's logical dimensions through native Python, the editors' physical pixel
bounds, and the WebGPU backbuffer sizes. The running page must contain only the
Blender canvas. Both render and fallback bundles run these checks in CI.
Set `WEB_TEST_HARDWARE=1` to repeat the UI/resize check with the hardware Vulkan
adapter and save its evidence in `build/native/evidence-hardware`.
The same setting runs `features.cjs` on hardware and writes
`build/native/feature-evidence-hardware`, including the subdivision viewport.
Set `WEB_RENDER_DEVICE_SCALE=2` to check startup, native Render menu input and
Eevee rendering on a high-DPI display. `WEB_RENDER_REFERENCE` accepts a saved
PNG and requires an exact match of decoded pixels.
`WEB_RENDER_RESIZE_DURING_RENDER=1` also changes the window size three times
while the native GPU job is running and checks that the render completes.

OpenSubdiv is built from the release's pinned source with SIMD and pthreads.
Subdivision modifiers currently use its CPU evaluator. Feature checks cover
Catmull-Clark cubes at three levels, simple subdivision, an open plane, and
native viewport drawing. GPU subdivision and Cycles remain separate work.

Browser layout and display-density changes notify GHOST's resize callback.
GHOST retains CSS coordinates for window bounds and input, reports display
density as the native pixel size, and sends Blender's size/DPI events. The GPU
backend recreates its physical drawing buffers and surface at that size.
Zero-sized canvases retain their last usable buffers, and display density is
limited by the device's maximum texture dimension. Resize also releases cached
bind groups and alias snapshots that could retain resources at old sizes.

To profile a cold render followed by repeat renders in the same session:

```sh
WEB_RENDER_PROFILE=1 WEB_RENDER_REPEATS=3 \
  WEB_RENDER_OUTPUT=build/native/render-profile node web/tests/render.cjs
python3 web/tools/summarize_render_profile.py build/native/render-profile
```

Each run writes a Chromium CPU/timeline trace, WebGPU API counts and CPU call
costs, asynchronous map waits, and Linux GPU utilization samples when available.
Open `browser-trace-0.json` in Chromium DevTools' Performance panel. API call times
measure CPU overhead; they do not measure GPU execution time. Detailed tracing
adds overhead, so compare normal acceptance runs separately for user timings.
The first render uses an empty IndexedDB shader cache in a fresh browser context.
Repeat renders reuse translations and pipelines within that session.


The spinning-cube benchmark runs Blender's native Render Animation command for
50 frames at factory Eevee quality. It starts three fresh Chromium processes,
measures the first saved frame and each subsequent frame inside Blender, samples
browser CPU time and Linux GPU busy counters, and validates every saved PNG.
It adds linear Z-rotation keyframes to the factory cube and retains the camera,
light, resolution, samples, color management and PNG settings. Timings include
PNG output to the Wasm memory filesystem. Exports, image validation and video
encoding happen after timing, with screen recording disabled.

```sh
WEB_SPIN_RUNS=3 node web/tests/spinning-cube.cjs
python3 web/tools/spinning_cube_report.py build/native/spinning-cube \
  --json build/native/spinning-cube/report.json \
  --html build/native/spinning-cube/report.html \
  --csv build/native/spinning-cube/frames.csv
```

`WEB_SPIN_OUTPUT` selects another evidence directory. `WEB_SPIN_FRAMES` selects a
shorter diagnostic sequence; the baseline report requires 50 frames per run.
Add `--plot path/to/frame-times.svg` to export a static plot with Matplotlib
installed. Keep other CPU/GPU workloads idle for an isolated-machine result;
`conditions.json` in the evidence directory records any known measurement
conditions in the published report. GPU busy counters cover the whole device.
The measured benchmark and output animation are linked in the
[render documentation](../docs/eevee-default-scene.md).
