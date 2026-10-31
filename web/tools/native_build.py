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
from native_package import stage, stage_locales, collect
from native_app_deps import build as build_application_dependencies

EDITOR_ARCHIVES = ["bf_rna", "bf_editor_interface", "bf_editor_screen", "bf_windowmanager",
                   "bf_blenfont", "bf_intern_ghost"]

def check_dependencies(source, base, prefix, jobs):
    media_sources = ["web/native/tests/media.cc", "web/native/tests/CMakeLists.txt",
                     "web/native/dependencies.cmake", "web/tools/native_deps.py",
                     "web/tools/native_media.py", "web/tools/native_text.py"]
    media_hashes = {name:hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                    for name in media_sources}
    embree_sources = ["web/native/tests/embree.cc", "web/native/tests/CMakeLists.txt",
                      "web/native/dependencies.cmake", "web/tools/native_deps.py",
                      "web/native/patches/embree.patch", "web/native/toolchain.cmake"]
    embree_hashes = {name:hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                     for name in embree_sources}
    cpu_library_inputs = {
        'openpgl': ['web/native/tests/openpgl.cc', 'web/native/patches/openpgl.patch'],
        'oidn': ['web/native/tests/oidn.cc', 'web/native/patches/oidn.patch',
                 'web/native/oidn/half.cpp', 'web/tools/native_oidn.py',
                 'web/tools/native_ispc.py', 'web/tools/native_ispc_half.py', 'web/native/ispc-host.json'],
        'hevc': ['web/native/tests/hevc.cc', 'web/tools/native_media.py'],
        'avif': ['web/native/tests/avif.cc', 'web/native/gltf.cmake',
                 'web/native/ghost/GHOST_WebDracoTypes.hh'],
        'usd-imaging': ['web/native/tests/usd_imaging.cc',
                        'web/native/usd-imaging/CMakeLists.txt', 'web/tools/native_usd_imaging.py'],
        'ffi-workers': ['web/native/tests/ffi-workers.cc', 'web/native/ghost/GHOST_WebFFI.cc',
                        'web/native/ghost/GHOST_WebFFI.hh', 'web/native/ghost/GHOST_WebWorkerResources.hh', 'web/tools/native_ffi.py',
                        'web/native/patches/ffi.patch'],
        'ffi-high-address': ['web/native/tests/ffi-high-address.cc',
                             'web/native/ghost/GHOST_WebFFI.cc', 'web/native/ghost/GHOST_WebFFI.hh', 'web/native/ghost/GHOST_WebWorkerResources.hh',
                             'web/tools/native_ffi.py', 'web/native/patches/ffi.patch'],
    }
    common_inputs = ['web/native/tests/CMakeLists.txt', 'web/native/dependencies.cmake',
                     'web/tools/native_deps.py', 'web/native/toolchain.cmake']
    cpu_library_hashes = {name: {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
                                for path in [*paths, *common_inputs]}
                          for name, paths in cpu_library_inputs.items()}
    checks = base / "checks"
    subprocess.run(["emcmake", "cmake", "-S", str(ROOT / "web/native/tests"), "-B", str(checks),
                    f"-DBLENDER_SOURCE={source}",
                    f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'web/native/toolchain.cmake'}",
                    f"-DCMAKE_PREFIX_PATH={prefix}", f"-DCMAKE_FIND_ROOT_PATH={prefix}",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_BUILD_TYPE=Release",
                    "-DWITH_OPENVDB=ON", "-DWITH_NANOVDB=ON",
                    "-DWITH_USD=ON", "-DWITH_MATERIALX=ON",
                    "-DWITH_CODEC_FFMPEG=ON", "-DWITH_CODEC_SNDFILE=ON", "-DWITH_RUBBERBAND=ON", "-DWITH_CYCLES_EMBREE=ON",
                    "-DWITH_OPENIMAGEDENOISE=ON", "-DWITH_CYCLES_PATH_GUIDING=ON",
                    "-DWITH_HARFBUZZ=ON", "-DWITH_FRIBIDI=ON", "-DWITH_LIBMV=ON"], check=True)
    subprocess.run(["cmake", "--build", str(checks), "--parallel", str(jobs)], check=True)
    result = subprocess.run(["node", str(checks / "dependencies.js")], check=True,
                            text=True, stdout=subprocess.PIPE, timeout=120)
    print(result.stdout, end="", flush=True)
    (base / "dependency-check.json").write_text(json.dumps(json.loads(result.stdout), indent=2) + "\n")
    for name in ("fftw", "alembic", "media", "tracking", "embree", "openpgl", "oidn", "hevc", "avif", "usd-imaging", "ffi-workers", "ffi-high-address"):
        command = ["node", str(checks / f"{name}-check.js")]
        if name == 'avif':
            (base / 'avif-check').mkdir(exist_ok=True)
        if name in {'oidn', 'hevc', 'avif'}:
            command.append(str(base / f'{name}-check'))
        if name == 'usd-imaging':
            directory = base / 'usd-imaging-check'
            directory.mkdir(exist_ok=True)
            command.extend([str(directory), str(prefix)])
        output = subprocess.check_output(command, text=True, timeout=60)
        report = json.loads(output)
        if name == "media":
            report['sourceHashes'] = media_hashes
        if name == 'embree':
            report['sourceHashes'] = embree_hashes
        if name in cpu_library_hashes:
            report['sourceHashes'] = cpu_library_hashes[name]
        if name == 'usd-imaging':
            report['installationSha256'] = hashlib.sha256(
                (base / 'deps/usd-imaging/installed.json').read_bytes()).hexdigest()
            (base / 'usd-imaging-check/unit-report.json').write_text(json.dumps(report, indent=2) + '\n')
        (base / f"{name}-check.json").write_text(json.dumps(report, indent=2) + "\n")
        print(output, end="", flush=True)
    subprocess.run(['python3', str(ROOT / 'web/tools/native_ispc_check.py'), '--base', str(base)], check=True)
    output = subprocess.check_output(['node', str(checks / 'text-check.js'),
        str(source / 'release/datafiles/fonts')], text=True, timeout=60)
    (base / 'text-check.json').write_text(json.dumps(json.loads(output), indent=2) + '\n')
    print(output, end='', flush=True)
    fixtures = base / 'volume-fixtures'
    fixtures.mkdir(exist_ok=True)
    output = subprocess.check_output(['node', str(checks / 'openvdb-check.js'), str(fixtures)],
                                     text=True, timeout=120)
    (base / 'openvdb-check.json').write_text(json.dumps(json.loads(output), indent=2) + '\n')
    print(output, end='', flush=True)
    for name in ('usd-python', 'materialx'):
        if (checks / f'{name}-check.js').exists():
            output = subprocess.check_output(['node', str(checks / f'{name}-check.js')],
                                             cwd=checks, text=True, timeout=120)
            (base / f'{name}-check.json').write_text(json.dumps(json.loads(output), indent=2) + '\n')
            print(output, end='', flush=True)


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
    from native_ffi import LINK_OPTIONS
    inputs = ['web/native/tests/python.cc', 'web/native/tests/python-foundation.py',
              'web/native/tests/python-ctypes.py',
              'web/native/tests/python-tls-fixture.hh', 'web/tools/native_build.py',
              'web/tools/native_python.py', 'web/tools/native_python_packages.py',
              'web/native/python-packages.json', 'web/native/openssl-wasm.conf',
              'web/native/ghost/GHOST_WebNetwork.cc', 'web/native/ghost/GHOST_WebNetwork.py',
              'web/native/ghost/GHOST_WebDownload.py', 'web/patches/native-python-download.patch',
              'web/native/patches/urllib3.patch', 'web/tools/native_deps.py',
              'web/tools/native_ffi.py', 'web/native/patches/ffi.patch',
              'web/native/ghost/GHOST_WebFFI.cc', 'web/native/ghost/GHOST_WebFFI.hh', 'web/native/ghost/GHOST_WebWorkerResources.hh',
              'web/native/ghost/GHOST_WebCTypes.cc', 'web/native/ghost/GHOST_WebCTypes.hh']
    source_hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}
    executable = base / "python-check.js"
    subprocess.run(["em++", "-O2", "-pthread", "-msimd128", "-fwasm-exceptions",
        f"-I{prefix / 'include/python3.13'}", f"-I{prefix / 'include'}", str(ROOT / "web/native/tests/python.cc"),
        "-Wl,--start-group", *map(str, sorted((prefix / "lib/blender-web-numpy").glob("*.a"))),
        str(prefix / 'lib/blender-web-python/extensions.a'),
        *[str(prefix / 'lib' / ('lib' + name + '.a')) for name in ('sqlite3', 'lzma', 'ssl', 'crypto', 'zstd', 'ffi')],
        str(prefix / "lib/libpython3.13.a"), str(prefix / "lib/libexpat.a"),
        str(prefix / "lib/libz.a"), "-Wl,--end-group", "-sUSE_BZIP2=1",
        *LINK_OPTIONS, "-sENVIRONMENT=node", "-sPROXY_TO_PTHREAD=1", "-sPTHREAD_POOL_SIZE=8", "-sEXIT_RUNTIME=1", "-sALLOW_MEMORY_GROWTH=1",
        "-sINITIAL_MEMORY=134217728", "-sDEFAULT_PTHREAD_STACK_SIZE=8388608", "-UNDEBUG",
        "-sSTACK_SIZE=8388608", "-sWASMFS=1", "-sFORCE_FILESYSTEM=1", "--preload-file",
        f"{base / 'assets/python3.13'}@/python/lib/python3.13", "--preload-file",
        f"{ROOT / 'web/native/tests/python-foundation.py'}@/python-foundation.py", "--preload-file",
        f"{ROOT / 'web/native/tests/python-ctypes.py'}@/python-ctypes.py", "-o", str(executable)], check=True)
    output = subprocess.check_output(["node", str(executable)], cwd=base, text=True, timeout=120)
    report = json.loads(output)
    if source_hashes != {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}:
        raise RuntimeError('Python check inputs changed during validation; rerun the check')
    report['sourceHashes'] = source_hashes
    (base / "python-check.json").write_text(json.dumps(report, indent=2)+"\n")
    print(output, end="", flush=True)


