# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared Cycles settings and render-handler timers for native/browser comparison."""
import bpy
import json
import os
import time

_cycles_timings = []
_cycles_started = 0.0


def _cycles_pre(scene):
    global _cycles_started
    _cycles_started = time.perf_counter()


def _cycles_post(scene):
    seconds = time.perf_counter() - _cycles_started
    _cycles_timings.append(seconds)
    os.write(1, ('CYCLES_TIMING:' + json.dumps(dict(run=len(_cycles_timings)-1, seconds=seconds)) + '\n').encode())


def configure_cycles_timing():
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 16
    scene.cycles.use_denoising = False
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.seed = 0
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 8
    scene.render.resolution_x = scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.filepath = '/tmp/web-cycles.png'
    bpy.app.handlers.render_pre.append(_cycles_pre)
    bpy.app.handlers.render_post.append(_cycles_post)


def native_cycles_timing():
    configure_cycles_timing()
    for _ in range(3):
        assert bpy.ops.render.render() == {'FINISHED'}
    report = dict(blenderVersion=bpy.app.version_string, engine='CYCLES', device='CPU',
                  threads=8, size=[128,128], samples=16, seed=0,
                  denoising=False, adaptiveSampling=False, handlerSeconds=_cycles_timings)
    os.write(1, ('CYCLES_NATIVE_COMPLETE:' + json.dumps(report) + '\n').encode())
