#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build both browser variants and publish a content-versioned static directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from prepare import prepare_source

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "web"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/web")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 2))
    args = parser.parse_args()
    if not shutil.which("emcmake"):
        parser.error("Activate Emscripten 4.0.23 with source <emsdk>/emsdk_env.sh first")
    pins = json.loads((WEB / "upstream.json").read_text())
    installed = subprocess.check_output(["emcc", "-dumpversion"], text=True).strip()
    if installed != pins["emscriptenVersion"]:
        parser.error(f"Expected Emscripten {pins['emscriptenVersion']}, found {installed}")
    build_dir = args.build_dir.resolve()
    blender_source = prepare_source(build_dir)
    dist = build_dir / "dist"
    artifacts = []
    for name, enabled in (("threaded", "ON"), ("single", "OFF")):
        target_dir = build_dir / name
        subprocess.run(["emcmake", "cmake", "-S", str(WEB), "-B", str(target_dir),
                        "-DCMAKE_BUILD_TYPE=Release", f"-DWEB_THREADS={enabled}",
                        f"-DBLENDER_ROOT={blender_source}"], check=True)
        subprocess.run(["cmake", "--build", str(target_dir), "--parallel", str(args.jobs)], check=True)
        files = sorted(p for p in target_dir.glob("ghost_web.*") if p.suffix in (".js", ".wasm"))
        if not files:
            raise RuntimeError(f"No build artifacts in {target_dir}")
        artifacts.extend((name, p) for p in files)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    digest = hashlib.sha256()
    digest.update((WEB / "upstream.json").read_bytes())
    for _, path in artifacts:
        digest.update(path.read_bytes())
    for path in sorted((WEB / "site").glob("*")):
        digest.update(path.read_bytes())
    version = digest.hexdigest()[:20]
    # Construct only after successful compilation, so failures preserve the previous site.
    if dist.exists():
        shutil.rmtree(dist)
    shutil.copytree(WEB / "site", dist)
    service_worker = dist / "sw.js"
    service_worker.write_text(service_worker.read_text().replace("__BUILD_VERSION__", version))
    manifest = {"version": version, "sourceRevision": revision,
                "blenderRevision": pins["blenderRevision"], "assets": []}
    for name, path in artifacts:
        destination = dist / "assets" / version / name / path.name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
        manifest["assets"].append(destination.relative_to(dist).as_posix())
    (dist / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Static site: {dist}\nBuild: {version}")


if __name__ == "__main__":
    main()
