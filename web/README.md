# Legacy browser platform fixture

The deployed application now uses [Blender’s original C++ UI](native/README.md).
This document describes the earlier platform fixture, built separately with
`web/tools/build.py`.

This fixture pins the released Blender 5.2.2 LTS tag in the `blender/` submodule and
`upstream.json`. It builds a small browser integration test from upstream GHOST,
clog, guardedalloc and selected Blender math code. A browser editor interface
controls the WebGPU cube fixture through a Wasm API. The fixture does not run Blender’s original native UI or scene evaluation.

The test proves the platform path before cross-compiling the editor's larger
dependency graph. `platform_test.cc` is a separate entry point, not a replacement
for `source/creator`.

## Build and run

The public repository keeps the port files and a pinned Blender submodule.
The build initializes that submodule without downloading binary LFS assets, then
applies `web/patches/ghost-web.patch` in a separate generated worktree at
`build/web/source`. The reference submodule stays untouched. This keeps Blender's
asset history on its official upstream and makes our integration patch reviewable.

Install the SDK revision recorded in `upstream.json`, then install and activate
Emscripten 4.0.23 with `emsdk install 4.0.23` and `emsdk activate 4.0.23`.

```sh
source /path/to/emsdk/emsdk_env.sh
python3 web/tools/build.py
python3 web/tools/serve.py --directory build/web/dist --pages-mode
```

Open `http://localhost:8080`. Drag or touch-drag to orbit, scroll to zoom, and
press Home while the canvas is focused to reset the view. Front, Right and Top
select orthographic views. Frame object centers the object, including translated
or scaled objects. The plus/minus controls provide touch zoom.

The outliner selects and hides the cube. Object properties rename it and edit
location, Euler rotation in degrees and scale. These fields change the Wasm transform used
to draw the object. Edit > Undo/Redo applies to object edits. The inspector and
outliner dividers accept pointer dragging or arrow keys; panel size changes also
generate GHOST resize events. On phones, Inspector opens the panels beneath the
viewport, leaving the canvas accessible for touch navigation.

File > Save preview explicitly flushes the object, camera, display settings and
panel layout through the Wasm filesystem to IndexedDB. Restore saved preview
loads that snapshot. Download/Open preview use a versioned `.blender-web.json`
format, with validation before changing the scene. These files are preview data,
not `.blend` files. Saved views from the first platform build can still be restored.

Use `?threads=2` to limit the pool, or `?threads=0` to select the separate
single-thread module. The default uses the browser's reported logical core count,
up to 32 workers. Workers sleep on a condition variable when idle. The CPU check
distributes a deterministic checksum calculation across all selected workers and
reports completion without blocking the browser thread.

For a phone on the same network, serve with `--bind 0.0.0.0` behind HTTPS.
Plain HTTP on a LAN address does not qualify as a secure browser context.
Use Safari on iOS/iPadOS 26 or newer with WebGPU support. Chromium mobile emulation
does not establish Safari compatibility; physical iOS testing remains necessary.

## Current implementation

* `intern/ghost/web/` owns the new browser platform implementation. The patch to
  Blender's code/build files contains only nine added lines: a guarded include/factory call and
  a CMake include. No upstream code was removed or rewritten.
* The real `GHOST_ISystem::createSystem` factory selects `WEB`. Browser events feed
  the upstream event queue and dispatcher. ResizeObserver and DPR changes update
  pixel bounds and generate GHOST window-size events.
* Pointer capture supports drag events beyond canvas bounds. Focus loss and
  pointer cancellation clear held input. Unsupported native capabilities are not
  advertised. A second GHOST window fails explicitly for now.
* `web/site/editor.js` owns the browser controls, object labels, history and layout.
  `web/platform_test.cc` owns the fixture's transforms, camera and rendering flags.
  The UI adapter is separate from Blender's native editor startup and operators.
* The preview renderer uses baseline WebGPU capabilities, persistent GPU buffers,
  and three 4×4 matrices per redraw. It draws a Z-up grid and object edges, handles
  negative/nonuniform scale, and recreates depth textures on resize
  and reports GPU loss. It redraws on demand and pauses while hidden. Raster DPR
  is capped at 2 and bounded by the device's texture-size limit.
* Both Wasm variants use `-O3`, link-time optimization and baseline SIMD. Initial
  Wasm memory is 64 MiB, with growth up to 1 GiB. This is a safety bound for the
  platform test, not a tested memory budget for the full editor.
