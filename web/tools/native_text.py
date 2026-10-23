#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the pinned text libraries with Meson's explicit Wasm cross file."""
import os
import shlex


def build_meson(source, work, prefix, options, jobs, log, run, flags):
    cross = work.parent / 'wasm.ini'
    args = repr(shlex.split(flags))
    cross.write_text(f'''[binaries]
c = 'emcc'
cpp = 'em++'
ar = 'emar'
strip = 'true'
pkg-config = 'pkg-config'
[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = {args}
cpp_args = {args}
c_link_args = {args}
cpp_link_args = {args}
''')
    env = dict(os.environ, PKG_CONFIG_LIBDIR=os.pathsep.join(
        str(prefix / part) for part in ('lib/pkgconfig', 'share/pkgconfig')),
        PKG_CONFIG_PATH='')
    # Match the release's host build tool, without installing into system Python.
    tools = prefix.parent / 'text-tools'
    if not (tools / 'bin/python').exists():
        run(['uv', 'venv', tools], log)
    run(['uv', 'pip', 'install', '--python', tools / 'bin/python',
         'meson==1.9.0', 'ninja==1.13.2'], log)
    env['PATH'] = str(tools / 'bin') + os.pathsep + env['PATH']
    meson = [tools / 'bin/meson']
    setup = ['setup', '--reconfigure'] if (work / 'meson-private/coredata.dat').exists() else ['setup']
    run([*meson, *setup, work, source, '--cross-file', cross, '--prefix', prefix,
         '--libdir', 'lib', '--default-library', 'static', '--buildtype', 'release',
         '--wrap-mode', 'nofallback', *options], log, env=env)
    run([*meson, 'compile', '-C', work, '-j', jobs], log, env=env)
    run([*meson, 'install', '-C', work], log, env=env)
