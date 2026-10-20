# Render optimization measurements

Three fresh Chromium sessions per build, alternated control/candidate. Three renders per session, no video/tracing, original factory settings. First-in-process engine handler time; global GPU driver caches are not cleared.

| Build | First render, median | Warm render, median |
| --- | ---: | ---: |
| control | 7.801 s | 2.999 s |
| candidate | 4.486 s | 3.037 s |

First-render time reduction: 42.5%. Warm-render median change: +1.3%, where positive means slower.

Every run retained the original scene settings and matched the prior browser reference pixels exactly.

The candidate bundles common original shader translations in its preload data. Compiler/source fingerprints guard the seed; unseen variants still compile and cache at runtime.

Queue batching and shaderc performance optimization experiments were rejected. See [experiment evidence](render-optimization-experiments.json) and [full measurements](render-optimization.json).
