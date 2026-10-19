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
| NumPy and FBX Python exporter | Enabled | Static NumPy extensions, linear algebra, FFT, both random APIs, RNA array transfer and FBX export/native import round trip |
| Process-dependent add-ons and extension marketplace | Deferred | Native subprocesses, multiprocessing and arbitrary dynamic extensions are unavailable; the marketplace downloader currently reports a Python startup error |
| OpenSubdiv CPU | Enabled | Catmull-Clark cubes at levels 1–3, simple subdivision, open boundaries and native viewport drawing |
| OpenSubdiv GPU | Deferred | Patch-basis shaders and evaluator/draw validation for WebGPU; the platform selects CPU evaluation |
| GMP and Manifold Boolean solvers | Enabled | Both solvers: union, intersection and difference with volume, bounds and closed-mesh checks |
| Octree remeshing, QuadriFlow and UV SLIM | Enabled | Smooth/sharp remesh, closed quad topology and minimum-stretch UV unwrap |
| FFTW and Ocean modifier | Enabled | Float/double pthread FFT checks; native ocean animation, foam and UVs compared with desktop at three frames |
| Alembic scene exchange | Enabled | Ogawa animation in both directions with desktop Blender, mesh deformation, UVs, parent transforms and persistent offline reopen |
| OpenVDB, NanoVDB and fluids | Deferred | Port volume dependencies and measure memory use on real scenes |
| USD, Hydra and MaterialX | Deferred | Port dependencies, file IO and applicable rendering integration |
| Audio, FFmpeg, video and time stretching | Deferred | Codec libraries plus browser audio/device integration |
| Cycles CPU final render | Enabled in JSPI bundle | 128×128, 16-sample factory cube compared with the pinned desktop release |
| Cycles progressive display and rendered viewport | Deferred | CPU final renders publish the completed Render Result; live display still needs browser-safe Python/GPU waits |
| Cycles GPU, denoising and OSL | Deferred | Separate device/backend and compiler work; Eevee WebGPU does not supply a Cycles device |
| Tracking, Freestyle and remaining image/vector formats | Deferred | Build dependencies and exercise actual operations |
| Translation, text shaping and IME | Deferred | HarfBuzz/FriBidi and browser text input integration |
| Additional windows, NDOF devices and XR | Deferred | Browser platform/device integration |

GMP 6.3.0, Manifold 3.5.2 and NumPy 2.3.4 follow the pinned release's
versions and checksums. NumPy uses its original numeric code as static CPython
extensions. Its linear algebra build uses bundled routines without BLAS.
The shared Wasm SIMD and pthread settings also apply to these libraries.

Cycles uses its CPU kernels and worker pool. Native CUDA, HIP, Metal and oneAPI
devices are disabled. Eevee's WebGPU backend does not supply a Cycles GPU device.
The session controller stays on the UI worker, which owns browser GPU handles.
CPU final renders write through Blender's original output driver, then the Image
Editor displays the completed result. Progressive GPU display is disabled because
CPython's JavaScript function-call trampoline cannot suspend for GPU waits.
Selecting a Cycles rendered viewport reports an unsupported-mode error.
The WebGPU binding adapter also handles the final single-level mipmap dispatch.
Blender binds its unused second output to the first output's texture; WebGPU
requires distinct writable bindings, so the adapter supplies unused storage for
that second output. The 128-pixel Cycles Image Editor check exercises this case.

The next dependency groups are OpenVDB/NanoVDB for volumes and simulation,
followed by USD for scene exchange. Audio and FFmpeg also
need browser playback integration. Device file selection and download/export
integration remain separate from the persistent browser filesystem.

FFTW 3.3.10 builds its original float and double libraries with pthread support.
The Ocean modifier uses Blender's original simulation and evaluation code.
Alembic 1.8.3 uses the original Ogawa archive implementation and the existing
Imath dependency. Neither feature needs an upstream algorithm patch.
HDF5 archives, ocean disk-cache baking and other Alembic schema types remain
outside this acceptance check. The tested ocean has 676 vertices and 625 faces,
with animation samples at frames 1, 6 and 12. Coordinate and UV comparisons
use an absolute tolerance of 0.0002 against the same pinned desktop release.

[Watch the ocean and Alembic browser recording](../docs/ocean-alembic-walkthrough.mp4).
It shows the cached ocean in Blender's viewport, timeline scrubbing, then an
offline reload and reopening the same animation from OPFS.

<!-- OCEAN_RECORDING_CHAPTERS -->
| Time | What to inspect |
| --- | --- |
| 0:00 | Blender starts with FFTW ocean simulation and Alembic enabled |
| 0:16 | Check ocean waves and exchange animated Alembic files with desktop Blender |
| 0:21 | Alembic ocean cache in the original Blender viewport |
| 0:31 | Scrub the cached animation from frame 1 through frame 12 |
| 0:39 | Reload offline and reopen the Alembic cache from persistent browser storage |
<!-- /OCEAN_RECORDING_CHAPTERS -->

![Alembic ocean animation reopened offline](../docs/web-ocean-alembic.png)

