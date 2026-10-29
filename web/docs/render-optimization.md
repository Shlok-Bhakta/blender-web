# Render optimization measurements

Three fresh Chromium sessions per build, alternated control/candidate. Three renders per session, no video/tracing, original factory settings. First-in-process engine handler time; global GPU driver caches are not cleared. This compares complete packaged builds: enabled dependencies, debug-name metadata and compiler configuration may differ, so it does not isolate a single cache or optimizer change.

| Build | First render, median | Warm render, median |
| --- | ---: | ---: |
| control | 7.679 s | 3.179 s |
| candidate | 4.407 s | 3.070 s |

First-render time reduction: 42.6%. Warm-render median change: -3.4%, where positive means slower.

Every run retained the original scene settings and matched the prior browser reference pixels exactly.

This is a comparison of complete packaged builds, including their dependency and compiler configuration differences. It does not isolate a single optimization. The candidate bundles common original shader translations in its preload data. Compiler/source fingerprints guard the seed; unseen variants still compile and cache at runtime.

Queue batching and shaderc performance optimization experiments were rejected. See [experiment evidence](render-optimization-experiments.json) and [full measurements](render-optimization.json).