* IDBFS mounts `/user` before application startup. Save explicitly waits for the
  IndexedDB flush. Storage failures disable local persistence while retaining rendering
  and JSON import/export.
  Browser storage is device-local and may be evicted; this does not provide
  durable project-file storage or `.blend` saving yet.

The test's main function runs on the browser thread and never blocks. For the
full editor, move application execution to a worker, proxy DOM/window calls to
the browser thread, and preserve GHOST queue ownership. Rendering this small test
on the main thread is not evidence that the editor can use the same arrangement.

## Static hosting and caching

`build/web/dist/` contains the static site. Wasm and generated JavaScript live in
content-versioned directories; both variants share one build identifier. The
service worker caches the shell, then caches only the requested variant. Warm
reloads and offline reloads reuse the cache instead of downloading the binaries.
Cache names include the Pages project scope. Failed cache writes do not prevent
startup, and activation removes previous caches for this project. An updated
worker waits for existing tabs to close before replacing their build.

GitHub Pages does not expose configurable COOP/COEP response headers. The service
worker supplies `same-origin` COOP and `require-corp` COEP to same-origin responses.
The initial visit can reload once after worker installation. Runtime checks
select pthreads only when cross-origin isolation and SharedArrayBuffer actually
work. If isolation is unavailable, the separately compiled single-thread module
remains usable. First visits still transfer assets; client caching cannot remove
bandwidth costs for new users, evicted caches, or new builds.

`.github/workflows/web-pages.yml` builds both variants, runs browser acceptance
checks against a host without isolation headers, uploads evidence, and deploys
the resulting directory on branch pushes. The public destination is
[Shlok-Bhakta/blender-web](https://github.com/Shlok-Bhakta/blender-web).
Its Pages source must be GitHub Actions. The Blender submodule points to the
official Blender server; web work is published to this repository's `origin`.

## Verification

With the local server running in Pages mode:

```sh
npm ci --prefix web/tests
cd web/tests
npx playwright install --with-deps chromium
npm test
```

The browser checks rendered pixels, GHOST resizing and pointer input, idle redraw,
actual Wasm worker calculations, numeric transforms and undo/redo, visibility,
orthographic views, panel resizing, project persistence across module reloads,
JSON import/export and invalid-import rejection. It also checks offline UI startup
and restoration, plus a fresh non-isolated mobile context using the single-thread
module with touch zoom and inspector controls. Screenshots and results are written
to `build/web/evidence/`. CI uses
software Vulkan for correctness; these tests measure no hardware performance and
do not run Safari.

## Remaining work to render Blender's actual default scene

The WebGPU test renderer is not a `blender::gpu::GPUBackend`. The editor still
needs implementations of the GPU resource classes, synchronization, state,
shader compilation/translation, and the operations required by its drawing path.
Putting this work in GHOST would duplicate responsibilities already assigned to
Blender's GPU layer. Keep the implementation in a separate backend directory and
use guarded factory hooks, as with GHOST.

Inspection of `source/blender/windowmanager/intern/wm_init_exit.cc` confirms that
the original UI starts after operator/editor registration, font initialization,
startup-file loading and GPU initialization. Linking GHOST alone cannot run those
panels. The browser controls deliberately expose only the fixture's working
operations; native mesh editing, modifiers, the timeline and Python panels require
the actual editor dependency graph.

A full-editor CMake probe on this checkout first failed because `startup.blend`
was a Git LFS pointer. Fetching that specific asset resolved the check. The next
observed failure is a missing target JPEG library in `platform_unix.cmake`.
Desktop precompiled libraries cannot link into Wasm. An isolated Emscripten
platform configuration and cross-built image, compression, font, math and tasking
dependencies are still needed. Host-side code generators must run natively rather
than being compiled to Wasm. Python and nonessential integrations can stay
disabled for initial scene bring-up; that is not full feature parity.

For that probe, fetch only the startup asset from inside `blender/` using
`git lfs fetch origin --include=release/datafiles/startup.blend`, then check it out
inside the generated source worktree with `git lfs checkout
release/datafiles/startup.blend`. Avoid downloading all upstream LFS assets until
the editor needs them.

GHOST system paths still need a web implementation backed by the virtual
filesystem. Editor startup and its event loop need guarded hooks for worker
execution and asynchronous GPU/device initialization. Multi-window support needs
user-initiated popup creation, shared application state, per-window surfaces,
event routing, and a fallback when popups are blocked. Opening independent copies
of this test would not implement Blender's multiple windows.

The next editor milestone is a cross-compiled minimal creator executable plus
the necessary WebGPU resource and shader path, loading the upstream startup
scene. Its acceptance check must distinguish a scene evaluated by Blender from
this test fixture.