Run
`node web/tests/ocean-alembic.cjs` for the render variant,
`WEB_TEST_UI_FALLBACK=1 node web/tests/ocean-alembic.cjs` for the fallback,
and `WEB_TEST_HARDWARE=1 node web/tests/ocean-alembic.cjs` for hardware capture.
The fixtures and desktop regeneration command live in
`web/tests/fixtures/ocean-alembic/README.txt`. To verify a browser export in the
pinned desktop executable, run:

```sh
blender -b --factory-startup --python-exit-code 1 \
  --python web/tests/verify_ocean_desktop.py -- build/native/ocean-evidence-hardware
```

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


[Watch the browser walkthrough](../docs/modeling-cycles-walkthrough.mp4).
It shows both Boolean solvers cutting a cylinder through a cube, QuadriFlow's
quad edges, a NumPy-generated wave, saving that wave as a native `.blend`,
reloading offline and reopening it, then a Cycles CPU render.

<!-- WEB_RECORDING_CHAPTERS -->
| Time | What to inspect |
| --- | --- |
| 0:00 | Loading Blender; runtime files are cached for later visits |
| 0:23 | Original Blender UI and default cube |
| 0:31 | GMP exact Boolean: cylinder cut through a cube |
| 0:39 | Manifold Boolean: the same cylinder cut |
| 0:46 | Native QuadriFlow remeshing: visible quad edges |
| 0:52 | NumPy builds a 1,600-vertex wave inside Blender |
| 0:59 | Save a native .blend file to persistent browser storage |
| 1:03 | Offline reload: cached Blender starts with no network |
| 1:18 | Reopen the saved NumPy wave after an offline reload |
| 1:30 | Cycles CPU render: 128 x 128, 16 samples. Checked against desktop Blender. |
<!-- /WEB_RECORDING_CHAPTERS -->

The [acceptance report](../docs/web-features.acceptance.json) records the tested
bundle, exact runtime hashes, browser adapters, modeling results, rendering
comparisons and recording checksum. Its build uses the original pinned Blender
5.2.2 release. Both render and fallback UI bundles exercise native import/export,
FBX round trips, IK, Bullet, subdivision, Booleans, remeshing, UV SLIM, NumPy and
save/reopen. The render bundle also runs those operations on the AMD adapter.
Its `oceanAlembic` section contains the three browser variants and the independent
desktop check of the browser's exported animation. Its current recording entry
refers to the ocean/Alembic walkthrough. The earlier modeling/Cycles recording
above documents the preceding feature batch.

Resize checks cover rapid changes, zero-height recovery and display densities
of 1, 1.25 and 2. They compare GHOST's logical window size, Blender's editor
bounds and UI scale, and WebGPU backbuffer sizes. Each resize must draw the cube
before further input. The page contains only Blender's canvas after startup.

The hardware Eevee check uses the factory 1920×1080, 64-sample scene, starts at
2× display density and changes window size during the GPU job. Its saved PNG is
compared pixel for pixel with the earlier reference. The Cycles CPU check uses
128×128, 16 samples, seed 0, with denoising and adaptive sampling disabled. Its
saved PNG is compared with the same settings in desktop Blender 5.2.2. These are
functional checks; host activity makes their timings unsuitable as an isolated
performance benchmark.

The selected runtime is about 193.3 MiB before HTTP compression, including
Python, NumPy and Blender's data. Only one browser variant downloads. The two
variants currently have identical Wasm bytes, so their content-hash cache entry
can also be shared. The [cache report](../docs/loading-cache.acceptance.json)
checks cold, warm, offline and browser-restart offline startup for both variants.

![GMP exact Boolean in the browser](../docs/web-modeling-exact.png)

![NumPy-generated geometry in Blender](../docs/web-modeling-numpy.png)

![Cycles CPU result in Blender's Image Editor](../docs/cycles-browser.png)

OpenSubdiv uses the release's pinned dependency and original CPU evaluator.
The upstream edits add a guarded GHOST capability call and omit unused OpenGL
includes for the web build. GHOST reports CSS window bounds and native pixel
density, while its acquire hook sizes GPU buffers before each draw. The WebGPU
backend releases cached bind groups and alias snapshots when buffers change size
and when their context is disposed.

Validation uses desktop Chromium with the repository's WebGPU launch flags.
Other browsers and mobile devices still need validation. Remaining dependency
and integration work is listed in the matrix above.

The [native/browser comparison](../docs/native-web-performance.md) records three
runs per workload on the same machine, including a 2.35-million-face CPU
subdivision case, ocean animation, Cycles CPU and the unchanged Eevee cube.
It separates CPU scene-memory growth from the larger browser baseline and
startup peaks. The configured Wasm32 heap limit remains 4 GiB; these checks do
not establish desktop-scale RAM parity or interactive viewport frame rate for
the largest evaluated mesh.

![Native subdivision in Blender on the hardware WebGPU adapter](../docs/web-subdivision.png)

![Blender after resizing to a 2× display density](../docs/web-hidpi.png)

![Physics scene reopened from OPFS after a browser reload, at frame 1](../docs/web-features-reopened.png)
