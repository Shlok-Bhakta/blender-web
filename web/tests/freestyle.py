# SPDX-License-Identifier: GPL-2.0-or-later
"""A small Cycles CPU render with original Freestyle stroke rendering."""
import bpy
import hashlib
import json
from pathlib import Path


def setup():
    assert bpy.app.build_options.freestyle
    scene=bpy.context.scene
    scene.render.engine='CYCLES'
    scene.cycles.device='CPU';scene.cycles.samples=16
    scene.cycles.use_denoising=False;scene.cycles.use_adaptive_sampling=False
    scene.cycles.seed=0
    scene.render.resolution_x=256;scene.render.resolution_y=256
    scene.render.resolution_percentage=100
    scene.render.threads_mode='FIXED';scene.render.threads=8
    scene.view_settings.view_transform='Standard';scene.view_settings.look='None'
    scene.render.use_freestyle=True
    settings=bpy.context.view_layer.freestyle_settings
    settings.linesets[0].linestyle.color=(1,0,0)
    settings.linesets[0].linestyle.thickness=3
    scene.render.image_settings.file_format='PNG'
    scene.render.image_settings.color_mode='RGBA'
    scene.render.image_settings.color_depth='8'
    return dict(engine=scene.render.engine,samples=scene.cycles.samples,size=[256,256],
                denoising=False,adaptiveSampling=False,freestyle=True,strokeColor=[1,0,0],thickness=3)


def reference(directory):
    settings=setup()
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    bpy.ops.render.render()
    bpy.data.images['Render Result'].save_render(str(directory/'freestyle.png'),scene=bpy.context.scene)
    (directory/'reference.json').write_text(json.dumps(dict(settings=settings,
        blenderVersion=bpy.app.version_string,sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),indent=2)+'\n')
