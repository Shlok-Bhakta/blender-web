# SPDX-License-Identifier: GPL-2.0-or-later
"""Small deterministic CPU render, shared by native and Wasm acceptance."""
import bpy
import json
import os
import time
import traceback

try:
    scene=bpy.context.scene
    scene.render.engine='CYCLES'
    scene.cycles.device='CPU'
    scene.cycles.samples=16
    scene.cycles.use_denoising=False
    scene.cycles.use_adaptive_sampling=False
    scene.cycles.seed=0
    scene.render.resolution_x=128
    scene.render.resolution_y=128
    scene.render.resolution_percentage=100
    scene.render.image_settings.file_format='PNG'
    scene.render.filepath='/tmp/web-cycles.png'
    started=time.perf_counter()
    assert bpy.ops.render.render(write_still=True)=={'FINISHED'}
    elapsed=time.perf_counter()-started
    result={'engine':scene.render.engine,'device':scene.cycles.device,'samples':scene.cycles.samples,
            'size':[128,128],'seconds':elapsed,'denoising':False,'seed':0}
    os.write(1,('CYCLES_COMPLETE:'+json.dumps(result)+'\n').encode())
except BaseException:
    os.write(1,('CYCLES_FAILED:'+traceback.format_exc()+'\n').encode())
    raise
finally:
    os.write(1,b'CYCLES_DONE\n')
