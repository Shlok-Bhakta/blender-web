# Native and browser Blender performance

The [machine-readable results](native-web-performance.json) compare the installed
Blender 5.2.2 LTS with the browser build on the same machine. Both run the same
Python workload, use the same NumPy version, evaluate CPU subdivision, and use
eight threads for Cycles CPU rendering. Video capture is disabled for timings.
Eevee renders the unchanged 1920 by 1080, 64-sample factory cube on the hardware
GPU. Native Eevee and the WebGPU port use different graphics APIs. Saved images
are compared and both adapter descriptions are recorded. First-run means the
first render in that process; global GPU driver caches are not cleared.

Each workload runs three times in one process. The JSON includes all times,
medians, first-run times and warm-run medians. Mesh construction includes NumPy
arrays, RNA bulk transfer, topology construction and mesh update. Subdivision
includes creating the native modifier, evaluating the dependency graph and
obtaining the evaluated mesh. These CPU measurements do not measure interactive
viewport frame rate.

Ocean evaluation uses resolution 16 and twelve animation times. Cycles uses
the factory cube at 128 by 128, 16 samples, seed 0, CPU device, with denoising
and adaptive sampling disabled. Both builds time the interval from Blender's
`render_pre` handler to `render_post`. The tiny render is a repeatable baseline,
not a substitute for production-scene testing. Native CPU ISA and acceleration
libraries differ from the current Wasm build.

The separate [browser profile](native-web-profile.json) retains WebGPU call
counts and sampled CPU hotspots. It is excluded from the baseline timings.
First-render shader translation consumed about 5.99 seconds of sampled CPU time.
The warm render recorded no shader translation and averaged 82.26% GPU busy,
with a 100% peak. These samples support focusing first-render work on translation
and warm-render work on the GPU backend. They do not establish the cause of
every difference from the native OpenGL backend.

Memory is Linux proportional set size, sampled every 200 milliseconds. The
native measurement covers Blender and its children. The browser measurement
covers the owned Chromium process tree, including its UI, renderer, GPU process
and JavaScript workers. It excludes the Node test driver and sampler. Shared
pages are counted proportionally rather than added repeatedly as RSS. Sampling
has some overhead and can miss brief peaks. Native runs in background mode, so
the results include baseline memory and scene growth above that baseline.
The table separates the workload peak from the whole-process peak that includes
startup. The browser's startup peak occurs before the baseline scene measurement.
PSS does not fully account for driver-managed GPU memory; this is a CPU scene
memory comparison, not a complete VRAM measurement.

The browser's linear memory capacity and physical memory are different numbers.
This build starts with a 512 MiB Wasm memory and can grow to 4 GiB. The browser
also allocates JavaScript, compiled code, GPU resources and process overhead
outside that heap. Freed Blender allocations can be reused, but the Wasm memory
buffer cannot shrink. This port has not established desktop-scale RAM parity.

<!-- PERFORMANCE_RESULTS -->
| Workload | Native median | Browser median | Browser/native |
| --- | ---: | ---: | ---: |
| Mesh construction, 128 by 128 grid | 0.017 s | 0.038 s | 2.18x |
| CPU subdivision, 128 by 128 grid | 0.054 s | 0.088 s | 1.64x |
| Mesh construction, 256 by 256 grid | 0.063 s | 0.132 s | 2.09x |
| CPU subdivision, 256 by 256 grid | 0.181 s | 0.270 s | 1.50x |
| Mesh construction, 384 by 384 grid | 0.149 s | 0.287 s | 1.92x |
| CPU subdivision, 384 by 384 grid | 0.458 s | 0.567 s | 1.24x |
| Ocean 12-frame evaluation | 0.819 s | 0.883 s | 1.08x |
| Cycles CPU render | 0.023 s | 0.083 s | 3.55x |
| Eevee GPU render | 1.070 s | 3.173 s | 2.96x |

| Process-tree memory | Native | Browser |
| --- | ---: | ---: |
| Baseline | 268.5 MiB | 1256.0 MiB |
| Workload peak | 653.1 MiB | 1594.5 MiB |
| Workload peak above baseline | 384.6 MiB | 338.5 MiB |
| After cleanup | 355.2 MiB | 1594.5 MiB |
| Whole-process peak, including startup | 653.1 MiB | 2123.9 MiB |

Tested bundle `7e35e48849404ebeac92`, measured 2026-10-19T03:44:08.600423+00:00.
<!-- /PERFORMANCE_RESULTS -->

Reproduce after the application build finishes, with other substantial work idle:

```sh
python3 web/tools/native_performance.py --blender /path/to/blender
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers node web/tests/performance.cjs
python3 web/tools/native_performance.py --blender /path/to/blender --cycles
WEB_CYCLES_REPEATS=3 WEB_CYCLES_BENCHMARK=1 node web/tests/cycles.cjs
python3 web/tools/native_performance.py --blender /path/to/blender --eevee
WEB_RENDER_OUTPUT=build/native/performance-eevee WEB_RENDER_TIMING=1 \
  WEB_RENDER_REPEATS=3 WEB_RENDER_BENCHMARK=1 node web/tests/render.cjs
python3 web/tools/compare_performance.py
```

The browser server defaults to `http://127.0.0.1:8093/`; override it with
`WEB_TEST_URL`. Native logs, per-stage memory readings and 200 ms memory/CPU
samples are saved under `build/native/performance-native/` and
`build/native/performance-browser/`. Cycles browser results are under
`build/native/cycles-evidence/`.