def check_gltf(base, source, prefix):
    inputs = [ROOT / 'web/native/ghost/GHOST_WebGLTF.cc', ROOT / 'web/native/ghost/GHOST_WebGLTF.hh',
              ROOT / 'web/native/ghost/GHOST_WebDracoTypes.hh', ROOT / 'web/native/gltf.cmake',
              ROOT / 'web/native/ghost/GHOST_WebGLTF.py', ROOT / 'web/native/tests/gltf.cc',
              ROOT / 'web/native/tests/gltf.py', ROOT / 'web/patches/native-gltf.patch']
    source_hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in inputs}
    executable = base / "gltf-check.js"
    bridge_sources = [source / "intern" / library / "intern" / name
                      for library in ("draco_bridge", "meshoptimizer_bridge")
                      for name in ("common.cpp", "encoder.cpp", "decoder.cpp")]
    subprocess.run(["em++", "-std=c++20", "-O2", "-pthread", "-msimd128", "-fwasm-exceptions",
        '-include', str(ROOT / 'web/native/ghost/GHOST_WebDracoTypes.hh'),
        f"-I{prefix / 'include/python3.13'}", f"-I{prefix / 'include'}", f"-I{source}",
        str(ROOT / "web/native/tests/gltf.cc"), str(ROOT / "web/native/ghost/GHOST_WebGLTF.cc"),
        *map(str, bridge_sources), "-Wl,--start-group",
        str(prefix / "lib/libdraco.a"), str(prefix / "lib/libmeshoptimizer.a"),
        str(prefix / "lib/libpython3.13.a"), str(prefix / "lib/libz.a"), "-Wl,--end-group",
        "-sUSE_BZIP2=1", "-sENVIRONMENT=node", "-sEXIT_RUNTIME=1", "-sALLOW_MEMORY_GROWTH=1",
        "-sSTACK_SIZE=8388608", "-sWASMFS=1", "-sFORCE_FILESYSTEM=1",
        "--preload-file", f"{base / 'assets/python3.13'}@/python/lib/python3.13",
        "--preload-file", f"{ROOT / 'web/native/ghost/GHOST_WebGLTF.py'}@/_bpy_ghost_web_gltf.py",
        "--preload-file", f"{ROOT / 'web/native/tests/gltf.py'}@/check.py",
        "-o", str(executable)], check=True)
    output = subprocess.check_output(["node", str(executable)], cwd=base, text=True, timeout=60)
    report = json.loads(next(line.removeprefix('GLTF_CHECK:') for line in output.splitlines()
                             if line.startswith('GLTF_CHECK:')))
    if source_hashes != {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in inputs}:
        raise RuntimeError('glTF inputs changed during validation; rerun the check')
    report['sourceHashes'] = source_hashes
    (base / "gltf-check.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report), flush=True)


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


def check_python_bindings(base, prefix):
    from native_bindings import INPUTS
    inputs = [*INPUTS, 'web/native/tests/python-bindings.cc',
              'web/native/tests/python-bindings.py', 'web/native/python-bindings/check/CMakeLists.txt']
    source_hashes = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}
    work = base / 'python-bindings-check'
    subprocess.run(['emcmake', 'cmake', '-G', 'Unix Makefiles',
        '-S', str(ROOT / 'web/native/python-bindings/check'), '-B', str(work),
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS=-O3 -pthread -msimd128 -fwasm-exceptions',
        f'-DBLENDER_WEB_PREFIX={prefix}', f'-DBLENDER_WEB_ASSETS={base / "assets"}',
        f'-DCMAKE_FIND_ROOT_PATH={prefix}', f'-DCMAKE_PREFIX_PATH={prefix}'], check=True)
    subprocess.run(['cmake', '--build', str(work), '--parallel', '2'], check=True)
    result = subprocess.run(['node', str(work / 'check.js')], cwd=work,
                            text=True, capture_output=True, timeout=180, check=True)
    if result.stderr.strip():
        raise RuntimeError('Unexpected interpreter shutdown diagnostics:\n' + result.stderr)
    report = json.loads(result.stdout)
    if source_hashes != {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}:
        raise RuntimeError('Binding inputs changed during validation; rerun the check')
    report['sourceHashes'] = source_hashes
    report['bindingInstallationSha256'] = hashlib.sha256(
        (base / 'deps/python-bindings/installed.json').read_bytes()).hexdigest()
    report['cleanInterpreterShutdown'] = True
    (base / 'python-bindings-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(versions=report['versions'], cleanInterpreterShutdown=True)), flush=True)
    from native_osl_python_check import check as check_osl_python
    check_osl_python(base)


def prepare_native(base, full_assets=False):
    source = prepare_source(base)
    patch = ROOT / "web/patches/native-web.patch"
    check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=source, capture_output=True)
    if check.returncode == 0:
        subprocess.run(["git", "apply", str(patch)], cwd=source, check=True)
    elif subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                        cwd=source, capture_output=True).returncode:
        raise RuntimeError(f"Native shims do not apply cleanly:\n{check.stderr.decode()}")
    for patch_name in ("native-ui.patch", "native-portability.patch", "native-render.patch", "native-files.patch", "native-modeling.patch", "native-cycles.patch", "native-python-clock.patch", "native-translation.patch", "native-tracking.patch", "native-gltf.patch", "native-guiding.patch", "native-denoise.patch", "native-python-download.patch"):
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
    parser.add_argument("--skip-shader-seed", action="store_true", help="Bootstrap a fresh shader capture after translator changes")
    parser.add_argument("--render-only", action="store_true", help="Skip the fallback link during shader bootstrap or backend development")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--deps-only", action="store_true", help="Build or verify dependencies, then stop before staging Blender")
    parser.add_argument("--check-deps", action="store_true", help="Run Wasm dependency checks under Node")
    parser.add_argument("--targets", nargs="+", help="Override the editor archive compilation checkpoint with upstream targets")
    args = parser.parse_args()
    # Large USD-enabled modules exhaust modest hosts when Binaryen runs a pass
    # on many functions at once. This only controls build-time optimization.
    os.environ.setdefault("BINARYEN_CORES", "1")
    if args.deps_only and args.skip_deps:
        parser.error("--deps-only cannot be combined with --skip-deps")
    if args.render_only and not args.app:
        parser.error("--render-only requires --app")
    if args.render_only and args.targets and "blender" not in args.targets:
        parser.error("--render-only requires the blender target")
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
        from native_usd_imaging import build as build_usd_imaging
        build_usd_imaging(base, args.jobs)
        if args.app:
            build_application_dependencies(base, args.jobs)
    if args.deps_only:
        return
    prefix = base / "deps/prefix"
    os.environ["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    os.environ["PKG_CONFIG_PATH"] = ""
    if args.check_deps:
        from native_compiler_check import check as check_compilers
        check_compilers(base, args.jobs)
    if args.app:
        stage(base, source, prefix, shader_seed=not args.skip_shader_seed)
        if args.check_deps:
            check_python(base, prefix)
            check_python_bindings(base, prefix)
            check_gltf(base, source, prefix)
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
        if args.app:
            subprocess.run([*command, "locales"], check=True)
            stage_locales(base, source, prefix)
        if args.targets or args.app:
            subprocess.run([*command, *(args.targets or ["blender"])], check=True)
            if args.app:
                if not args.render_only:
                    build_ui_fallback(base, args.jobs)
                check_expressions(base, source, prefix)
                check_shader_processor(base, source)
                check_image_cache(base, prefix)
                print(f"Browser application: {collect(base, ROOT, ui_fallback=not args.render_only)}")
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
                                     for name in ("ghost-web.patch", "native-web.patch", "native-ui.patch", "native-portability.patch", "native-render.patch", "native-files.patch", "native-modeling.patch", "native-cycles.patch", "native-python-clock.patch", "native-translation.patch", "native-tracking.patch", "native-gltf.patch", "native-guiding.patch", "native-denoise.patch", "native-python-download.patch")}}
            (base / "editor-compilation.json").write_text(json.dumps(report, indent=2) + "\n")
            print("Original Blender editor archives compiled. Use --app to build the browser executable.")


if __name__ == "__main__":
    main()
