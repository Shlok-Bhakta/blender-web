# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare a native 50-frame animation and timestamp completed PNG frames."""
import bpy
import json
import math
import os
import time

scene = bpy.context.scene
cube = scene.objects['Cube']
assert scene.render.engine == 'BLENDER_EEVEE'
assert (scene.render.resolution_x, scene.render.resolution_y,
        scene.render.resolution_percentage, scene.eevee.taa_render_samples) == (1920, 1080, 100, 64)
assert scene.render.image_settings.file_format == 'PNG'
scene.frame_start = 1
scene.frame_end = SPIN_FRAME_COUNT
scene.frame_step = 1
scene.render.filepath = '/tmp/spinning-cube/frame_'
os.makedirs('/tmp/spinning-cube', exist_ok=True)
cube.rotation_euler.z = 0
cube.keyframe_insert(data_path='rotation_euler', index=2, frame=1)
cube.rotation_euler.z = 2 * math.pi
cube.keyframe_insert(data_path='rotation_euler', index=2, frame=SPIN_FRAME_COUNT + 1)
action = cube.animation_data.action
for curve in action.layers[0].strips[0].channelbags[0].fcurves:
    for key in curve.keyframe_points:
        key.interpolation = 'LINEAR'
spin_rotation_curve = action.layers[0].strips[0].channelbags[0].fcurves[0]
scene.frame_set(1)


def spin_settings():
    return dict(engine=scene.render.engine, width=scene.render.resolution_x,
                height=scene.render.resolution_y, percent=scene.render.resolution_percentage,
                samples=scene.eevee.taa_render_samples,
                image_format=scene.render.image_settings.file_format,
                color_mode=scene.render.image_settings.color_mode,
                color_depth=scene.render.image_settings.color_depth,
                view_transform=scene.view_settings.view_transform,
                look=scene.view_settings.look, exposure=scene.view_settings.exposure,
                gamma=scene.view_settings.gamma, fps=scene.render.fps,
                fps_base=scene.render.fps_base,
                eevee={p.identifier: getattr(scene.eevee, p.identifier)
                       for p in scene.eevee.bl_rna.properties
                       if p.type in ('BOOLEAN', 'INT', 'FLOAT', 'STRING', 'ENUM')
                       and not p.is_readonly and not getattr(p, 'is_array', False)})


def spin_emit(marker, value):
    os.write(1, (marker + ':' + json.dumps(value, separators=(',', ':')) + '\n').encode())


spin_before = spin_settings()
spin_rows = []
spin_pre_times = {}
spin_started = spin_previous = None


def spin_init(scene, *args):
    global spin_started, spin_previous
    spin_started = spin_previous = time.perf_counter()
    spin_emit('SPIN_BEGIN', dict(frames=SPIN_FRAME_COUNT, settings=spin_before))


def spin_pre(scene, *args):
    spin_pre_times[scene.frame_current] = time.perf_counter()


def spin_post(scene, *args):
    global spin_previous
    # Blender's animation render_post runs after the normal PNG writer.
    done = time.perf_counter()
    frame = scene.frame_current
    filename = scene.render.frame_path(frame=frame)
    with open(filename, 'rb') as file:
        header = file.read(24)
    assert header[:8] == b'\x89PNG\r\n\x1a\n'
    assert (int.from_bytes(header[16:20], 'big'), int.from_bytes(header[20:24], 'big')) == (1920, 1080)
    row = dict(frame=frame, elapsedSeconds=done - spin_started,
               frameSeconds=done - spin_previous,
               renderAndWriteSeconds=done - spin_pre_times[frame],
               keyframedRotationZ=spin_rotation_curve.evaluate(frame),
               bytes=os.stat(filename).st_size, path=filename)
    spin_previous = done
    spin_rows.append(row)
    spin_emit('SPIN_FRAME', row)


def spin_complete(scene, *args):
    end = time.perf_counter()
    spin_emit('SPIN_COMPLETE', dict(frames=len(spin_rows),
                                  totalSeconds=end - spin_started,
                                  firstFrameSeconds=spin_rows[0]['elapsedSeconds'],
                                  settingsAfter=spin_settings()))


def spin_cancel(scene, *args):
    spin_emit('SPIN_CANCEL', dict(frames=len(spin_rows)))


for handlers, callback in [(bpy.app.handlers.render_init, spin_init),
                           (bpy.app.handlers.render_pre, spin_pre),
                           (bpy.app.handlers.render_post, spin_post),
                           (bpy.app.handlers.render_complete, spin_complete),
                           (bpy.app.handlers.render_cancel, spin_cancel)]:
    handlers.append(callback)
spin_emit('SPIN_READY', dict(frames=SPIN_FRAME_COUNT, settings=spin_before))
