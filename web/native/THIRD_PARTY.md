Browser GPU code and shader/Python build recipes adapted from
[HeyPuter/blender](https://github.com/HeyPuter/blender/tree/6b031d3d41c392883e3c495aa72343e10d15b43d)
and [HeyPuter/blender-wasm](https://github.com/HeyPuter/blender-wasm/tree/f8f811929f8a5bc218e04944cf2dff25eeeccbf3),
under GPL-2.0-or-later. Original file notices are retained.
The Blender source remains pinned to the official stable release in `web/upstream.json`.

Tracking uses the pinned Blender release's Abseil 20250814.1 archive and Ceres
commit `0c70ed3a1a2d6ba47c06c7e8b3b040880bc474db`, with original checksums.
Abseil retains its Apache-2.0 notices and Ceres its BSD-3-Clause notices, both
published under `licenses/dependencies/`. Bundled libmv source notices and the
gflags/glog licenses are published under `licenses/`. These libraries and
Blender's original tracking code are compiled without algorithm substitutions.

The expanded web configuration also compiles Blender's bundled Bullet physics,
IK solvers and ufbx importer. Their original source notices are retained in the
pinned source. Bullet uses the zlib license; ufbx is available under its included
MIT license or public-domain dedication. The browser license directory includes
the ufbx license and Bullet's source license notice.

OpenSubdiv is built from Blender's pinned release dependency and retains its
upstream notices and license. Packaging copies those files from the dependency
source into the static site's license directory.

GMP, Manifold and NumPy use the versions and source checksums specified by the
pinned Blender release. Their licenses, including NumPy's bundled notices, are
stored under the dependency prefix and published in `licenses/dependencies/`.
NumPy's original numeric implementations are linked as static CPython modules.
The build patch changes Meson's extension target type and isolates legacy random
helper names so the modules can share one Wasm binary. Cycles' CPU code is part
of the pinned Blender source, under its existing Apache-2.0 notices.

FFTW 3.3.10 and Alembic 1.8.3 also use the pinned Blender release's source
archives and checksums. Both float and double FFTW libraries are static builds
with pthread support, under GPL-2.0-or-later. Alembic uses its original Ogawa
reader/writer and existing Imath dependency, under BSD-3-Clause. HDF5 and
external DCC plug-ins are disabled. Their original license files are published
under `licenses/dependencies/fftw/` and `licenses/dependencies/alembic/`.

OpenVDB and NanoVDB 13.0.0 and Blosc 1.21.1 use the pinned Blender release's
archives and checksums. OpenVDB/NanoVDB retain their Apache-2.0 source notices;
Blosc retains its BSD notices and the notices for its bundled codecs. These
static Wasm builds use the existing pthread TBB, zlib and Zstandard libraries.
Delayed loading and the separate Python OpenVDB module are disabled. Their
installed notices are published under `licenses/dependencies/`.
Blender's bundled Mantaflow solver retains its original license and source
notices; the site publishes its `LICENSE` as `licenses/mantaflow-LICENSE.txt`.

The bundled WGSL seed contains original Blender shaders translated by the
pinned compiler pipeline. It uses GPL-2.0-or-later, with compiler recipe hashes
and capture provenance recorded in `shader-seed.json`.

WebP 1.6.0, OpenJPEG 2.5.3, PugiXML 1.10, libharu 2.4.5 and Potrace 1.16
use Blender's pinned source archives and checksums. Their original notices are
published under `licenses/dependencies/`, including WebP's `PATENTS` grant.
OpenImageIO embeds the actual WebP and JPEG 2000 codec plugins. libharu uses
Emscripten's math library and the existing PNG/zlib builds. Potrace uses
Blender's own dependency CMake recipe. Freestyle retains its notices in the
unchanged, pinned Blender source.

The OpenImageIO patch also preserves the DPX reader's negative buffer-size
markers on 32-bit targets by converting to a signed 64-bit value before
multiplication. Without that conversion, RGBA DPX reads request nearly 4 GiB.

MaterialX 1.39.4 and OpenUSD 26.03 use Blender's pinned archives and checksums.
Their original licenses are published under `licenses/dependencies/materialx/`
and `licenses/dependencies/usd/`. MaterialX's bundled PugiXML 1.9 retains its
MIT notice and uses private symbols in the static build, alongside Blender's
PugiXML 1.10. OpenUSD's license file includes its bundled third-party notices.
Its original C++ libraries and Python extension modules are static Wasm
builds. The build patches keep Wasm exception handling consistent, fix two
32-bit assumptions, isolate module wrapper names and package schema resources
with Blender's preloaded data. CPU imaging adapters provide primitive-shape
geometry through OpenSubdiv. A Wasm-only shader-registry declaration avoids
Clang's incorrect destructor callback for static reference temporaries, tracked
in [LLVM #226619](https://github.com/llvm/llvm-project/pull/226619). The standalone
USD check runs Python finalization and process exit to cover this callback.
Hydra rendering and native graphics backends are
disabled in this scene-exchange build.

The media build uses Blender's pinned FFmpeg 8.1, libsndfile 1.2.2, Ogg 1.3.5,
Vorbis 1.3.7, FLAC 1.4.2, Opus 1.3.1, LAME 3.100, Theora 1.1.1, libvpx 1.15.2,
and x264 commit 35fe20d1ba49918ec739a5b068c208ca82f977f7. Their archive checksums
come from the release's dependency definitions. The static FFmpeg build enables
GPL codecs, including x264, and retains its upstream license notices alongside
each codec's notices in `licenses/dependencies/`. Audaspace 1.9.0 comes from
Blender's bundled source under Apache-2.0; the site includes its `LICENSE`.
The web audio device and AudioWorklet bridge live in the GHOST web overlay.
The x264 overlay adds a generic Wasm target and uses Emscripten's logical core
count for automatic encoder threading.

HarfBuzz 10.0.1 and FriBidi 1.0.12 use the release's pinned source archives and
checksums. Their original notices are published in `licenses/dependencies/`.
The HarfBuzz build overlay uses Clang's `-fignore-exceptions` for Emscripten,
keeping the shared Wasm exception ABI while omitting throws from HarfBuzz.
FriBidi's original Unicode-data generators run as host tools during its Meson
cross build. Blender's original translation catalogs and language list retain
the notices in the pinned Blender source. No separate gettext implementation
is substituted for Blender's message-catalog compiler or runtime reader.

Draco 1.5.7 and meshoptimizer 1.1 use the pinned release's ZIP archives and
checksums. They link statically with Blender's original compression bridges.
Draco retains its Apache-2.0 notices and meshoptimizer its MIT notices in
`licenses/dependencies/`. The glTF Python adapter lives in the GHOST overlay
and calls only these compiled functions. It does not provide dynamic loading
or a general-purpose ctypes module.

Rubber Band 4.0.0 uses Blender's pinned source archive and its FFTW backend.
It retains GPL-2.0-or-later notices. libaom 3.13.1 retains its BSD-2-Clause
license and patent notices; x265 4.1 retains GPL-2.0-or-later notices. All three
are static Wasm libraries built with pthreads and SIMD compiler flags. Their
portable C/C++ implementations are used; x86/ARM assembly is disabled.

Embree 4.4.1 uses Blender's pinned ZIP archive and Apache-2.0 notices. The
upstream Emscripten path lowers its SSE2 kernels to Wasm SIMD, with the existing
oneTBB port for task scheduling. ISPC and SYCL are disabled. A build-only patch
selects the same Wasm exception ABI as Blender; the SSE2 compiler options omit
an x86-only negative feature flag rejected by Emscripten.

OpenPGL 0.7.1 and Open Image Denoise 2.5.0 use Blender's pinned source archives
and Apache-2.0 notices. OpenPGL uses its original CPU-4 kernels and bundled
Embree helpers, with private helper namespaces to avoid collisions with Embree
4. Its missing Wasm emulation header comes from pinned Embree 4.4.1 and retains
that header's copyright. OIDN uses its original RT weights and generic ISPC
kernels. Its Wasm compiler bridge uses the checksum-pinned official ISPC 1.30.0
host binary and existing Imath binary16 conversions. Their original notices
are included with the dependency notices. See [the compiler bridge notes](oidn/README.md)
for the 32-bit adaptation and numerical checks. Native GPU denoisers and
RTLightmap weights are not part of this build.
