#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install original OSL libraries and a genuine Node-hosted oslc build command."""
import json
import os
import shlex
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

from native_llvm import host_tools
from prepare import ROOT

PATCHES = ['web/native/patches/osl-runtime.patch', 'web/native/patches/osl-owned.patch',
           'web/native/patches/osl-closures.patch']
HEADERS = ['web/native/ghost/GHOST_WebOSL.hh', 'web/native/ghost/GHOST_WebOSLOwnedProbe.hh']
ADAPTERS = ['web/native/osl-query-probe/cmake/OSLDependencies.cmake',
            'web/native/osl-query-probe/cmake/FindLLVM.cmake']


def isolate_flex(executable, directory):
    """Keep OSL's inferred Flex include directory out of the native sysroot."""
    directory = Path(directory).resolve()
    binary, headers = directory / 'bin/flex', directory / 'include'
    binary.parent.mkdir(parents=True, exist_ok=True)
    headers.mkdir(parents=True, exist_ok=True)
    # OSL derives ../include from FLEX_EXECUTABLE. A real launcher, rather than
    # a symlink, keeps that path isolated even when the tool is /usr/bin/flex.
    binary.write_text('#!/bin/sh\nexec ' + shlex.quote(str(executable)) + ' "$@"\n')
    binary.chmod(0o755)
    original = Path(executable).parents[1] / 'include/FlexLexer.h'
    header = headers / 'FlexLexer.h'
    if original.is_file():
        shutil.copy2(original, header)
    else:
        header.unlink(missing_ok=True)
    return binary


def llvm_source(directory, lock, downloads):
    from native_deps import download
    source = directory / 'source'
    receipt = directory / 'source.json'
    if source.exists():
        if not receipt.exists() or json.loads(receipt.read_text()) != lock:
            raise RuntimeError('The original LLVM source receipt does not match its installed libraries')
        return source
    archive = download(lock, downloads)
    with tempfile.TemporaryDirectory(dir=directory, prefix='extract-host-') as temporary:
        with tarfile.open(archive) as bundle:
            bundle.extractall(temporary, filter='data')
        roots = list(Path(temporary).iterdir())
        if len(roots) != 1 or not (roots[0] / 'llvm/CMakeLists.txt').is_file():
            raise RuntimeError('Unexpected original LLVM archive layout')
        shutil.move(roots[0], source)
    receipt.write_text(json.dumps(lock, indent=2) + '\n')
    return source


