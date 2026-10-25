#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare the checksum-pinned host ISPC compiler used by OIDN's Wasm bridge."""
import json
import platform
import shutil
import tarfile

from prepare import ROOT

INPUTS = ('web/native/ispc-host.json', 'web/native/oidn/half.cpp',
          'web/tools/native_oidn.py', 'web/tools/native_ispc.py',
          'web/tools/native_ispc_half.py')


def configure(base, download, prefix):
    pins = json.loads((ROOT / 'web/native/ispc-host.json').read_text())
    host = platform.system().lower() + '-' + platform.machine().lower()
    if host not in pins:
        raise RuntimeError(f'OIDN host compiler has no verified binary for {host}')
    # ISPC's version itself is resolved from Blender, like the library version.
    from subprocess import check_output
    check_output(['cmake', f'-DBLENDER_SOURCE={ROOT / "blender"}',
        '-DDEPENDENCIES=ISPC', f'-DOUTPUT={base / "ispc-source.json"}',
        '-P', str(ROOT / 'web/native/dependency-lock.cmake')], text=True)
    ispc_version = json.loads((base / 'ispc-source.json').read_text())['ISPC']['VERSION']
    if ispc_version.removeprefix('v') != pins['version']:
        raise RuntimeError(f'Update the verified host ISPC binary for Blender ISPC {ispc_version}')
    archive = download(pins[host], base / 'downloads')
    tools = base / 'host-ispc'
    compiler = tools / f'ispc-v{pins["version"]}-linux/bin/ispc'
    if not compiler.is_file():
        tools.mkdir(exist_ok=True)
        with tarfile.open(archive) as source:
            source.extractall(tools, filter='data')
    notices = prefix / 'share/blender-web/licenses/oidn'
    notices.mkdir(parents=True, exist_ok=True)
    for name in ('LICENSE.txt', 'third-party-programs.txt'):
        shutil.copy2(compiler.parent.parent / name, notices / ('ispc-' + name))
    # An explicit launcher keeps CMake rebuilds independent of shell state.
    launcher = base / 'oidn/ispc-wasm'
    launcher.write_text('#!/usr/bin/env python3\nimport sys\n'
        f'sys.path.insert(0, {str(ROOT / "web/tools")!r})\n'
        'from native_ispc import main\n'
        f'raise SystemExit(main({str(compiler)!r}))\n')
    launcher.chmod(0o755)
    return [f'-DISPC_EXECUTABLE={launcher}',
            f'-DOIDN_WEB_HALF_SOURCE={ROOT / "web/native/oidn/half.cpp"}',
            '-DOIDN_WEB_COMPILER_DEPENDS=' + ';'.join(str(ROOT / name) for name in INPUTS)]
