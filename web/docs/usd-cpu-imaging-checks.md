# USD CPU image and volume plugins

The integration branch builds OpenUSD 26.03's original `hioOiio` and
`hioOpenVDB` plugins without enabling its GPU backends. Standalone Wasm factory
checks and independent native Blender 5.2.2 LTS readback passed. Editor CI passed;
the application build and browser integration checks are pending. These plugins
are not yet part of the published Pages bundle.

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
