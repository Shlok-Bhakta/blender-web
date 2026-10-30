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
counts, GPU timestamps and resource samples. This release omits C++ function
names, so unavailable native CPU attribution is reported explicitly. Named C++
profiling builds can enable `BLENDER_WEB_DEBUG_NAMES`. Profile timings are
excluded from the baseline.
GPU timestamp queries measure individual passes separately from JavaScript API
call costs. Resource samples include GPU utilization and available clock readings.
The [optimization experiments](render-optimization-experiments.json) retain the
earlier profile and rejected shader/queue changes. These local measurements do
not establish the cause of every difference from native rendering.

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
The separate [memory64 probe](memory64.md) verifies addressing above 4 GiB;
production Blender still uses wasm32.

The [render optimization comparison](render-optimization.md) measures first and
warm renders across three fresh browser sessions per build. Bundling common
shader translations reduces startup work. The profiling evidence keeps GPU
pass times separate from CPU API-call times and from uninstrumented benchmarks.

<!-- PERFORMANCE_RESULTS -->
| Workload | Native median | Browser median | Browser/native |
| --- | ---: | ---: | ---: |
| Mesh construction, 128 by 128 grid | 0.018 s | 0.037 s | 2.14x |
| CPU subdivision, 128 by 128 grid | 0.049 s | 0.090 s | 1.83x |
| Mesh construction, 256 by 256 grid | 0.058 s | 0.146 s | 2.51x |
| CPU subdivision, 256 by 256 grid | 0.172 s | 0.268 s | 1.56x |
| Mesh construction, 384 by 384 grid | 0.155 s | 0.331 s | 2.14x |
| CPU subdivision, 384 by 384 grid | 0.463 s | 0.607 s | 1.31x |
| Ocean 12-frame evaluation | 0.853 s | 0.900 s | 1.05x |
| Cycles CPU render | 0.025 s | 0.092 s | 3.61x |
| Eevee GPU render | 1.071 s | 3.139 s | 2.93x |

| Process-tree memory | Native | Browser |
| --- | ---: | ---: |
| Baseline | 269.6 MiB | 2215.9 MiB |
| Workload peak | 652.6 MiB | 2559.7 MiB |
| Workload peak above baseline | 383.0 MiB | 343.8 MiB |
| After cleanup | 351.9 MiB | 2559.7 MiB |
| Whole-process peak, including startup | 652.6 MiB | 2559.7 MiB |

Tested bundle `6fc613eabfc47ac917c2`, measured 2026-10-30T02:47:30.385368+00:00.
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
