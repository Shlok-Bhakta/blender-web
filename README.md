# Blender Web

Blender 5.2.2 LTS compiled to WebAssembly with its original C++ interface and a WebGPU backend.

[Open Blender Web](https://shlok-bhakta.github.io/blender-web/) · [Build instructions](web/native/README.md)

The browser build runs Blender's original editors, Python API, scene evaluation, and Workbench startup scene. Its frontend contains a canvas and loading status; Blender draws the interface. The initial target is desktop Chromium. Eevee renders the unchanged default cube scene at 1920×1080 and 64 samples. Safari, mobile input, additional windows, and Cycles still need work.

Activate Emscripten 4.0.23, then build and serve:

```sh
python3 web/tools/native_build.py --app --jobs 8 --check-deps
python3 web/tools/serve.py --directory build/native/dist --pages-mode
```

Open `http://localhost:8080`. GitHub Actions builds the original application, runs browser acceptance checks, and deploys the static files to GitHub Pages. The first visit downloads the application and runtime assets; a service worker caches versioned files and supplies cross-origin isolation for pthreads. User files use browser OPFS storage.

The `blender/` submodule pins the official stable release. Platform code lives in `intern/ghost/web/` and `web/native/`. Guarded patches in `web/patches/` connect it to upstream in an isolated build worktree, keeping the reference checkout clean. Native C++ uses optimization, SIMD, pthreads, and oneTBB.

[Watch the first Eevee render](web/docs/eevee-default-scene.mp4) · [Render details and validation](web/docs/eevee-default-scene.md)

![Original Blender UI running in Chromium](web/docs/native-ui.png)

The older [platform fixture](web/README.md) remains available separately. Its JavaScript interface is not part of the native application.

Port code is GPL-2.0-or-later. Blender and dependencies retain their upstream licenses. Backend and recipe attribution is recorded in [THIRD_PARTY.md](web/native/THIRD_PARTY.md).
