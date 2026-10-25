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


def build_x265(source, work, prefix, options, jobs, log, run, flags):
    """Build the original 8/10/12-bit namespaces and merge their static objects."""
    from prepare import ROOT
    archives = {}
    for depth in (12, 10, 8):
        build = work / str(depth)
        extra = [f'-DHIGH_BIT_DEPTH={"ON" if depth > 8 else "OFF"}',
                 f'-DEXPORT_C_API={"ON" if depth == 8 else "OFF"}',
                 f'-DMAIN12={"ON" if depth == 12 else "OFF"}']
        if depth == 8:
            extra += ['-DLINKED_10BIT=ON', '-DLINKED_12BIT=ON',
                      '-DEXTRA_LIB=' + ';'.join(str(archives[value]) for value in (10, 12))]
        run(['emcmake', 'cmake', '-S', source / 'source', '-B', build,
             f'-DCMAKE_TOOLCHAIN_FILE={ROOT / "web/native/toolchain.cmake"}',
             f'-DCMAKE_INSTALL_PREFIX={prefix}', '-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_C_FLAGS={flags}', f'-DCMAKE_CXX_FLAGS={flags}',
             *options, *extra], log)
        run(['cmake', '--build', build, '--parallel', jobs], log)
        archives[depth] = build / 'libx265.a'
    run(['cmake', '--install', work / '8'], log)
    merged = prefix / 'lib/libx265-multilib.a'
    merged.unlink(missing_ok=True)
    # q appends duplicate object basenames from the different namespaces;
    # replacing members by basename would silently discard other bit depths.
    run(['emar', 'qcsL', merged, *(archives[depth] for depth in (8, 10, 12))], log)
    merged.replace(prefix / 'lib/libx265.a')
    pc = prefix / 'lib/pkgconfig/x265.pc'
    pc.write_text('\n'.join('Libs.private:' if line.startswith('Libs.private:') else line
                            for line in pc.read_text().splitlines()) + '\n')
