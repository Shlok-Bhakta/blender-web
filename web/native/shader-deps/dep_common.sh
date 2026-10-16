# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
ROOT="${ROOT:?Set ROOT to repository root}"
BASE="${BLENDER_WEB_NATIVE_BASE:-$ROOT/build/native}"
SYSROOT="$BASE/deps/prefix"
DEPS="$BASE/gpu-deps"
DL="$DEPS/downloads"
SRC="$DEPS/src"
BLD="$DEPS/build"
NPROC="${NPROC:-3}"
WASM_CFLAGS="-O2 -pthread -msimd128 -fwasm-exceptions"
WASM_CXXFLAGS="$WASM_CFLAGS"
mkdir -p "$DL" "$SRC" "$BLD" "$SYSROOT"
log() { echo ">> $(basename "$0"): $*" >&2; }
fetch_extract() {
  local url="$1" file="$2" dir="$3"
  if [ ! -f "$DL/$file" ]; then
    curl -fL --retry 3 -o "$DL/$file.tmp" "$url"
    mv "$DL/$file.tmp" "$DL/$file"
  fi
  if [[ "$file" == Python-* ]]; then
    python3 - "$ROOT" "$DL/$file" <<'CHECK'
import hashlib, pathlib, re, sys
root, archive = map(pathlib.Path, sys.argv[1:])
versions = (root / 'blender/build_files/build_environment/cmake/versions.cmake').read_text()
algorithm = re.search(r'set\(PYTHON_HASH_TYPE ([^)]+)\)', versions)[1].lower()
expected = re.search(r'set\(PYTHON_HASH ([^)]+)\)', versions)[1]
if hashlib.new(algorithm, archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('CPython archive does not match the pinned Blender release')
CHECK
  fi
  if [ ! -d "$SRC/$dir" ]; then tar -C "$SRC" -xf "$DL/$file"; fi
  echo "$SRC/$dir"
}
