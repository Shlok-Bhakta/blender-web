#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build real Wasm dependencies using the pinned Blender release's source hashes."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

from prepare import ROOT


# Ordered by dependencies. These are build options, not substitute implementations.
RECIPES = {
    "ZLIB": ("", ["-DZLIB_BUILD_EXAMPLES=OFF"]),
    "JPEG": ("", ["-DENABLE_SHARED=OFF", "-DWITH_SIMD=OFF", "-DWITH_TURBOJPEG=OFF"]),
    "PNG": ("", ["-DPNG_SHARED=OFF", "-DPNG_TESTS=OFF", "-DPNG_TOOLS=OFF", "-DPNG_HARDWARE_OPTIMIZATIONS=OFF"]),
    "ZSTD": ("build/cmake", ["-DZSTD_BUILD_SHARED=OFF", "-DZSTD_BUILD_PROGRAMS=OFF", "-DZSTD_BUILD_TESTS=OFF"]),
    "BROTLI": ("", ["-DBROTLI_DISABLE_TESTS=ON"]),
    "FREETYPE": ("", ["-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_PNG=ON", "-DFT_REQUIRE_BROTLI=ON", "-DFT_REQUIRE_ZLIB=ON"]),
    "FMT": ("", ["-DFMT_TEST=OFF", "-DFMT_DOC=OFF"]),
    "EIGEN": ("", ["-DEIGEN_BUILD_TESTING=OFF", "-DEIGEN_BUILD_DOC=OFF", "-DEIGEN_BUILD_BLAS=OFF", "-DEIGEN_BUILD_LAPACK=OFF", "-DEIGEN_BUILD_DEMOS=OFF"]),
    "IMATH": ("", ["-DIMATH_BUILD_TESTS=OFF", "-DIMATH_BUILD_EXAMPLES=OFF", "-DPYTHON=OFF"]),
    "DEFLATE": ("", ["-DLIBDEFLATE_BUILD_SHARED_LIB=OFF", "-DLIBDEFLATE_BUILD_GZIP=OFF"]),
    "OPENJPH": ("", ["-DOJPH_BUILD_EXECUTABLES=OFF", "-DOJPH_BUILD_TESTS=OFF", "-DOJPH_ENABLE_TIFF_SUPPORT=OFF", "-DOJPH_DISABLE_SIMD=ON"]),
    "OPENEXR": ("", ["-DOPENEXR_BUILD_TOOLS=OFF", "-DOPENEXR_INSTALL_TOOLS=OFF", "-DOPENEXR_BUILD_EXAMPLES=OFF", "-DOPENEXR_FORCE_INTERNAL_DEFLATE=OFF", "-DOPENEXR_FORCE_INTERNAL_OPENJPH=OFF", "-DCMAKE_REQUIRE_FIND_PACKAGE_openjph=ON"]),
    "EXPAT": ("expat", ["-DEXPAT_BUILD_TOOLS=OFF", "-DEXPAT_BUILD_EXAMPLES=OFF", "-DEXPAT_BUILD_TESTS=OFF"]),
    "YAMLCPP": ("", ["-DYAML_CPP_BUILD_TESTS=OFF", "-DYAML_CPP_BUILD_TOOLS=OFF", "-DYAML_BUILD_SHARED_LIBS=OFF"]),
    "PYSTRING": ("", []),
    "MINIZIPNG": ("", ["-DMZ_COMPAT=OFF", "-DMZ_BZIP2=OFF", "-DMZ_LZMA=OFF", "-DMZ_ZSTD=OFF", "-DMZ_LIBBSD=OFF", "-DMZ_OPENSSL=OFF", "-DMZ_LIBCOMP=OFF", "-DMZ_FETCH_LIBS=OFF", "-DMZ_BUILD_TESTS=OFF", "-DMZ_BUILD_UNIT_TESTS=OFF"]),
    "OPENCOLORIO": ("", ["-DOCIO_BUILD_APPS=OFF", "-DOCIO_BUILD_PYTHON=OFF", "-DOCIO_BUILD_TESTS=OFF", "-DOCIO_BUILD_GPU_TESTS=OFF", "-DOCIO_BUILD_DOCS=OFF", "-DOCIO_USE_SIMD=OFF", "-DOCIO_INSTALL_EXT_PACKAGES=NONE"]),
    "ROBINMAP": ("", ["-DTSL_ROBIN_MAP_BUILD_TESTS=OFF"]),
    "TBB": ("", ["-DTBB_TEST=OFF", "-DTBB_EXAMPLES=OFF", "-DTBB_STRICT=OFF", "-DTBBMALLOC_BUILD=OFF", "-DTBBMALLOC_PROXY_BUILD=OFF", "-DTBB_ENABLE_IPO=OFF", "-DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON"]),
    "OPENSUBDIV": ("", ["-DNO_EXAMPLES=ON", "-DNO_TUTORIALS=ON", "-DNO_REGRESSION=ON", "-DNO_PTEX=ON", "-DNO_DOC=ON", "-DNO_OMP=ON", "-DNO_TBB=OFF", "-DNO_CUDA=ON", "-DNO_OPENCL=ON", "-DNO_CLEW=ON", "-DNO_OPENGL=ON", "-DNO_METAL=ON", "-DNO_DX=ON", "-DNO_TESTS=ON", "-DNO_GLTESTS=ON", "-DNO_GLEW=ON", "-DNO_GLFW=ON"]),
    "TIFF": ("", ["-Dtiff-tools=OFF", "-Dtiff-tests=OFF", "-Dtiff-contrib=OFF", "-Dtiff-docs=OFF", "-Dlzma=OFF", "-Dwebp=OFF", "-Djbig=OFF"]),
    "OPENIMAGEIO": ("", ["-DOIIO_BUILD_TOOLS=OFF", "-DOIIO_BUILD_TESTS=OFF", "-DOIIO_BUILD_DOCS=OFF", "-DOpenImageIO_BUILD_MISSING_DEPS=OFF", "-DEMBEDPLUGINS=ON", "-DUSE_PYTHON=OFF", "-DUSE_QT=OFF", "-DUSE_FFMPEG=OFF", "-DUSE_OPENCV=OFF", "-DUSE_OPENVDB=OFF", "-DUSE_TBB=OFF", "-DUSE_SIMD=0"]),
}

