#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile Blender's upstream editor libraries for Wasm, separately from the preview."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from native_deps import build as build_dependencies
from prepare import ROOT, prepare_source

EDITOR_ARCHIVES = ["bf_rna", "bf_editor_interface", "bf_editor_screen", "bf_windowmanager",
                   "bf_blenfont", "bf_intern_ghost"]

def check_dependencies(source, base, prefix, jobs):
    checks = base / "checks"
    subprocess.run(["emcmake", "cmake", "-S", str(ROOT / "web/native/tests"), "-B", str(checks),
                    f"-DBLENDER_SOURCE={source}",
                    f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'web/native/toolchain.cmake'}",
                    f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", str(checks), "--parallel", str(jobs)], check=True)
    result = subprocess.run(["node", str(checks / "dependencies.js")], check=True,
                            text=True, stdout=subprocess.PIPE, timeout=120)
    print(result.stdout, end="", flush=True)
    (base / "dependency-check.json").write_text(json.dumps(json.loads(result.stdout), indent=2) + "\n")


def prepare_native(base):
    source = prepare_source(base)
    patch = ROOT / "web/patches/native-web.patch"
    check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=source, capture_output=True)
    if check.returncode == 0:
        subprocess.run(["git", "apply", str(patch)], cwd=source, check=True)
    elif subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                        cwd=source, capture_output=True).returncode:
        raise RuntimeError(f"Native shims do not apply cleanly:\n{check.stderr.decode()}")
    shutil.copytree(ROOT / "web/native", source / "intern/ghost/web/native", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__"))
    # Only fetch startup data, not the entire multi-gigabyte upstream LFS store.
    startup = "release/datafiles/startup.blend"
    if (source / startup).stat().st_size < 1024:
        subprocess.run(["git", "lfs", "fetch", "origin", f"--include={startup}"], cwd=source, check=True)
        subprocess.run(["git", "lfs", "checkout", startup], cwd=source, check=True)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/native")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--skip-deps", action="store_true", help="Use already compiled dependencies")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--check-deps", action="store_true", help="Run Wasm dependency checks under Node")
    parser.add_argument("--targets", nargs="+", help="Override the editor archive compilation checkpoint with upstream targets")
    args = parser.parse_args()
    if not shutil.which("emcmake"):
        parser.error("Activate the pinned Emscripten SDK first")
    pins = json.loads((ROOT / "web/upstream.json").read_text())
    version = subprocess.check_output(["emcc", "-dumpversion"], text=True).strip()
    if version != pins["emscriptenVersion"]:
        parser.error(f"Expected Emscripten {pins['emscriptenVersion']}, found {version}")
    base = args.build_dir.resolve()
    source = prepare_native(base)
    if not args.skip_deps:
        build_dependencies(argparse.Namespace(build_dir=base / "deps", jobs=args.jobs, packages=[]))
    prefix = base / "deps/prefix"
    os.environ["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    os.environ["PKG_CONFIG_PATH"] = ""
    if args.check_deps:
        check_dependencies(source, base, prefix, args.jobs)
    subprocess.run(["emcmake", "cmake", "-G", "Unix Makefiles", "-S", str(source), "-B", str(base / "editor"),
                    "-C", str(source / "intern/ghost/web/native/config.cmake"),
                    f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'web/native/toolchain.cmake'}",
                    f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], check=True)
    if not args.configure_only:
        command = ["cmake", "--build", str(base / "editor"), "--parallel", str(args.jobs), "--target"]
        if args.targets:
            subprocess.run([*command, *args.targets], check=True)
            print(f"Compiled upstream targets: {', '.join(args.targets)}")
        else:
            subprocess.run([*command, "datatoc", "shader_tool", "makesrna"], check=True)
            # Make's upstream /fast rules compile each original archive without
            # recursively building every linked editor/render library first.
            # bf_rna runs the original generator rules before the UI uses them.
            for target in EDITOR_ARCHIVES:
                subprocess.run([*command, f"{target}/fast"], check=True)
            archives = []
            readobj = Path(shutil.which("emcc")).resolve().parent.parent / "bin/llvm-readobj"
            for target in EDITOR_ARCHIVES:
                path = base / "editor/lib" / f"lib{target}.a"
                headers = subprocess.check_output([str(readobj), "--file-headers", str(path)], text=True)
                formats = [line for line in headers.splitlines() if line.startswith("Format:")]
                architectures = [line for line in headers.splitlines() if line.startswith("Arch:")]
                if not formats or set(formats) != {"Format: WASM"} or set(architectures) != {"Arch: wasm32"}:
                    raise RuntimeError(f"Expected an archive of Wasm32 objects: {path}")
                archives.append({"target": target, "path": str(path.relative_to(base)),
                                 "objects": len(formats), "architecture": "wasm32",
                                 "bytes": path.stat().st_size,
                                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
            report = {"blenderRevision": pins["blenderRevision"], "emscriptenVersion": version,
                      "browserRunnable": False, "archives": archives,
                      "dnaLayoutAssertions": (base / "editor/source/blender/makesdna/intern/dna_verify.cc").read_text().count("BLI_STATIC_ASSERT("),
                      "shimHashes": {name: hashlib.sha256((ROOT / "web/patches" / name).read_bytes()).hexdigest()
                                     for name in ("ghost-web.patch", "native-web.patch")}}
            (base / "editor-compilation.json").write_text(json.dumps(report, indent=2) + "\n")
            print("Original Blender editor archives compiled. Browser UI startup and the native WebGPU backend remain unfinished.")


if __name__ == "__main__":
    main()
