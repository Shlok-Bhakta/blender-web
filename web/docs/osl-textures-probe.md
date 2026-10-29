# Original OSL textures on browser workers

Watch the [recording](osl-textures-walkthrough.mp4), inspect the
[screenshot](osl-textures-browser.png), or read the
[evidence](osl-textures-probe.json). Original OSL samples an EXR texture through
original renderer services and OpenImageIO in Node and offline Chromium.
This is a standalone test. The published Blender application remains
`81aa3ca51ec1b21db65c`, with 62 production dependency recipes.

The test writes a 2×2 float EXR through OpenImageIO, then samples it from generated
OSL code at 7,296 shading invocations per environment. Nearest samples
match the independently specified red, green, blue and white pixels within
`1e-6`. The shader also checks diffuse/emission/debug closure weights, normal
parameters and full 64-bit hashes in formal and keyword closure parameters.
Two compile requests, four existing workers, newly started workers, callback
replacement and cooperative cancellation run across eight native OSL owners.
All 14,592 renderer calls and 7,296 libffi callbacks complete. Owned module
records and static allocations are released; table capacity is reused.

Texture IO exposed a browser scheduling requirement. A synchronous C++ main
blocked the browser from servicing startup messages after ImageIO exhausted
the twelve prestarted workers. Condition variables alone could not service
those JavaScript events. The new executable runs C++ main on a worker and
leaves the browser runtime thread available. The tests require actual worker
creation beyond the initial pool. Ownership and table checks explicitly proxy
to the runtime thread, which owns generated-module allocations.

The browser compiles its own fresh bytecode with original OSL/Clang and executes
it offline using WasmFS. There are no page errors. A native executable runs the
same texture, closure, string and callback checks against Blender's installed
libraries. Native-, Node- and browser-authored bytecode each pass all 7,296
invocations. Native libffi headers and code come from the qualified pinned
archive. The EXR fixture is independently written in each native run.

The build reuses the original libraries from the [closure proof](osl-closures-probe.md),
verifying its source and archive hashes. It does not rebuild those libraries or
mutate their qualified sources. The shader executable uses an unoptimized debug
link, a 128 MiB initial heap, a 4 GiB Wasm32 ceiling, eight MiB thread stacks and
twelve prestarted worker slots with dynamic growth. It is not a performance
benchmark or a large-scene memory test.

This establishes nearest 2D EXR texture sampling for the tested shader.
Filtered derivatives, other texture types, the full Cycles closure catalog,
interactive parameters, compiler cancellation, variable-size module reuse,
Python query bindings and the full Cycles adapter remain pending.

After reproducing the closure proof, activate the pinned SDK and run:

```sh
PLAYWRIGHT_BROWSERS_PATH=/path/to/browsers \
  python3 web/tools/native_osl_textures_probe.py \
    --blender /path/to/blender --jobs 4
```

`--osl-build-dir` selects the qualified closure libraries and native libffi.
`--compiler-build-dir`, `--llvm-build-dir` and `--prefix` select prerequisites.
`--build-dir` selects a separate shader executable and evidence directory.
The command records offline compilation/execution, verifies all three bytecode
variants with native OSL and records source, library and artifact hashes.
