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
| Mesh construction, 128 by 128 grid | 0.017 s | 0.036 s | 2.20x |
| CPU subdivision, 128 by 128 grid | 0.049 s | 0.080 s | 1.64x |
| Mesh construction, 256 by 256 grid | 0.063 s | 0.133 s | 2.10x |
| CPU subdivision, 256 by 256 grid | 0.172 s | 0.277 s | 1.61x |
| Mesh construction, 384 by 384 grid | 0.153 s | 0.300 s | 1.96x |
| CPU subdivision, 384 by 384 grid | 0.413 s | 0.579 s | 1.40x |
| Ocean 12-frame evaluation | 0.840 s | 0.769 s | 0.92x |
| Cycles CPU render | 0.024 s | 0.079 s | 3.25x |
| Eevee GPU render | 1.075 s | 3.137 s | 2.92x |

| Process-tree memory | Native | Browser |
| --- | ---: | ---: |
| Baseline | 272.4 MiB | 1355.2 MiB |
| Workload peak | 658.8 MiB | 1689.2 MiB |
| Workload peak above baseline | 386.4 MiB | 334.0 MiB |
| After cleanup | 358.8 MiB | 1689.2 MiB |
| Whole-process peak, including startup | 658.8 MiB | 2140.4 MiB |

Tested bundle `d274910ed5f9aab67b3f`, measured 2026-10-20T01:30:28.235290+00:00.
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
