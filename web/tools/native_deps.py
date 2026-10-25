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
import urllib.error
import urllib.request
import zipfile

from prepare import ROOT


# Ordered by dependencies. These are build options, not substitute implementations.
RECIPES = {
    "ZLIB": ("", ["-DZLIB_BUILD_EXAMPLES=OFF"]),
    "JPEG": ("", ["-DENABLE_SHARED=OFF", "-DWITH_SIMD=OFF", "-DWITH_TURBOJPEG=OFF"]),
    "PNG": ("", ["-DPNG_SHARED=OFF", "-DPNG_TESTS=OFF", "-DPNG_TOOLS=OFF", "-DPNG_HARDWARE_OPTIMIZATIONS=OFF"]),
    "ZSTD": ("build/cmake", ["-DZSTD_BUILD_SHARED=OFF", "-DZSTD_BUILD_PROGRAMS=OFF", "-DZSTD_BUILD_TESTS=OFF"]),
    "BROTLI": ("", ["-DBROTLI_DISABLE_TESTS=ON"]),
    "FREETYPE": ("", ["-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_PNG=ON", "-DFT_REQUIRE_BROTLI=ON", "-DFT_REQUIRE_ZLIB=ON"]),
    "HARFBUZZ": ("", ["-Dtests=disabled", "-Dfreetype=enabled", "-Dglib=disabled", "-Dgobject=disabled", "-Dcairo=disabled", "-Dutilities=disabled", "-Dcpp_eh=default"]),
    "FRIBIDI": ("", ["-Ddocs=false", "-Dbin=false", "-Dtests=false"]),
    "DRACO": ("", ["-DDRACO_JS_GLUE=OFF", "-DDRACO_WASM=OFF", "-DDRACO_TESTS=OFF"]),
    "MESHOPTIMIZER": ("", ["-DMESHOPT_BUILD_SHARED_LIBS=OFF", "-DMESHOPT_BUILD_DEMO=OFF", "-DMESHOPT_BUILD_GLTFPACK=OFF"]),
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
    "EMBREE": ("", ["-DEMBREE_STATIC_LIB=ON", "-DEMBREE_TUTORIALS=OFF", "-DEMBREE_ISPC_SUPPORT=OFF", "-DEMBREE_SYCL_SUPPORT=OFF", "-DEMBREE_MAX_ISA=SSE2", "-DFLAGS_SSE2=-msse2", "-DEMBREE_IGNORE_CMAKE_CXX_FLAGS=OFF", "-DEMBREE_TASKING_SYSTEM=TBB", "-DEMBREE_RAY_MASK=ON", "-DEMBREE_FILTER_FUNCTION=ON", "-DEMBREE_BACKFACE_CULLING=OFF", "-DEMBREE_BACKFACE_CULLING_CURVES=ON", "-DEMBREE_BACKFACE_CULLING_SPHERES=ON"]),
    "ABSEIL": ("", ["-DABSL_BUILD_TESTING=OFF", "-DABSL_ENABLE_INSTALL=ON", "-DABSL_PROPAGATE_CXX_STD=ON", "-DCMAKE_CXX_STANDARD=17"]),
    "CERES": ("", ["-DBUILD_TESTING=OFF", "-DBUILD_BENCHMARKS=OFF", "-DBUILD_EXAMPLES=OFF", "-DUSE_CUDA=OFF", "-DSUITESPARSE=OFF", "-DACCELERATESPARSE=OFF", "-DLAPACK=OFF"]),
    "OPENSUBDIV": ("", ["-DNO_EXAMPLES=ON", "-DNO_TUTORIALS=ON", "-DNO_REGRESSION=ON", "-DNO_PTEX=ON", "-DNO_DOC=ON", "-DNO_OMP=ON", "-DNO_TBB=OFF", "-DNO_CUDA=ON", "-DNO_OPENCL=ON", "-DNO_CLEW=ON", "-DNO_OPENGL=ON", "-DNO_METAL=ON", "-DNO_DX=ON", "-DNO_TESTS=ON", "-DNO_GLTESTS=ON", "-DNO_GLEW=ON", "-DNO_GLFW=ON"]),
    "GMP": ("", ["--host=none", "--enable-cxx", "--disable-assembly", "--disable-shared", "--enable-static"]),
    "MANIFOLD": ("", ["-DMANIFOLD_JSBIND=OFF", "-DMANIFOLD_CBIND=OFF", "-DMANIFOLD_PYBIND=OFF", "-DMANIFOLD_PAR=ON", "-DMANIFOLD_CROSS_SECTION=OFF", "-DMANIFOLD_EXPORT=OFF", "-DMANIFOLD_DEBUG=OFF", "-DMANIFOLD_TEST=OFF", "-DMANIFOLD_DOWNLOADS=OFF"]),
    "FFTW": ("", ["-DENABLE_THREADS=ON", "-DWITH_COMBINED_THREADS=OFF", "-DBUILD_TESTS=OFF"]),
    "RUBBERBAND": ("", ["-Dauto_features=disabled", "-Dfft=fftw", "-Dresampler=builtin"]),
    "AOM": ("", ["-DAOM_TARGET_CPU=generic", "-DENABLE_DOCS=OFF", "-DENABLE_EXAMPLES=OFF", "-DENABLE_TESTS=OFF", "-DENABLE_TOOLS=OFF", "-DCONFIG_RUNTIME_CPU_DETECT=OFF"]),
    "X265": ("source", ["-DENABLE_SHARED=OFF", "-DENABLE_CLI=OFF", "-DENABLE_ASSEMBLY=OFF", "-DENABLE_LIBNUMA=OFF"]),
    "OGG": ("", ["-DBUILD_TESTING=OFF"]),
    "VORBIS": ("", []),
    "FLAC": ("", ["-DBUILD_CXXLIBS=OFF", "-DBUILD_PROGRAMS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_TESTING=OFF", "-DINSTALL_MANPAGES=OFF"]),
    "OPUS": ("", ["--host=wasm32-unknown-none", "--disable-shared", "--enable-static", "--disable-extra-programs", "--disable-doc", "--disable-stack-protector"]),
    "LAME": ("", ["--host=wasm32-unknown-none", "--disable-shared", "--enable-static", "--disable-frontend", "--disable-asm"]),
    "THEORA": ("", ["--host=wasm32-unknown-none", "--disable-shared", "--enable-static", "--disable-examples", "--disable-oggtest", "--disable-vorbistest", "--disable-asm"]),
    "VPX": ("", ["--target=generic-gnu", "--disable-examples", "--disable-tools", "--disable-docs", "--disable-unit-tests", "--disable-shared", "--enable-static", "--disable-runtime-cpu-detect"]),
    "X264": ("", ["--host=wasm32-unknown-none", "--disable-cli", "--disable-asm", "--enable-static", "--disable-opencl"]),
    "SNDFILE": ("", ["-DBUILD_PROGRAMS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_TESTING=OFF", "-DENABLE_EXTERNAL_LIBS=ON", "-DENABLE_MPEG=ON"]),
    "FFMPEG": ("", ["--target-os=none", "--arch=wasm32", "--enable-cross-compile", "--disable-inline-asm", "--disable-runtime-cpudetect", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-debug", "--disable-network", "--disable-devices", "--disable-shared", "--enable-static", "--enable-pthreads", "--enable-gpl", "--enable-zlib", "--enable-libmp3lame", "--enable-libopus", "--enable-libvorbis", "--enable-libtheora", "--enable-libvpx", "--enable-libx264", "--enable-libaom", "--enable-libx265", "--pkg-config-flags=--static"]),
    "ALEMBIC": ("", ["-DUSE_PYALEMBIC=OFF", "-DUSE_ARNOLD=OFF", "-DUSE_MAYA=OFF", "-DUSE_PRMAN=OFF", "-DUSE_HDF5=OFF", "-DUSE_TESTS=OFF", "-DUSE_BINARIES=OFF", "-DALEMBIC_SHARED_LIBS=OFF"]),
    "BLOSC": ("", ["-DBUILD_SHARED=OFF", "-DBUILD_TESTS=OFF", "-DBUILD_BENCHMARKS=OFF", "-DDEACTIVATE_SSE2=ON", "-DDEACTIVATE_AVX2=ON", "-DPREFER_EXTERNAL_ZLIB=ON", "-DPREFER_EXTERNAL_ZSTD=ON"]),
    "OPENVDB": ("", ["-DOPENVDB_CORE_SHARED=OFF", "-DOPENVDB_CORE_STATIC=ON", "-DOPENVDB_BUILD_BINARIES=OFF", "-DOPENVDB_BUILD_UNITTESTS=OFF", "-DOPENVDB_BUILD_PYTHON_MODULE=OFF", "-DOPENVDB_BUILD_NANOVDB=ON", "-DNANOVDB_BUILD_TOOLS=OFF", "-DNANOVDB_BUILD_UNITTESTS=OFF", "-DNANOVDB_USE_CUDA=OFF", "-DOPENVDB_USE_DELAYED_LOADING=OFF", "-DUSE_STATIC_DEPENDENCIES=ON", "-DUSE_BLOSC=ON", "-DUSE_ZLIB=ON", "-DUSE_LOG4CPLUS=OFF"]),
    "WEBP": ("", ["-DWEBP_BUILD_ANIM_UTILS=OFF", "-DWEBP_BUILD_CWEBP=OFF", "-DWEBP_BUILD_DWEBP=OFF", "-DWEBP_BUILD_GIF2WEBP=OFF", "-DWEBP_BUILD_IMG2WEBP=OFF", "-DWEBP_BUILD_VWEBP=OFF", "-DWEBP_BUILD_WEBPINFO=OFF", "-DWEBP_BUILD_WEBPMUX=OFF", "-DWEBP_BUILD_EXTRAS=OFF", "-DWEBP_ENABLE_SIMD=OFF"]),
    "OPENJPEG": ("", ["-DBUILD_CODEC=OFF", "-DBUILD_DOC=OFF", "-DBUILD_JPIP=OFF"]),
    "PUGIXML": ("", ["-DPUGIXML_BUILD_TESTS=OFF"]),
    "HARU": ("", ["-DLIBHPDF_EXAMPLES=OFF", "-DMATH_LIB=m", "-DM_LIB=m"]),
    "POTRACE": ("", []),
    "MATERIALX": ("", ["-DMATERIALX_BUILD_SHARED_LIBS=OFF", "-DMATERIALX_BUILD_PYTHON=OFF", "-DMATERIALX_BUILD_RENDER=OFF", "-DMATERIALX_BUILD_TESTS=OFF", "-DMATERIALX_BUILD_VIEWER=OFF", "-DMATERIALX_BUILD_GRAPH_EDITOR=OFF", "-DMATERIALX_BUILD_GEN_OSL=OFF", "-DMATERIALX_BUILD_GEN_MDL=OFF"]),
    "USD": ("", ["-DPXR_BUILD_MONOLITHIC=ON", "-DPXR_ENABLE_PYTHON_SUPPORT=ON", "-DPXR_BUILD_IMAGING=ON", "-DPXR_BUILD_USD_IMAGING=ON", "-DPXR_BUILD_GPU_SUPPORT=OFF", "-DPXR_BUILD_USDVIEW=OFF", "-DPXR_BUILD_USD_TOOLS=OFF", "-DPXR_BUILD_TESTS=OFF", "-DPXR_BUILD_EXAMPLES=OFF", "-DPXR_BUILD_TUTORIALS=OFF", "-DPXR_ENABLE_MATERIALX_SUPPORT=ON", "-DPXR_ENABLE_OPENVDB_SUPPORT=OFF", "-DPXR_ENABLE_GL_SUPPORT=OFF", "-DPXR_ENABLE_VULKAN_SUPPORT=OFF", "-DPXR_ENABLE_OSL_SUPPORT=OFF", "-DPXR_ENABLE_PTEX_SUPPORT=OFF", "-DPXR_BUILD_OPENIMAGEIO_PLUGIN=OFF", "-DPXR_BUILD_OPENCOLORIO_PLUGIN=OFF", "-DPXR_SET_INTERNAL_NAMESPACE=pxrBlender_v26_03"]),
    "TIFF": ("", ["-Dtiff-tools=OFF", "-Dtiff-tests=OFF", "-Dtiff-contrib=OFF", "-Dtiff-docs=OFF", "-Dlzma=OFF", "-Dwebp=OFF", "-Djbig=OFF"]),
    "OPENIMAGEIO": ("", ["-DOIIO_BUILD_TOOLS=OFF", "-DOIIO_BUILD_TESTS=OFF", "-DOIIO_BUILD_DOCS=OFF", "-DOpenImageIO_BUILD_MISSING_DEPS=OFF", "-DEMBEDPLUGINS=ON", "-DUSE_PYTHON=OFF", "-DUSE_QT=OFF", "-DUSE_FFMPEG=OFF", "-DUSE_OPENCV=OFF", "-DUSE_OPENVDB=OFF", "-DUSE_TBB=OFF", "-DUSE_SIMD=0", "-DUSE_WEBP=ON", "-DUSE_OPENJPEG=ON"]),
}

C_FLAGS = "-pthread -msimd128 -fwasm-exceptions"
CXX_FLAGS = C_FLAGS
CXX_PACKAGES = {"EMBREE", "RUBBERBAND", "AOM", "X265", "DRACO", "MESHOPTIMIZER", "ABSEIL", "CERES", "HARFBUZZ", "FMT", "EIGEN", "IMATH", "OPENJPH", "OPENEXR", "YAMLCPP", "PYSTRING",
                "OPENCOLORIO", "TBB", "OPENSUBDIV", "MANIFOLD", "ALEMBIC", "OPENVDB", "PUGIXML", "MATERIALX", "USD", "OPENIMAGEIO", "TIFF"}


def run(command, log, cwd=None, env=None):
    with log.open("a") as output:
        output.write("\n" + " ".join(map(str, command)) + "\n")
        output.flush()
        result = subprocess.run(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT, cwd=cwd, env=env)
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
        # Some pinned recipes append a friendly archive filename after the ref.
        if ref.endswith("/" + entry["FILE"]):
            ref = ref.removesuffix("/" + entry["FILE"])
        archive_format = "zip" if ref.endswith(".zip") else "tar.gz"
        url = f"https://codeload.github.com/{repo}/{archive_format}/{ref.removesuffix('.' + archive_format)}"
    urls = [url]
    if url.startswith("https://gmplib.org/download/gmp/"):
        # Some CI clients receive an HTTP-200 bot challenge from gmplib.org.
        # GNU's mirror carries the same archive; keep Blender's exact checksum.
        urls.append("https://ftp.gnu.org/gnu/gmp/" + entry["FILE"])
    temporary = archive.with_suffix(archive.suffix + ".partial")
    failures = []
    for url in urls:
        try:
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            actual = hashlib.new(algorithm, temporary.read_bytes()).hexdigest()
            if actual == entry["HASH"]:
                temporary.replace(archive)
                return archive
            failures.append(f"Checksum mismatch from {url}: {actual}")
        except (urllib.error.URLError, TimeoutError) as error:
            failures.append(f"Download failed from {url}: {error}")
        finally:
            temporary.unlink(missing_ok=True)
    raise RuntimeError(f"Cannot download {entry['FILE']}: " + "; ".join(failures))


def install_notices(source, prefix, name):
    # Keep notices alongside the cached prefix. CI may restore installed
    # libraries without restoring their extracted source trees.
    destination = prefix / 'share/blender-web/licenses' / name.lower()
    destination.mkdir(parents=True, exist_ok=True)
    for notice in source.iterdir():
        if notice.name.upper().startswith(('LICENSE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'PATENTS')):
            if notice.is_file():
                shutil.copy2(notice, destination / notice.name)
            elif notice.is_dir():
                shutil.copytree(notice, destination / notice.name, dirs_exist_ok=True)
    if name == 'MATERIALX':
        shutil.copy2(source / 'source/MaterialXFormat/External/PugiXML/readme.txt',
                     destination / 'PugiXML-1.9-MIT.txt')


def build(args):
    if not shutil.which("emcmake"):
        raise RuntimeError("Activate the pinned Emscripten SDK before building dependencies.")
    # Draco 1.5 requires the legacy SDK path even with its JS glue disabled.
    os.environ["EMSCRIPTEN"] = str(Path(shutil.which("emcc")).resolve().parent)
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
        if name == 'BROTLI':
            signature['staticPkgConfig'] = 2
        if name in {"HARFBUZZ", "FRIBIDI", "RUBBERBAND"}:
            signature["textBuilder"] = hashlib.sha256(
                (ROOT / "web/tools/native_text.py").read_bytes()).hexdigest()
        if name in {"OPUS", "LAME", "THEORA", "VPX", "X264", "FFMPEG"}:
            signature["mediaBuilder"] = hashlib.sha256(
                (ROOT / "web/tools/native_media.py").read_bytes()).hexdigest()
        patch = ROOT / "web/native/patches" / f"{name.lower()}.patch"
        if patch.exists():
            signature["patch"] = hashlib.sha256(patch.read_bytes()).hexdigest()
        if name in {"FFMPEG", "RUBBERBAND"}:
            dependencies = ("AOM", "X265", "X264", "VPX", "OPUS", "LAME", "VORBIS", "THEORA") if name == "FFMPEG" else ("FFTW",)
            signature["dependencyStamps"] = {}
            for dependency in dependencies:
                dependency_stamp = base / dependency.lower() / 'installed.json'
                if not dependency_stamp.is_file():
                    raise RuntimeError(f"Build {dependency} before {name}")
                signature["dependencyStamps"][dependency] = hashlib.sha256(dependency_stamp.read_bytes()).hexdigest()
        if name == "EMBREE":
            dependency_stamp = base / 'tbb/installed.json'
            if not dependency_stamp.is_file():
                raise RuntimeError("Build TBB before EMBREE")
            signature['dependencyStamps'] = {'TBB': hashlib.sha256(dependency_stamp.read_bytes()).hexdigest()}
        if name == "CERES":
            signature["dependencyStamps"] = {}
            for dependency in ("ABSEIL", "EIGEN"):
                dependency_stamp = base / dependency.lower() / 'installed.json'
                if not dependency_stamp.is_file():
                    raise RuntimeError(f"Build {dependency} before CERES")
                signature["dependencyStamps"][dependency] = hashlib.sha256(
                    dependency_stamp.read_bytes()).hexdigest()
        if name == "USD":
            signature["dependencyStamps"] = {}
            for dependency in ("MATERIALX", "TBB", "OPENSUBDIV"):
                dependency_stamp = base / dependency.lower() / 'installed.json'
                if not dependency_stamp.is_file():
                    raise RuntimeError(f"Build {dependency} before USD")
                signature["dependencyStamps"][dependency] = hashlib.sha256(
                    dependency_stamp.read_bytes()).hexdigest()
            # USD's original Blender hooks exchange Python pxr objects. Build
            # the same embedded CPython before cross-compiling its bindings.
            from native_app_deps import build as build_app_dependencies
            python_report = build_app_dependencies(base.parent, args.jobs, only={"python"})
            signature["python"] = python_report[0]["signature"]
            signature["staticBindings"] = hashlib.sha256(
                (ROOT / 'web/tools/native_usd.py').read_bytes()).hexdigest()
        cached = stamp.exists() and json.loads(stamp.read_text()) == json.loads(json.dumps(signature))
        notices = prefix / 'share/blender-web/licenses' / name.lower()
        notices_ready = notices.is_dir()
        if name == 'MATERIALX':
            notices_ready = notices_ready and (notices / 'PugiXML-1.9-MIT.txt').is_file()
        if cached and notices_ready:
            if (directory / 'source').is_dir():
                install_notices(directory / 'source', prefix, name)
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
            if zipfile.is_zipfile(archives[name]):
                with zipfile.ZipFile(archives[name]) as archive:
                    archive.extractall(extracted)
            else:
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
        if name == "POTRACE":
            shutil.copy2(ROOT / "blender/build_files/build_environment/patches/cmakelists_potrace.txt", source / "CMakeLists.txt")
        log = directory / "build.log"
        subdir, options = RECIPES[name]
        options = list(options)
        if name == "FREETYPE":
            options += [f"-DBROTLIDEC_INCLUDE_DIRS={prefix / 'include'}",
                        f"-DBROTLIDEC_LIBRARIES={prefix / 'lib/libbrotlidec-static.a'};{prefix / 'lib/libbrotlicommon-static.a'}"]
        if name == "USD":
            options += [f"-DPython3_EXECUTABLE={base.parent / 'gpu-deps/build/python-native/python'}",
                        f"-DPython3_INCLUDE_DIR={prefix / 'include/python3.13'}",
                        f"-DPython3_LIBRARY={prefix / 'lib/libpython3.13.a'}",
                        "-DPXR_PY_UNDEFINED_DYNAMIC_LOOKUP=ON"]
        configured = directory / "configured.json"
        configuration = json.loads(json.dumps({**signature, "options": options}))
        previous = json.loads(configured.read_text()) if configured.exists() else {}
        if any(previous.get(key) != configuration.get(key) for key in ("toolchain", "sdk", "source")):
            shutil.rmtree(directory / "build", ignore_errors=True)
        configured.write_text(json.dumps(configuration, indent=2) + "\n")
        if name in {"HARFBUZZ", "FRIBIDI", "RUBBERBAND"}:
            from native_text import build_meson
            build_meson(source, directory / "build", prefix, options,
                        args.jobs, log, run, C_FLAGS)
            stamp.write_text(json.dumps(signature, indent=2) + "\n")
            continue
        if name in {"OPUS", "LAME", "THEORA", "VPX", "X264", "FFMPEG"}:
            from native_media import build_autoconf
            build_autoconf(name, source, directory / "build", prefix, options,
                           args.jobs, log, run, C_FLAGS)
            stamp.write_text(json.dumps(signature, indent=2) + "\n")
            continue
        if name == "GMP":
            work = directory / 'build'
            work.mkdir(exist_ok=True)
            run(["env", "CC=emcc", "CXX=em++", "AR=emar", "RANLIB=emranlib", "NM=emnm",
                 f"CFLAGS=-O3 {C_FLAGS}", f"CXXFLAGS=-O3 {CXX_FLAGS}",
                 "emconfigure", source / 'configure', f"--prefix={prefix}",
                 f"--srcdir={source}", *options], log, cwd=work)
            run(["emmake", "make", f"-j{args.jobs}"], log, cwd=work)
            run(["emmake", "make", "install"], log, cwd=work)
            stamp.write_text(json.dumps(signature, indent=2) + "\n")
            continue
        if name == "FFTW":
            # Blender uses double precision for oceans and float/threaded float
            # transforms elsewhere. Install both original implementations.
            for precision in ("double", "float"):
                work = directory / "build" / precision
                run(["emcmake", "cmake", "-S", source, "-B", work,
                     f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'web/native/toolchain.cmake'}",
                     f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_BUILD_TYPE=Release",
                     f"-DCMAKE_C_FLAGS={C_FLAGS}", "-DBUILD_SHARED_LIBS=OFF",
                     f"-DENABLE_FLOAT={'ON' if precision == 'float' else 'OFF'}",
                     *options], log)
                run(["cmake", "--build", work, "--parallel", args.jobs], log)
                run(["cmake", "--install", work], log)
            stamp.write_text(json.dumps(signature, indent=2) + "\n")
            continue
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
        if name == "USD":
            from native_usd import install_bindings
            print(json.dumps(install_bindings(prefix)), flush=True)
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
            for pc in (directory / 'build').glob('libbrotli*.pc'):
                content = pc.read_text().replace(' -R${libdir}', '')
                for codec in ('common', 'dec', 'enc'):
                    content = content.replace(f'-lbrotli{codec}', f'-lbrotli{codec}-static')
                (prefix / 'lib/pkgconfig' / pc.name).write_text(content)
        stamp.write_text(json.dumps(signature, indent=2) + "\n")
    print(f"Wasm dependency prefix: {prefix}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/native/deps")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("packages", nargs="*", choices=list(RECIPES))
    build(parser.parse_args())