C_FLAGS = "-pthread -msimd128 -fwasm-exceptions"
CXX_FLAGS = C_FLAGS
CXX_PACKAGES = {"FMT", "EIGEN", "IMATH", "OPENJPH", "OPENEXR", "YAMLCPP", "PYSTRING",
                "OPENCOLORIO", "TBB", "OPENSUBDIV", "OPENIMAGEIO", "TIFF"}


def run(command, log):
    with log.open("a") as output:
        output.write("\n" + " ".join(map(str, command)) + "\n")
        output.flush()
        result = subprocess.run(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed; see {log}\n" + "\n".join(log.read_text().splitlines()[-45:]))


def download(entry, downloads):
    archive = downloads / entry["FILE"]
    algorithm = entry["HASH_TYPE"].lower()
    if archive.exists() and hashlib.new(algorithm, archive.read_bytes()).hexdigest() == entry["HASH"]:
        return archive
    url = entry["URI"].replace("http://", "https://")
    # GitHub's archive endpoint sometimes rejects automated clients; codeload serves
    # the identical tarball. The release's checksum still has to match.
    if "github.com/" in url and "/archive/" in url:
        repo, ref = url.split("github.com/", 1)[1].split("/archive/", 1)
        url = f"https://codeload.github.com/{repo}/tar.gz/{ref.removesuffix('.tar.gz')}"
    temporary = archive.with_suffix(archive.suffix + ".partial")
    try:
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        actual = hashlib.new(algorithm, temporary.read_bytes()).hexdigest()
        if actual != entry["HASH"]:
            raise RuntimeError(f"Checksum mismatch for {entry['FILE']}: {actual}")
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)
    return archive


def install_notices(source, prefix, name):
    # Keep notices alongside the cached prefix. CI may restore installed
    # libraries without restoring their extracted source trees.
    destination = prefix / 'share/blender-web/licenses' / name.lower()
    destination.mkdir(parents=True, exist_ok=True)
    for notice in source.iterdir():
        if notice.is_file() and notice.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE')):
            shutil.copy2(notice, destination / notice.name)


