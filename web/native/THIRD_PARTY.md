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
