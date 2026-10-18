#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the original Blender application or editor archives for WebAssembly."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess

from native_deps import build as build_dependencies
from prepare import ROOT, prepare_source
from native_package import stage, collect
from native_app_deps import build as build_application_dependencies

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


def check_image_cache(base, prefix):
    commands = json.loads((base / "editor/compile_commands.json").read_text())
    original = next(item for item in commands if item["file"].endswith("/blenkernel/intern/idtype.cc"))
    command = shlex.split(original["command"])
    obj = base / "image-cache-check.o"
    command[command.index("-o") + 1] = str(obj)
    command[-1] = str(ROOT / "web/native/tests/image_cache.cc")
    subprocess.run(command, cwd=original["directory"], check=True)
    libraries = base / "editor/lib"
    executable = base / "image-cache-check.js"
    subprocess.run(["em++", "-O2", "-pthread", "-msimd128", "-fwasm-exceptions", str(obj),
        str(base / "editor/source/blender/blenkernel/CMakeFiles/bf_blenkernel.dir/intern/idtype.cc.o"),
        "-Wl,--start-group", str(libraries / "libbf_blenlib.a"),
        str(libraries / "libbf_intern_guardedalloc.a"), str(libraries / "libbf_intern_clog.a"),
        str(prefix / "lib/libtbb.a"), "-Wl,--end-group", "-sENVIRONMENT=node",
        "-sEXIT_RUNTIME=1", "-sSTACK_SIZE=8388608", "-o", str(executable)], check=True)
    output = subprocess.check_output(["node", str(executable)], text=True, timeout=60)
    (base / "image-cache-check.json").write_text(json.dumps(json.loads(output), indent=2)+"\n")
    print(output, end="", flush=True)