def build(args):
    if not shutil.which("emcmake"):
        raise RuntimeError("Activate the pinned Emscripten SDK before building dependencies.")
    expected = json.loads((ROOT / "web/upstream.json").read_text())["emscriptenVersion"]
    installed = subprocess.check_output(["emcc", "-dumpversion"], text=True).strip()
    if installed != expected:
        raise RuntimeError(f"Expected Emscripten {expected}, found {installed}")
    base = args.build_dir.resolve()
    base.mkdir(parents=True, exist_ok=True)
    downloads = base / "downloads"
    downloads.mkdir(exist_ok=True)
    names = args.packages or list(RECIPES)
    lock_path = base / "dependencies.json"
    subprocess.run(["cmake", f"-DBLENDER_SOURCE={ROOT / 'blender'}",
                    f"-DDEPENDENCIES={';'.join(names)}", f"-DOUTPUT={lock_path}",
                    "-P", str(ROOT / "web/native/dependency-lock.cmake")], check=True)
    lock = json.loads(lock_path.read_text())
    # Only downloads run concurrently. Compilation uses the user's job limit.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        archives = dict(zip(names, pool.map(lambda name: download(lock[name], downloads), names)))
    prefix = base / "prefix"
    os.environ["PKG_CONFIG_LIBDIR"] = str(prefix / "lib/pkgconfig")
    os.environ["PKG_CONFIG_PATH"] = ""
    for name in names:
        directory = base / name.lower()
        directory.mkdir(exist_ok=True)
        stamp = directory / "installed.json"
        signature = {"source": lock[name], "recipe": RECIPES[name], "threads": True, "cFlags": C_FLAGS,
                     "toolchain": hashlib.sha256((ROOT / "web/native/toolchain.cmake").read_bytes()).hexdigest(),
                     "sdk": json.loads((ROOT / "web/upstream.json").read_text())["emscriptenVersion"]}
        if name in CXX_PACKAGES:
            signature["cxxFlags"] = CXX_FLAGS
        patch = ROOT / "web/native/patches" / f"{name.lower()}.patch"
        if patch.exists():
            signature["patch"] = hashlib.sha256(patch.read_bytes()).hexdigest()
        cached = stamp.exists() and json.loads(stamp.read_text()) == json.loads(json.dumps(signature))
        if cached and (prefix / 'share/blender-web/licenses' / name.lower()).is_dir():
            print(f"{name}: cached", flush=True)
            continue
        source = directory / "source"
        source_record = directory / "source.json"
        if source_record.exists() and json.loads(source_record.read_text()) != lock[name]:
            shutil.rmtree(source, ignore_errors=True)
            shutil.rmtree(directory / "build", ignore_errors=True)
        if not source.exists():
            extracted = directory / "extract"
            extracted.mkdir(exist_ok=True)
            with tarfile.open(archives[name]) as archive:
                archive.extractall(extracted, filter="data")
            roots = list(extracted.iterdir())
            if len(roots) != 1 or not roots[0].is_dir():
                raise RuntimeError(f"Unexpected archive structure in {archives[name]}")
            roots[0].rename(source)
            extracted.rmdir()
        source_record.write_text(json.dumps(lock[name], indent=2) + "\n")
        install_notices(source, prefix, name)
        if cached:
            print(f"{name}: cached", flush=True)
            continue
        print(f"{name}: compiling {lock[name]['VERSION']} for Wasm/pthreads", flush=True)
        if patch.exists():
            check = subprocess.run(["patch", "--silent", "--dry-run", "--forward", "-p1", "-i", str(patch)],
                                   cwd=source, capture_output=True)
            if check.returncode == 0:
                subprocess.run(["patch", "--silent", "--forward", "-p1", "-i", str(patch)], cwd=source, check=True)
            elif subprocess.run(["patch", "--silent", "--dry-run", "--reverse", "-p1", "-i", str(patch)],
                                cwd=source, capture_output=True).returncode:
                raise RuntimeError(f"Dependency patch does not apply: {patch}\n{check.stdout.decode()}")
        if name == "PYSTRING":
            shutil.copy2(ROOT / "blender/build_files/build_environment/patches/cmakelists_pystring.txt", source / "CMakeLists.txt")
        log = directory / "build.log"
        subdir, options = RECIPES[name]
        options = list(options)
        if name == "FREETYPE":
            options += [f"-DBROTLIDEC_INCLUDE_DIRS={prefix / 'include'}",
                        f"-DBROTLIDEC_LIBRARIES={prefix / 'lib/libbrotlidec-static.a'};{prefix / 'lib/libbrotlicommon-static.a'}"]
        configured = directory / "configured.json"
        configuration = json.loads(json.dumps({**signature, "options": options}))
        previous = json.loads(configured.read_text()) if configured.exists() else {}
        if any(previous.get(key) != configuration.get(key) for key in ("toolchain", "sdk", "source")):
            shutil.rmtree(directory / "build", ignore_errors=True)
        configured.write_text(json.dumps(configuration, indent=2) + "\n")
        run(["emcmake", "cmake", "-S", source / subdir, "-B", directory / "build",
             f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'web/native/toolchain.cmake'}",
             f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_PREFIX_PATH={prefix}",
             f"-DCMAKE_FIND_ROOT_PATH={prefix}",
             "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_C_FLAGS={C_FLAGS}", f"-DCMAKE_CXX_FLAGS={CXX_FLAGS}",
             "-DCMAKE_EXE_LINKER_FLAGS=-pthread -fwasm-exceptions", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF",
             *options], log)
        run(["cmake", "--build", directory / "build", "--parallel", args.jobs], log)
        run(["cmake", "--install", directory / "build"], log)
        if name == "TIFF":
            # libtiff's static config needs its codec find modules when consumed
            # by another project, including CMake's config-first FindTIFF path.
            for module in ("FindDeflate.cmake", "FindZSTD.cmake"):
                shutil.copy2(source / "cmake" / module, prefix / "lib/cmake/tiff" / module)
        if name == "BROTLI":
            # Brotli 1.0.9 deliberately has no install rules under Emscripten.
            # Install its actual compiled archives and public headers ourselves.
            shutil.copytree(source / "c/include/brotli", prefix / "include/brotli", dirs_exist_ok=True)
            for library in (directory / "build").glob("libbrotli*-static.a"):
                shutil.copy2(library, prefix / "lib" / library.name)
        stamp.write_text(json.dumps(signature, indent=2) + "\n")
    print(f"Wasm dependency prefix: {prefix}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/native/deps")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("packages", nargs="*", choices=list(RECIPES))
    build(parser.parse_args())