def build_library(source, work, prefix, options, jobs, log, run, flags):
    from native_llvm import digest
    directory = work.parent
    dependencies = directory.parent
    llvm = dependencies / 'llvm'
    llvm_lock = json.loads((llvm / 'installed.json').read_text())['source']
    original_llvm = llvm_source(llvm, llvm_lock, dependencies / 'downloads')
    env = dict(os.environ)
    for name in ('PKG_CONFIG_LIBDIR', 'PKG_CONFIG_PATH', 'EM_PKG_CONFIG_PATH'):
        env.pop(name, None)
    tools = {}
    for name in ('flex', 'bison', 'm4'):
        value = os.environ.get('BLENDER_WEB_' + name.upper()) or shutil.which(name)
        if not value or not Path(value).is_file():
            raise RuntimeError(f'Original OSL needs native {name}; provide BLENDER_WEB_{name.upper()}')
        tools[name] = str(Path(value).resolve())
    env['M4'] = tools['m4']
    flex = isolate_flex(tools['flex'], directory / 'host-tools')
    bison_data = Path(tools['bison']).parents[1] / 'share/bison'
    if bison_data.is_dir():
        env['BISON_PKGDATADIR'] = str(bison_data)
    host, native_hashes = host_tools(original_llvm, llvm, llvm_lock, jobs, log, run, env)
    for name in PATCHES:
        patch = ROOT / name
        check = subprocess.run(['patch', '--silent', '--dry-run', '--forward',
            '-p1', '-i', str(patch)], cwd=source, capture_output=True)
        if check.returncode == 0:
            run(['patch', '--batch', '-p1', '-i', patch], log, cwd=source, env=env)
        elif subprocess.run(['patch', '--silent', '--dry-run', '--reverse',
                '-p1', '-i', str(patch)], cwd=source, capture_output=True).returncode:
            raise RuntimeError(f'Preserve and inspect original OSL sources: {patch.name} does not apply')
    sdk = Path(shutil.which('emcc')).resolve().parent
    generator = ';'.join(map(str, [sys.executable, ROOT / 'web/tools/native_osl_bitcode.py',
        '--compiler', host / 'bin/clang++', '--sdk', sdk, '--']))
    adapter = ROOT / 'web/native/osl-query-probe/cmake'
    executable_flags = ('-pthread -fwasm-exceptions -sNODERAWFS=1 -sENVIRONMENT=node '
        '-sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=134217728 '
        '-sMAXIMUM_MEMORY=4294967296 -sSTACK_SIZE=8388608 -sPTHREAD_POOL_SIZE=4')
    run(['emcmake', 'cmake', '-G', 'Unix Makefiles', '-S', source, '-B', work,
         f'-DCMAKE_PROJECT_OSL_INCLUDE={adapter / "OSLDependencies.cmake"}',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_STANDARD=17', '-DBUILD_SHARED_LIBS=OFF',
         f'-DCMAKE_C_FLAGS={flags}',
         f'-DCMAKE_CXX_FLAGS={flags} -I{ROOT / "web/native/ghost"} -DWITH_GHOST_WEB_OSL_OWNED_PROBE=1',
         f'-DCMAKE_EXE_LINKER_FLAGS={executable_flags}', f'-DCMAKE_INSTALL_PREFIX={prefix}',
         f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
         '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY', f'-DCMAKE_MODULE_PATH={adapter}',
         f'-DLLVM_DIR={prefix / "lib/cmake/llvm"}', f'-DClang_DIR={prefix / "lib/cmake/clang"}',
         f'-DLLD_DIR={prefix / "lib/cmake/lld"}', '-DLLVM_STATIC=ON', '-DUSE_LLVM_BITCODE=ON',
         f'-DLLVM_BC_GENERATOR={generator}', f'-DLLVM_AS_TOOL={host / "bin/llvm-as"}',
         f'-DLLVM_LINK_TOOL={host / "bin/llvm-link"}', '-DOSL_BUILD_TESTS=OFF',
         '-DOSL_BUILD_SHADERS=OFF', '-DOSL_BUILD_PLUGINS=OFF', '-DUSE_PYTHON=OFF', '-DUSE_QT=OFF',
         '-DUSE_PARTIO=OFF', '-DOSL_USE_OPTIX=OFF', '-DSTOP_ON_WARNING=OFF', '-DINSTALL_DOCS=OFF',
         f'-DFLEX_EXECUTABLE={flex}', f'-DBISON_EXECUTABLE={tools["bison"]}', *options], log, env=env)
    # These are the original installed/exported targets. The default "all"
    # additionally runs BSDL's uninstalled native-only table generator, whose
    # tables are not consumed by any of OSL's four runtime/compiler libraries.
    run(['cmake', '--build', work, '--target', 'oslcomp', 'oslexec', 'oslnoise',
         'oslquery', 'oslc', 'oslinfo', '--parallel', jobs], log, env=env)
    run(['cmake', '--install', work, '--prefix', prefix], log, env=env)
    # CMake installs the executable's .js target, but Emscripten's companion
    # .wasm is not an install target. Preserve the actual linker output too.
    for name in ('oslc', 'oslinfo'):
        for extension in ('.js', '.wasm'):
            artifact = work / 'bin' / (name + extension)
            if not artifact.is_file():
                raise RuntimeError(f'The original OSL build did not produce {artifact.name}')
            shutil.copy2(artifact, prefix / 'bin' / artifact.name)
    # Shader builds are disabled during cross-compilation, but the original
    # compiler still needs the standard shader declarations and their includes.
    shaders = prefix / 'share/OSL/shaders'
    shaders.mkdir(parents=True, exist_ok=True)
    for header in (source / 'src/shaders').glob('*.h'):
        shutil.copy2(header, shaders / header.name)
    compiler = prefix / 'bin/oslc.js'
    if not compiler.is_file() or not compiler.with_suffix('.wasm').is_file():
        raise RuntimeError('The original OSL compiler was not installed')
    launcher = prefix / 'bin/oslc'
    launcher.write_text('#!/bin/sh\n'
        'exec node "$(dirname "$0")/oslc.js" -I"$(dirname "$0")/../share/OSL/shaders" "$@"\n')
    launcher.chmod(0o755)
    report = dict(nativeTools=native_hashes, source=json.loads((directory / 'source.json').read_text()),
                  runtimePatches={name: digest(ROOT / name) for name in PATCHES},
                  compilerJavaScript=digest(compiler), compilerWasm=digest(compiler.with_suffix('.wasm')),
                  libraries={name: digest(prefix / f'lib/lib{name}.a')
                             for name in ('oslcomp', 'oslexec', 'oslnoise', 'oslquery')})
    (directory / 'runtime-installation.json').write_text(json.dumps(report, indent=2) + '\n')
