# Browser feature coverage

The application uses an explicit [web configuration](config.cmake), independent
of Blender's desktop lite preset. Features are restored and verified in
batches. The static site's
`manifest.json` records the actual CMake `buildOptions` for its binary.

| Feature | Configuration | Verification or remaining work |
| --- | --- | --- |
| Original editors, Python, Eevee, color management and image IO | Enabled | Original UI, Python, storage and default-scene render checks |
| OBJ, STL and PLY | Enabled | Native cube export/import, geometry and bounds checks |
| FBX import | Enabled | Native ufbx importer, independently authored triangle fixture |
| Standard IK and iTaSC | Enabled | Two-bone chain follows two target positions with each solver |
| Bullet rigid bodies | Enabled | 100-frame fall and floor collision |
| Native `.blend` persistence | Enabled | Save physics scene to OPFS and reopen after browser reload |
| Device file picker, uploads and downloads | Deferred | Native file IO currently sees WasmFS; connect user-selected device files and exported results to browser APIs |
| Automatic `.blend` thumbnails | Deferred | Synchronous preview generation hangs outside a browser GPU job; a guarded GHOST save hook skips capture and restores the user's preview preference |
| FBX Python exporter and NumPy/process-dependent add-ons | Deferred | Port NumPy, validate extension modules and adapt process-dependent code to browser workers; native FBX import is separate |
| OpenSubdiv CPU | Enabled | Catmull-Clark cubes at levels 1–3, simple subdivision, open boundaries and native viewport drawing |
| OpenSubdiv GPU | Deferred | Patch-basis shaders and evaluator/draw validation for WebGPU; the platform selects CPU evaluation |
| GMP, Manifold, remeshing and UV SLIM | Deferred | Compile dependencies and validate the corresponding geometry operations |
| OpenVDB, NanoVDB, fluids and ocean simulation | Deferred | Port volume/FFT dependencies and measure memory use on real scenes |
| USD, Alembic, Hydra and MaterialX | Deferred | Port dependencies, file IO and applicable rendering integration |
| Audio, FFmpeg, video and time stretching | Deferred | Codec libraries plus browser audio/device integration |
| Cycles CPU | Deferred | Wasm kernel/dependency work and image correctness checks |
| Cycles GPU, denoising and OSL | Deferred | Separate device/backend and compiler work; Eevee WebGPU does not supply a Cycles device |
| Tracking, Freestyle and remaining image/vector formats | Deferred | Build dependencies and exercise actual operations |
| Translation, text shaping and IME | Deferred | HarfBuzz/FriBidi and browser text input integration |
| Additional windows, NDOF devices and XR | Deferred | Browser platform/device integration |

The next dependency batches should start with GMP and Manifold for geometry
operations, then NumPy for Python tools and add-ons. After that, prioritize
OpenVDB/NanoVDB and FFTW for volumes and simulation, followed by Alembic and USD
for scene exchange. Each batch needs real operation and file round-trip checks.
FFmpeg/audio also needs browser playback integration. Cycles CPU is a separate
kernel port, and Cycles GPU needs a WebGPU device implementation. Larger downloads
alone do not enable these features. The next batches remain unported.

The restored Bullet, IK and importer code is already part of the pinned Blender
source. It compiles with the existing Wasm SIMD, pthread and exception settings.
No feature-specific edits to upstream algorithms are required for this group.
The save hook temporarily disables automatic preview capture, which the browser
UI cannot synchronously complete. Native scene serialization, overwrite backups
and persistent storage use Blender's writer. The integration check saves twice,
checks that the preview preference is restored, then reloads the browser and
opens the file again.
Shared platform and GPU adapters remain in GHOST and the WebGPU backend.

Run `node web/tests/features.cjs` against the locally served application. It
writes the native Python results, browser log and screenshots to
`build/native/feature-evidence`. Repeat with `WEB_TEST_UI_FALLBACK=1` to exercise
the bundle without JSPI; its results go to `build/native/feature-evidence-ui`.
Compilation alone does not establish runtime coverage for arbitrary production files, rigs or simulations.
Set `WEB_FEATURE_PROFILE=1` to capture a Chromium V8 CPU trace in the evidence
directory while diagnosing a stalled operation.


The [verified build report](../docs/web-features.acceptance.json) records bundle
`4ef171e2e987d290773b` under Chromium 145.0.7632.6. Both render and fallback UI
bundles passed six native subdivision fixtures, import/export, IK, Bullet and
save/reopen checks on SwiftShader. The render bundle also passed on AMD gcn-5,
including drawing a subdivided cube with the GPU subdivision preference enabled.
GHOST selects the CPU evaluator until WebGPU subdivision shaders are supported.

The UI checks passed rapid resizes, canvas-only layout changes, zero-height
recovery, and display densities of 1, 1.25 and 2 on both bundles. The render
bundle also passed those checks on the hardware adapter. They compare GHOST's
logical window size, Blender's physical editor bounds and UI scale, and WebGPU
backbuffer sizes. Each resize must draw the selected cube before further input.
After startup, the page contains only Blender's canvas. The local server now
opens the native application by default; the old preview fixture needs an
explicit directory.

The hardware Eevee check starts at 2× display density and sends three window-size
changes while the native GPU job is running. Its factory 1920×1080, 64-sample PNG
matches the earlier reference pixel for pixel, with unchanged scene and render
settings. The [browser recording](../docs/eevee-hidpi-resize.mp4) shows that check.
These are functional regression checks, not a new repeated performance benchmark.

The render Wasm grew from 109,574,040 to 110,074,709 bytes, an increase of
500,669 bytes, or 0.48 MiB and 0.46%. Runtime data size is unchanged. Dependency
license notices are stored in the cached install prefix and published with the
site; a check also restores a prefix without its dependency sources.

OpenSubdiv uses the release's pinned dependency and original CPU evaluator.
The upstream edits add a guarded GHOST capability call and omit unused OpenGL
includes for the web build. GHOST reports CSS window bounds and native pixel
density, while its acquire hook sizes GPU buffers before each draw. The WebGPU
backend releases cached bind groups and alias snapshots when buffers change size
and when their context is disposed.

Validation uses desktop Chromium with the repository's WebGPU launch flags.
Other browsers and mobile devices still need validation. Remaining dependency
and integration work is listed in the matrix above.

![Native subdivision in Blender on the hardware WebGPU adapter](../docs/web-subdivision.png)

![Blender after resizing to a 2× display density](../docs/web-hidpi.png)

![Physics scene reopened from OPFS after a browser reload, at frame 1](../docs/web-features-reopened.png)
