# USD CPU image and volume plugins

Candidate `f5bc7678d055b2921075` includes OpenUSD 26.03's original `hioOiio` and
`hioOpenVDB` CPU plugins. Both original factories passed standalone Wasm tests and
independent native Blender readback. The optimized application passed hardware
WebGPU, software WebGPU and fallback checks, USD scene exchange and offline
reopening. All 108 local stages and both source CI builds passed. Pages
publication is pending. See the [validation record](combined-dependencies-validation.json).

Watch the [optimized production recording](usd-materialx-walkthrough.mp4) and
[its metadata](usd-recording.json). The shorter clip below records the earlier
unoptimized integration probe.

The earlier browser probe reuses unchanged Blender objects from runtime commit `89179a7`,
links the original CPU plugin archives from `8e6d2ff`, and preloads their static
metadata. Its final Wasm link uses `-O0`. It provides integration evidence; it is
not a performance measurement or the production release binary.

Watch the [43-second hardware browser clip](usd-cpu-imaging-probe.mp4). It shows
the imported cube, Blender's original USD export menu and offline interchange.
Loading waits are cut, and the caption identifies the unoptimized probe.
[Recording metadata](usd-cpu-imaging-probe-recording.json) lists the source hash
and exact cuts.

| Original API | Tested operations |
| --- | --- |
| `HioImage` | Read a 7 by 5 float RGBA TIFF through plugin discovery, write another TIFF and read it back with exact pixels |
| `HioFieldTextureData` | Float, double, Vec3f and Vec3d OpenVDB grids, exact dense voxels, bounding boxes and linear transforms |
| Error recovery | Reject a missing grid and then read the valid grid successfully, with one expected warning and no USD errors |
| Native readback | Original OpenImageIO and OpenVDB bindings read both TIFFs and every exported grid and raw voxel buffer exactly |
| Cache restore | Rebuild from the verified source archive with no original USD extraction present, apply the monolith's existing Wasm ABI patch, then verify a cache hit |

The static plugin metadata uses USD's original empty library-path convention.
Both plugin types must register and load inside each browser runtime. The full
application acceptance also requires matching installed archive/header/metadata
hashes, the standalone factory results and native readback.

The cold-cache CI check caught an unpatched relative header include. Applying the
same checked USD patch to the restored source tree fixes the Wasm pointer-sized
`TfPyObjWrapper` layout. Installed headers alone cannot override every relative
include in upstream headers.

These tests do not provide Hydra graphics rendering, large-volume performance or
every image format. See the [operation evidence](usd-cpu-imaging-checks.json) and
[build instructions](../native/README.md).
