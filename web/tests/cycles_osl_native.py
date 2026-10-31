# SPDX-License-Identifier: GPL-2.0-or-later
"""Render the shared Cycles OSL scene with native Blender.

blender -b --factory-startup --python web/tests/cycles_osl_native.py -- output.png
"""
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cycles_osl import configure_cycles_osl  # noqa: E402

scene = configure_cycles_osl()
scene.render.filepath = sys.argv[sys.argv.index('--') + 1]
bpy.ops.render.render(write_still=True)
