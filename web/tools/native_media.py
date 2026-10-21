#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cross-build Blender's media libraries without executing target binaries."""
import os


def build_autoconf(name, source, work, prefix, options, jobs, log, run, flags):
    work.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "CC": "emcc", "CXX": "em++", "AR": "emar",
           "RANLIB": "emranlib", "NM": "emnm", "STRIP": "true",
           "CFLAGS": f"-O3 {flags}", "CXXFLAGS": f"-O3 {flags}",
           "CPPFLAGS": f"-I{prefix / 'include'}", "LDFLAGS": f"{flags} -L{prefix / 'lib'}",
           "EM_PKG_CONFIG_PATH": os.pathsep.join(str(prefix / part)
                                               for part in ('lib/pkgconfig', 'share/pkgconfig'))}
    extra = []
    if name == "FFMPEG":
        extra = ["--cc=emcc", "--cxx=em++", "--ar=emar", "--nm=emnm",
                 "--ranlib=emranlib", "--strip=true", "--dep-cc=emcc", "--pkg-config=pkg-config",
                 f"--extra-cflags=-O3 {flags} -I{prefix / 'include'}",
                 f"--extra-ldflags={flags} -L{prefix / 'lib'}"]
    if name == "X264":
        env["AS"] = "emcc"
    if name == "VPX":
        extra = [f"--extra-cflags=-O3 {flags}", f"--extra-cxxflags=-O3 {flags}"]
    run(["emconfigure", source / "configure", f"--prefix={prefix}", *options, *extra],
        log, cwd=work, env=env)
    run(["emmake", "make", f"-j{jobs}"], log, cwd=work, env=env)
    run(["emmake", "make", "install"], log, cwd=work, env=env)
