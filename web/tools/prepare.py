#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare an isolated, patched worktree of the pinned official Blender source."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


GITHUB_MIRROR = "https://github.com/blender/blender.git"
BLENDER_LFS = "https://projects.blender.org/blender/blender.git/info/lfs"


def prepare_source(build_dir):
    # Support the original development checkout as well as the public wrapper repo.
    if (ROOT / "source/blender").is_dir():
        return ROOT
    pins = json.loads((ROOT / "web/upstream.json").read_text())
    upstream = ROOT / "blender"
    env = {**os.environ, "GIT_LFS_SKIP_SMUDGE": "1"}
    if not (upstream / ".git").exists():
        update = ["git", "submodule", "update", "--init", "--depth", "1", "blender"]
        if subprocess.run(update, cwd=ROOT, env=env).returncode:
            # projects.blender.org sometimes refuses CI runners (HTTP 403). The
            # official GitHub mirror has the same commits; the revision check below
            # still pins the result. Large files stay on Blender's own LFS server.
            subprocess.run(["git", "config", "submodule.blender.url", GITHUB_MIRROR], cwd=ROOT, check=True)
            subprocess.run(update, cwd=ROOT, env=env, check=True)
            subprocess.run(["git", "config", "lfs.url", BLENDER_LFS], cwd=upstream, check=True)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=upstream, text=True).strip()
    if revision != pins["blenderRevision"]:
        raise RuntimeError("The Blender submodule does not match web/upstream.json. Update the submodule explicitly before building.")
    source = build_dir / "source"
    source.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        subprocess.run(["git", "worktree", "add", "--detach", str(source), revision],
                       cwd=upstream, env=env, check=True)
    prepared_revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if prepared_revision != revision:
        raise RuntimeError(f"{source} uses a different revision. Preserve any edits and remove that generated worktree before rebuilding.")
    patch = ROOT / "web/patches/ghost-web.patch"
    check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=source, capture_output=True)
    if check.returncode == 0:
        subprocess.run(["git", "apply", str(patch)], cwd=source, check=True)
    else:
        applied = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)], cwd=source, capture_output=True)
        if applied.returncode:
            raise RuntimeError(f"The GHOST shim patch does not apply cleanly in {source}:\n{check.stderr.decode()}")
    destination = source / "intern/ghost/web"
    destination.mkdir(parents=True, exist_ok=True)
    for path in (ROOT / "intern/ghost/web").iterdir():
        if path.is_file():
            shutil.copy2(path, destination / path.name)
    return source


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/web")
    args = parser.parse_args()
    print(prepare_source(args.build_dir.resolve()))