def check_shader_processor(base, source):
    host_shader = base / "host/shader_tool"
    shader_source = source / "source/blender/gpu/shader_tool"
    host_signature = hashlib.sha256(b"".join(path.read_bytes() for path in
        sorted(shader_source.rglob("*")) if path.suffix in (".cc", ".hh"))).hexdigest()
    host_stamp = base / "host/shader_tool.sha256"
    if not host_shader.exists() or not host_stamp.exists() or host_stamp.read_text() != host_signature:
        host_shader.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["g++", "-std=c++20", "-O2", "-I", str(shader_source),
                        *map(str, sorted(shader_source.glob("*.cc"))),
                        *map(str, sorted((shader_source / "lexit").glob("*.cc"))),
                        "-o", str(host_shader)], check=True)
        host_stamp.write_text(host_signature)
    objects = base / "editor/source/blender/gpu/CMakeFiles/bf_gpu.dir/shader_tool"
    executable = base / "shader-scope-check.js"
    subprocess.run(["em++", "-O2", "-pthread", "-msimd128", "-fwasm-exceptions",
        *map(str, sorted(objects.rglob("*.cc.o"))),
        str(base / "editor/source/blender/gpu/shader_tool/CMakeFiles/shader_tool.dir/shader_tool.cc.o"),
        "-sENVIRONMENT=node", "-sNODERAWFS=1", "-sALLOW_MEMORY_GROWTH=1",
        "-sSTACK_SIZE=8388608", "-sEXIT_RUNTIME=1", "-o", str(executable)], check=True)
    cases = ["source/blender/compositor/shaders/library/gpu_shader_function_transpose_matrix.glsl",
             "source/blender/gpu/shaders/gpu_shader_text_vert.glsl"]
    includes = [source / "source/blender/gpu" / directory for directory in
                ("", "intern", "shaders", "shaders/common", "shaders/infos", "shaders/material")]
    for index, name in enumerate(cases):
        outputs = {}
        for architecture, command in (("wasm", ["node", str(executable)]),
                                       ("host", [str(base / "host/shader_tool")])):
            directory = base / "shader-check" / str(index) / architecture
            directory.mkdir(parents=True, exist_ok=True)
            files = [directory / name for name in ("output.glsl", "metadata.hh", "infos.hh", "output.d")]
            subprocess.run([*command, str(source / name), *map(str, files), *map(str, includes)], check=True, timeout=60)
            outputs[architecture] = [path.read_bytes() for path in files[:3]]
        if outputs["host"] != outputs["wasm"]:
            raise RuntimeError(f"Shader processor differs between host and Wasm: {name}")
    report = {"shaderProcessorCases": len(cases), "hostMatchesWasm": True}
    (base / "shader-check.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report), flush=True)


def build_ui_fallback(base, jobs):
    """Relink the same objects without stack switching for browsers lacking JSPI."""
    binary = base / "editor/bin"
    saved = base / "render-binary"
    saved.mkdir(exist_ok=True)
    files = [path for path in binary.glob("blender.*") if path.suffix in (".js", ".wasm", ".data")]
    for path in files:
        shutil.copy2(path, saved / path.name)
    subprocess.run(["cmake", "-S", str(base / "source"), "-B", str(base / "editor"),
                    "-DBLENDER_WEB_JSPI=OFF"], check=True)
    try:
        subprocess.run(["cmake", "--build", str(base / "editor"), "--parallel", str(jobs),
                        "--target", "blender"], check=True)
        if (binary / "blender.data").read_bytes() != (saved / "blender.data").read_bytes():
            raise RuntimeError("UI and render variants must share identical preload data")
        for path in files:
            if path.suffix != ".data":
                shutil.copy2(path, binary / path.name.replace("blender.", "blender-ui.", 1))
    finally:
        subprocess.run(["cmake", "-S", str(base / "source"), "-B", str(base / "editor"),
                        "-DBLENDER_WEB_JSPI=ON"], check=True)
        for path in files:
            shutil.copy2(saved / path.name, path)


def check_switch_lowering(base):
    generator = base / "shader-switch-fixture"
    generated = base / "shader-switch-generated.cc"
    check = base / "shader-switch-check"
    subprocess.run(["g++", "-std=c++20", "-O2", str(ROOT / "web/native/tests/shader_switch.cc"), "-o", str(generator)], check=True)
    generated.write_text(subprocess.check_output([str(generator)], text=True))
    subprocess.run(["g++", "-std=c++20", "-O2", str(generated), "-o", str(check)], check=True)
    result = subprocess.check_output([str(check)], text=True)
    (base / "shader-switch-check.json").write_text(result)
    print(result, end="", flush=True)


def check_shader_adaptation(base):
    source = ROOT / "web/native/tests/shader_adaptation.cc"
    host = base / "shader-adaptation-host"
    wasm = base / "shader-adaptation.js"
    subprocess.run(["g++", "-std=c++20", "-O2", str(source), "-o", str(host)], check=True)
    subprocess.run(["em++", "-std=c++20", "-O2", str(source), "-sENVIRONMENT=node",
                    "-sEXIT_RUNTIME=1", "-o", str(wasm)], check=True)
    expected = subprocess.check_output([str(host)], text=True, timeout=60)
    actual = subprocess.check_output(["node", str(wasm)], text=True, timeout=60)
    if expected != actual:
        raise RuntimeError("Shader adaptation differs between host and Wasm")
    (base / "shader-adaptation-check.json").write_text(actual)
    print(actual, end="", flush=True)


def check_shader_translation(base, prefix):
    dawn = base / "gpu-deps/src/dawn"
    libraries = base / "gpu-deps/build"
    executable = base / "shader-translation-check.js"
    tint = sorted((libraries / "tint/src/tint").glob("libtint*.a"))
    absl = sorted((libraries / "tint/third_party/abseil/absl").glob("*/libabsl*.a"))
    subprocess.run(["em++", "-O2", "-std=c++20", "-pthread", "-msimd128", "-fwasm-exceptions",
        f"-I{dawn}", f"-I{libraries / 'tint'}", f"-I{prefix / 'include'}",
        str(ROOT / "web/native/tests/shader_translation.cc"),
        "-Wl,--start-group", *map(str, tint), *map(str, absl),
        str(libraries / "spirv-tools/source/opt/libSPIRV-Tools-opt.a"),
        str(libraries / "spirv-tools/source/libSPIRV-Tools.a"),
        str(prefix / "lib/libshaderc_combined.a"), "-Wl,--end-group",
        "-sENVIRONMENT=node", "-sEXIT_RUNTIME=1", "-sSTACK_SIZE=8388608",
        "-sALLOW_MEMORY_GROWTH=1", "-o", str(executable)], check=True)
    result = subprocess.run(["node", str(executable)], text=True, capture_output=True, timeout=60, check=True)
    (base / "shader-translation-check.json").write_text(json.dumps(json.loads(result.stdout), indent=2)+"\n")
    print(result.stdout, end="", flush=True)


def check_python(base, prefix):
    executable = base / "python-check.js"
    subprocess.run(["em++", "-O2", "-pthread", "-msimd128", "-fwasm-exceptions",
        f"-I{prefix / 'include/python3.13'}", f"-I{prefix / 'include'}", str(ROOT / "web/native/tests/python.cc"),
        "-Wl,--start-group", *map(str, sorted((prefix / "lib/blender-web-numpy").glob("*.a"))),
        str(prefix / "lib/libpython3.13.a"), str(prefix / "lib/libexpat.a"),
        str(prefix / "lib/libz.a"), "-Wl,--end-group", "-sUSE_BZIP2=1",
        "-sENVIRONMENT=node", "-sEXIT_RUNTIME=1", "-sALLOW_MEMORY_GROWTH=1",
        "-sSTACK_SIZE=8388608", "-sWASMFS=1", "-sFORCE_FILESYSTEM=1", "--preload-file",
        f"{base / 'assets/python3.13'}@/python/lib/python3.13", "-o", str(executable)], check=True)
    output = subprocess.check_output(["node", str(executable)], cwd=base, text=True, timeout=60)
    (base / "python-check.json").write_text(json.dumps(json.loads(output), indent=2)+"\n")
    print(output, end="", flush=True)


def check_expressions(base, source, prefix):
    library = base / "editor/lib"
    executable = base / "expression-check.js"
    subprocess.run(["em++", "-O2", "-pthread", "-fwasm-exceptions", "-msimd128",
        f"-I{source / 'source/blender/blenlib'}", str(ROOT / "web/native/tests/expression.cc"),
        "-Wl,--start-group", str(library / "libbf_blenlib.a"),
        str(library / "libbf_intern_guardedalloc.a"), str(library / "libbf_intern_clog.a"),
        str(prefix / "lib/libtbb.a"), "-Wl,--end-group", "-sENVIRONMENT=node",
        "-sEXIT_RUNTIME=1", "-sALLOW_MEMORY_GROWTH=1", "-sSTACK_SIZE=8388608",
        "-o", str(executable)], check=True)
    output = subprocess.check_output(["node", str(executable)], text=True, timeout=60)
    (base / "expression-check.json").write_text(json.dumps(json.loads(output), indent=2)+"\n")
    print(output, end="", flush=True)


def prepare_native(base, full_assets=False):
    source = prepare_source(base)
    patch = ROOT / "web/patches/native-web.patch"
    check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=source, capture_output=True)
    if check.returncode == 0:
        subprocess.run(["git", "apply", str(patch)], cwd=source, check=True)
    elif subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                        cwd=source, capture_output=True).returncode:
        raise RuntimeError(f"Native shims do not apply cleanly:\n{check.stderr.decode()}")
    for patch_name in ("native-ui.patch", "native-portability.patch", "native-render.patch", "native-files.patch", "native-modeling.patch", "native-cycles.patch", "native-python-clock.patch"):
        ui_patch = ROOT / "web/patches" / patch_name
        if not ui_patch.exists():
            continue
        applied = subprocess.run(["git", "apply", "--check", str(ui_patch)], cwd=source, capture_output=True)
        if applied.returncode == 0:
            subprocess.run(["git", "apply", str(ui_patch)], cwd=source, check=True)
        elif subprocess.run(["git", "apply", "--reverse", "--check", str(ui_patch)], cwd=source, capture_output=True).returncode:
            raise RuntimeError(applied.stderr.decode())
    shutil.copytree(ROOT / "web/native", source / "intern/ghost/web/native", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__"))
    # Only fetch startup data, not the entire multi-gigabyte upstream LFS store.
    startup = "release/datafiles/startup.blend"
    if (source / startup).stat().st_size < 1024:
        subprocess.run(["git", "lfs", "fetch", "origin", f"--include={startup}"], cwd=source, check=True)
        subprocess.run(["git", "lfs", "checkout", startup], cwd=source, check=True)
    if full_assets:
        asset_stamp = base / "runtime-assets.revision"
        revision = json.loads((ROOT / "web/upstream.json").read_text())["blenderRevision"]
        if not asset_stamp.exists() or asset_stamp.read_text() != revision:
            subprocess.run(["git", "lfs", "fetch", "origin", "--include=release/datafiles/**"], cwd=source, check=True)
            subprocess.run(["git", "lfs", "checkout", "release/datafiles/**"], cwd=source, check=True)
            asset_stamp.write_text(revision)
    return source


def generate_rna_checkpoint(base, source, output=None):
    # /fast bypasses CMake's dependency phase. Generate headers before any
    # hand-written RNA objects compile, including on an empty CI build tree.
    directory = output or base / "editor/source/blender/makesrna/intern"
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(base / "editor/bin/makesrna.js"),
                    str(source / "source/blender/makesdna") + "/",
                    str(directory) + "/", str(directory.parent) + "/"],
                   env={**os.environ, "_BLENDER_WEB_BUILD_": "1"}, check=True)
    if not (directory.parent / "RNA_prototypes.hh").is_file():
        raise RuntimeError("RNA generator did not produce its public prototype header")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/native")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--app", action="store_true", help="Build the complete original Blender browser application")
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
    source = prepare_native(base, full_assets=args.app)
    if not args.skip_deps:
        build_dependencies(argparse.Namespace(build_dir=base / "deps", jobs=args.jobs, packages=[]))
        if args.app:
            build_application_dependencies(base, args.jobs)
    prefix = base / "deps/prefix"
    os.environ["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    os.environ["PKG_CONFIG_PATH"] = ""
    if args.app:
        stage(base, source, prefix)
        if args.check_deps:
            check_python(base, prefix)
            check_shader_translation(base, prefix)
            check_switch_lowering(base)
            check_shader_adaptation(base)
    if args.check_deps:
        check_dependencies(source, base, prefix, args.jobs)
    subprocess.run(["emcmake", "cmake", f"-DBLENDER_WEB_APPLICATION={'ON' if args.app else 'OFF'}", "-DBLENDER_WEB_JSPI=ON", "-G", "Unix Makefiles", "-S", str(source), "-B", str(base / "editor"),
                    f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'web/native/toolchain.cmake'}",
                    f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
                    "-C", str(source / "intern/ghost/web/native/config.cmake"),
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], check=True)
    if not args.configure_only:
        command = ["cmake", "--build", str(base / "editor"), "--parallel", str(args.jobs), "--target"]
        if args.targets or args.app:
            subprocess.run([*command, *(args.targets or ["blender"])], check=True)
            if args.app:
                build_ui_fallback(base, args.jobs)
                check_expressions(base, source, prefix)
                check_shader_processor(base, source)
                check_image_cache(base, prefix)
                print(f"Browser application: {collect(base, ROOT)}")
            print(f"Compiled upstream targets: {', '.join(args.targets or ['blender'])}")
        else:
            subprocess.run([*command, "datatoc", "shader_tool", "makesrna"], check=True)
            # Make's upstream /fast rules compile each original archive without
            # recursively building every linked editor/render library first.
            generate_rna_checkpoint(base, source)
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
                                     for name in ("ghost-web.patch", "native-web.patch", "native-ui.patch", "native-portability.patch", "native-render.patch", "native-files.patch", "native-modeling.patch", "native-cycles.patch", "native-python-clock.patch")}}
            (base / "editor-compilation.json").write_text(json.dumps(report, indent=2) + "\n")
            print("Original Blender editor archives compiled. Use --app to build the browser executable.")


if __name__ == "__main__":
    main()
