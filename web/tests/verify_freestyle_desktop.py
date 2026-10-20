# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the browser-created Freestyle scene in original desktop Blender."""
import bpy
import hashlib
import json
from pathlib import Path
import sys

base=Path(sys.argv[sys.argv.index('--')+1]).resolve()
report=json.loads((base/'acceptance.json').read_text())
file=base/'freestyle-scene.blend'
assert hashlib.sha256(file.read_bytes()).hexdigest()==report['blendSha256']
bpy.ops.wm.open_mainfile(filepath=str(file),load_ui=False)
scene=bpy.context.scene
assert scene.render.use_freestyle and scene.render.engine=='CYCLES'
assert (scene.render.resolution_x,scene.render.resolution_y)==(256,256)
assert scene.cycles.samples==16 and not scene.cycles.use_denoising
style=bpy.context.view_layer.freestyle_settings.linesets[0].linestyle
assert tuple(style.color)==(1.,0.,0.) and style.thickness==3
assert bpy.data.objects['Cube'].type=='MESH'
result=dict(blenderVersion=bpy.app.version_string,freestyle=True,strokeColor=list(style.color),
            thickness=style.thickness,blendSha256=report['blendSha256'])
(base/'desktop-verification.json').write_text(json.dumps(result,indent=2)+'\n')
print('FREESTYLE_DESKTOP_VERIFIED:'+json.dumps(result))
