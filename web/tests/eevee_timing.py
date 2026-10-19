# SPDX-License-Identifier: GPL-2.0-or-later
"""Render-handler timing for the unchanged factory Eevee scene in both builds."""
import bpy
import json
import os
import time

_eevee_started = 0.0
_eevee_timings = []


def _eevee_pre(scene):
    global _eevee_started
    _eevee_started = time.perf_counter()


def _eevee_post(scene):
    seconds = time.perf_counter() - _eevee_started
    _eevee_timings.append(seconds)
    os.write(1, ('EEVEE_TIMING:' + json.dumps(dict(run=len(_eevee_timings)-1, seconds=seconds)) + '\n').encode())


def configure_eevee_timing():
    scene = bpy.context.scene
    assert scene.render.engine == 'BLENDER_EEVEE'
    assert (scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage) == (1920,1080,100)
    assert scene.eevee.taa_render_samples == 64
    bpy.app.handlers.render_pre.append(_eevee_pre)
    bpy.app.handlers.render_post.append(_eevee_post)


def native_eevee_timing():
    import gpu
    configure_eevee_timing()
    for _ in range(3):
        assert bpy.ops.render.render() == {'FINISHED'}
    adapter = dict(vendor=gpu.platform.vendor_get(), renderer=gpu.platform.renderer_get(),
                   backend=gpu.platform.backend_type_get())
    assert not any(word in json.dumps(adapter).lower() for word in ('llvmpipe','swiftshader','lavapipe','software'))
    report = dict(blenderVersion=bpy.app.version_string, engine='BLENDER_EEVEE',
                  size=[1920,1080], samples=64, adapter=adapter,
                  handlerSeconds=_eevee_timings)
    os.write(1, ('EEVEE_NATIVE_COMPLETE:' + json.dumps(report) + '\n').encode())
