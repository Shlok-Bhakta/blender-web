#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make an Emscripten Node generator directly executable by upstream build rules."""
from pathlib import Path
import subprocess
import sys

node = sys.argv[1]
if not Path(node).is_file():
    raise RuntimeError(f"Expected an absolute Node executable, got {node!r}")
command = sys.argv[2:]
subprocess.run(command, check=True)
path = Path(command[command.index("-o") + 1])
data = path.read_bytes()
if not data.startswith(b"#!"):
    path.write_bytes(f"#!{node}\n".encode() + data)
path.chmod(path.stat().st_mode | 0o111)
