# Browser feature coverage

The application uses an explicit [web configuration](config.cmake), independent
of Blender's desktop lite preset. This is the first expansion toward a full
browser build, not a claim of desktop feature parity. The static site's
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
| GMP, Manifold, OpenSubdiv, remeshing and UV SLIM | Deferred | Compile dependencies and validate the corresponding geometry operations |
| OpenVDB, NanoVDB, fluids and ocean simulation | Deferred | Port volume/FFT dependencies and measure memory use on real scenes |
| USD, Alembic, Hydra and MaterialX | Deferred | Port dependencies, file IO and applicable rendering integration |
| Audio, FFmpeg, video and time stretching | Deferred | Codec libraries plus browser audio/device integration |
| Cycles CPU | Deferred | Wasm kernel/dependency work and image correctness checks |
| Cycles GPU, denoising and OSL | Deferred | Separate device/backend and compiler work; Eevee WebGPU does not supply a Cycles device |
| Tracking, Freestyle and remaining image/vector formats | Deferred | Build dependencies and exercise actual operations |
| Translation, text shaping and IME | Deferred | HarfBuzz/FriBidi and browser text input integration |
| Additional windows, NDOF devices and XR | Deferred | Browser platform/device integration |

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
`86de92adb2a96198e4ab` under Chromium 145.0.7632.6. Both render and fallback UI
bundles passed all feature checks, including save, overwrite backup and reopen
after reload. The largest measured IK endpoint error was 0.00032 Blender units.
The rigid body's center fell from Z=4 to Z=0.5 and remained on the floor through
frame 100.

Both original UI checks also passed resizing, native Python, persistent storage
and offline reload. The 1920×1080, 64-sample Eevee output matches the earlier
saved PNG pixel for pixel. The render Wasm grew from 107,830,788 to 109,574,040
bytes, an increase of 1.66 MiB or 1.62%. These are functional regression checks,
not a new repeated performance benchmark.

Validation uses desktop Chromium with the repository's WebGPU launch flags.
Feature/UI checks use SwiftShader; the Eevee check uses AMD gcn-5 hardware.
Other browsers and mobile devices still need validation.

![Physics scene reopened from OPFS after a browser reload, at frame 1](../docs/web-features-reopened.png)
