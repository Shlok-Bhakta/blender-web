# Blender Web

Browser platform bring-up based on Blender 5.2.2 LTS.

[Live platform test](https://shlok-bhakta.github.io/blender-web/) · [Build instructions and current scope](web/README.md)

The current build compiles Blender's GHOST, logging, allocator and selected math code to WebAssembly. It renders a WebGPU cube test fixture and exercises resize/input events, CPU workers, IndexedDB persistence and offline caching. Blender's editor, scene evaluation and Workbench renderer are not running yet.

The `blender/` submodule pins official source. Web platform files live separately in `intern/ghost/web/`; `web/patches/ghost-web.patch` adds nine guarded integration lines to upstream code. The build applies that patch in an isolated worktree, keeping the reference source untouched.

With Emscripten 4.0.23 activated:

```sh
python3 web/tools/build.py
python3 web/tools/serve.py --pages-mode
```

Open `http://localhost:8080`. GitHub Actions builds both threaded and single-thread variants, runs browser checks, then deploys the static site to GitHub Pages.

Port code is licensed under GPL-2.0-or-later. Blender and its dependencies retain their upstream licenses.
