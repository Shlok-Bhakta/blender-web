# Original OSL compiler proof

Watch the [offline compiler recording](osl-compiler-walkthrough.mp4) or inspect
the [browser screenshot](osl-compiler-browser.png). This is the standalone
compiler proof, outside Blender's application build.

The original OSL 1.15.3.0 compiler runs in Node and offline Chromium. It uses
the original Clang and LLVM 20.1.8 libraries compiled to Wasm. Compilation
happens locally in the browser.

The [evidence](osl-compiler-probe.json) covers file compilation, memory-buffer
compilation, invalid-shader rejection and successful compilation after that
failure. Node and Chromium produce identical bytecode in all three successful
cases. Native Blender queries all six outputs and matches its own compiler's
nine parameters, defaults, types and metadata exactly.

Native Cycles also loads the browser-authored `.oso` directly, without
recompiling it. The shader multiplies a color by a float and sums array values
with an integer parameter. Its outputs drive an emission material's color and
strength. The native-authored and browser-authored shaders produce identical
16 by 16 floating-point EXR images, including center pixel `[2.5, 5, 7.5, 1]`.

This verifies browser shader compilation and native execution of its output.
Browser shader execution is covered by the [separate runtime proof](osl-runtime-probe.md).
Cycles integration, worker lifetime management and Python OSL query bindings
remain pending. The published application is
still `81aa3ca51ec1b21db65c`, with 62 production dependency recipes.

First build the [LLVM compiler proof](llvm-compiler-probe.md) and the production
Wasm dependency prefix. With the pinned SDK, native Blender, Flex, Bison and
m4 available, run:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_osl_compiler_probe.py \
    --blender /path/to/blender --jobs 4
```

`--build-dir`, `--llvm-build-dir`, `--prefix`, `--flex`, `--bison` and `--m4`
select existing builds and host tools. The command first reproduces the
[query proof](osl-query-probe.md), builds the original Wasm Clang libraries
and `oslc`, runs the offline checks, then compares metadata and renders using
native Blender. It writes source and artifact hashes into
`osl-compiler.acceptance.json` beside its build log and test outputs.

The standalone compiler uses a 128 MiB initial heap, a 2 GiB growth ceiling,
an 8 MiB stack and eight preloaded workers. These are proof settings, not
measurements of production scene capacity or render performance.

The compiler Wasm is 23,682,306 bytes. The separate query proof also tests
signed and unsigned 32-bit `long` diagnostic values at their limits. The
guarded source patch keeps those types distinct from the 64-bit hash type
and emits string constants as 64-bit LLVM constants on Wasm.
