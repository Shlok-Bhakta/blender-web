Browser GPU code and shader/Python build recipes adapted from
[HeyPuter/blender](https://github.com/HeyPuter/blender/tree/6b031d3d41c392883e3c495aa72343e10d15b43d)
and [HeyPuter/blender-wasm](https://github.com/HeyPuter/blender-wasm/tree/f8f811929f8a5bc218e04944cf2dff25eeeccbf3),
under GPL-2.0-or-later. Original file notices are retained.
The Blender source remains pinned to the official stable release in `web/upstream.json`.

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
