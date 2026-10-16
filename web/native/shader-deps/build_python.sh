#!/usr/bin/env bash
# Adapted from HeyPuter/blender-wasm, GPL-2.0-or-later. See ../THIRD_PARTY.md.
# CPython 3.13.13 → wasm-sysroot (static libpython, embedded into Blender).
# Two-step emscripten cross-build per CPython's Tools/wasm/README.md:
#   1) a native build-python of the SAME version (freezes stdlib during cross-build)
#   2) the wasm cross-build with --enable-wasm-pthreads (matches our -pthread ABI)
set -euo pipefail
source "$(dirname "$0")/dep_common.sh"
PYSRC=$(fetch_extract \
  "https://www.python.org/ftp/python/3.13.13/Python-3.13.13.tar.xz" \
  "Python-3.13.13.tar.xz" "Python-3.13.13")

# Keep CPython's bundled Expat private when Blender also links Expat for OCIO.
if ! grep -q '^#define g_reparseDeferralEnabledDefault ' "$PYSRC/Modules/expat/pyexpatns.h"; then
  patch -d "$PYSRC" -p1 -i "$ROOT/web/native/shader-deps/python-expat-namespace.patch"
fi

# 1) The host generator needs zlib to package the standard library. Build it
# from the same verified source as the target zlib; system dev headers are not
# required on the build machine.
HOST_PREFIX="$DEPS/host-prefix"
if [ ! -f "$HOST_PREFIX/lib/libz.a" ]; then
  # Dependency caches may contain only installed libraries and source archives.
  python3 - "$BASE" <<'ZLIB_SOURCE'
import hashlib, json, pathlib, tarfile
base = pathlib.Path(__import__('sys').argv[1])
source = base / 'deps/zlib/source'
if not source.exists():
    lock = json.loads((base / 'deps/dependencies.json').read_text())['ZLIB']
    archive = base / 'deps/downloads' / lock['FILE']
    if hashlib.new(lock['HASH_TYPE'].lower(), archive.read_bytes()).hexdigest() != lock['HASH']:
        raise SystemExit('Host zlib source checksum mismatch')
    temporary = base / 'deps/zlib/extract-host'
    temporary.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as bundle:
        bundle.extractall(temporary, filter='data')
    roots = list(temporary.iterdir())
    if len(roots) != 1:
        raise SystemExit('Unexpected zlib archive layout')
    roots[0].rename(source)
    temporary.rmdir()
    (base / 'deps/zlib/source.json').write_text(json.dumps(lock, indent=2)+'\n')
ZLIB_SOURCE
  cmake -S "$BASE/deps/zlib/source" -B "$BLD/zlib-native" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$HOST_PREFIX" -DZLIB_BUILD_EXAMPLES=OFF
  cmake --build "$BLD/zlib-native" --parallel "$NPROC"
  cmake --install "$BLD/zlib-native"
fi
NATIVE="$BLD/python-native"
if [ ! -x "$NATIVE/python" ] || ! "$NATIVE/python" -c "import binascii, zlib" 2>/dev/null; then
  log "building native build-python 3.13.13 (full)"
  mkdir -p "$NATIVE"; ( cd "$NATIVE"
    rm -f config.cache
    ZLIB_CFLAGS="-I$HOST_PREFIX/include" ZLIB_LIBS="$HOST_PREFIX/lib/libz.a" \
      "$PYSRC/configure" >/dev/null
    make -j"$NPROC" >/dev/null 2>&1 )
fi
BUILD_PY="$NATIVE/python"
"$BUILD_PY" --version

# 2) WASM cross-build → $SYSROOT.
WB="$BLD/python-wasm"; mkdir -p "$WB"
( cd "$WB"
  # -DPY_CALL_TRAMPOLINE: enable CPython's Emscripten C-function call trampoline.
  # WASM call_indirect enforces exact signature match; CPython calls METH_NOARGS/
  # METH_O C functions through a generic PyCFunctionWithKeywords pointer, which is
  # a bad fpcast that traps ("null function or function signature mismatch") on
  # wasm. The trampoline routes those calls through JS (wasmTable.get + arity
  # reflection) so the real arity is honored. Not auto-defined by 3.13 configure.
  CONFIG_SITE="$PYSRC/Tools/wasm/config.site-wasm32-emscripten" \
  CFLAGS="$WASM_CFLAGS -DPY_CALL_TRAMPOLINE" \
    emconfigure "$PYSRC/configure" -C \
      --host=wasm32-unknown-emscripten \
      --build=x86_64-pc-linux-gnu \
      --enable-wasm-pthreads \
      --with-build-python="$BUILD_PY" \
      --prefix="$SYSROOT" \
      --disable-shared \
      --disable-test-modules \
      --with-ensurepip=no
  emmake make -j"$NPROC"
  emmake make install )

# The _decimal module's bundled libmpdec objects compile but are NOT archived
# into libpython3.13.a by CPython's static-wasm build, so _decimal.o is left with
# undefined mpd_* symbols at the final Blender link. Fold them into the archive.
# Several bundled modules compile their support sources but CPython's static-wasm
# build does NOT archive them into libpython3.13.a, leaving undefined symbols at
# the final Blender link: _decimal's libmpdec (mpd_*), pyexpat/_elementtree's
# vendored expat (PyExpat_XML_*), and hashlib's HACL SHA2 (python_hashlib_*).
# Fold them in. Prefix member names: libmpdec has context.o/io.o whose BASENAMES
# collide with core CPython objects in the flat-namespaced archive (`ar r` keys
# by basename and would silently overwrite Python's own context.o).
_mz=$(mktemp -d)
for _o in "$WB"/Modules/_decimal/libmpdec/*.o \
          "$WB"/Modules/expat/xmlparse.o "$WB"/Modules/expat/xmltok.o "$WB"/Modules/expat/xmlrole.o \
          "$WB"/Modules/_hacl/Hacl_Hash_SHA2.o; do
  [ -f "$_o" ] && cp "$_o" "$_mz/pymod_$(basename "$_o")"
done
if ls "$_mz"/*.o >/dev/null 2>&1; then
  emar r "$SYSROOT/lib/libpython3.13.a" "$_mz"/*.o
  log "archived bundled libmpdec/expat/hacl objects into libpython3.13.a"
fi
rm -rf "$_mz"

log "installed python into $SYSROOT"
ls -la "$SYSROOT"/lib/libpython3.13*.a 2>&1
