# Upstream Blender editor compilation

This target builds the original Blender source through its own top-level CMake
project. It is separate from the deployed GHOST/WebGPU fixture in `web/site`.
The initial targets are the original RNA, interface, screen, window manager,
font, and GHOST libraries. Compiling these archives does not produce a runnable UI.
The native WebGPU backend and browser startup loop still need implementation.

Activate the Emscripten version in `web/upstream.json`, then run:

```sh
python3 web/tools/native_build.py --jobs 8
```

The script prepares `build/native/source`, downloads and verifies dependency
sources against the versions and hashes in Blender's release, builds static
Wasm libraries with pthreads and SIMD, and configures Blender in
`build/native/editor`. Dependencies install into `build/native/deps/prefix`.
Each dependency has a `build.log` in its directory. Successful dependency
builds are cached with their source, recipe, toolchain, and SDK version.

To resume editor compilation without checking dependencies again:

```sh
python3 web/tools/native_build.py --skip-deps
```

`--configure-only` stops after CMake generation; `--targets` selects upstream
targets. The default checkpoint builds the original generators, then uses
CMake's Makefile `/fast` rules to compile the editor archives without building
all recursively linked libraries. The initial configuration uses Blender's lite preset and disables
Python, desktop window systems, and desktop graphics backends. Python panels,
Cycles, and the remaining optional dependencies will need later builds.
oneTBB remains enabled for Blender's own parallel task execution.
Release compilation uses `-O3`, Wasm SIMD, pthreads, and Wasm exceptions.
The C and C++ dependencies use the same exception/longjmp ABI.

`web/patches/native-web.patch` contains guarded integration hooks.
Platform configuration and generator integration live here and are copied into
`intern/ghost/web/native` in the generated worktree. The reference submodule
is not edited. Dependency platform patches live in `patches/`.

A guarded packing hook in `DNA_customdata_types.h` keeps the mesh-data mask
record on the four-byte boundaries used by 32-bit DNA. Wasm32's default
eight-byte alignment otherwise inserts unrepresented padding into `Scene`.
The hook is implemented in `intern/ghost/web/GHOST_WebDNA.hh` and has no effect
on Wasm64. The generated member-offset and struct-size assertions remain enabled.
This checkpoint does not yet validate `.blend` file round trips.
The `ResourceScope` size assertion uses a separate guarded ABI helper so its
two-pointer invariant also holds on Wasm32.

Blender's `datatoc`, `makesdna`, `makesrna`, and `shader_tool` are compiled for
the target architecture and run with Node and Emscripten's host filesystem
access during the build. This preserves target pointer sizes and alignment in
generated DNA. Host filesystem access is confined to those build tools.

The dependency check in `tests/` exercises oneTBB on multiple pthreads, PNG
encoding/decoding through OpenImageIO and the virtual filesystem, OpenColorIO,
and Blender's actual WOFF2 font through FreeType/Brotli. It runs under Node;
browser rendering and iOS Safari require separate verification once the native
renderer can draw the UI.

Run the dependency checks with:

```sh
python3 web/tools/native_build.py --skip-deps --configure-only --check-deps
```

The native editor workflow runs this check and compiles the archives on GitHub
Actions. It uploads the archives, dependency results, and
`editor-compilation.json`, which records object counts, Wasm32 verification,
archive hashes, shim hashes, and the number of original DNA layout assertions.
Pages deployment remains in the separate preview workflow.

For subsequent GPU work, the [HeyPuter WebGPU implementation](https://github.com/HeyPuter/blender/tree/6b031d3d41c392883e3c495aa72343e10d15b43d/source/blender/gpu/webgpu)
is a potential source of reusable code. That revision targets 5.3 alpha and
needs a compatibility review against this release and its guarded hooks.
