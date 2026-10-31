# SPDX-License-Identifier: GPL-2.0-or-later
"""Report scene and viewport state for the touch test without further keyboard input."""
import json
import os

import bpy
from bpy_extras.view3d_utils import location_3d_to_region_2d


def screen_point(window, area, obj):
    region = next(r for r in area.regions if r.type == 'WINDOW')
    rv3d = area.spaces.active.region_3d
    point = location_3d_to_region_2d(region, rv3d, obj.matrix_world.translation)
    if point is None:
        return None
    # Window pixels with y growing downwards; the test divides by devicePixelRatio.
    return [region.x + point.x, window.height - (region.y + point.y)]


def report():
    window = bpy.context.window_manager.windows[0]
    state = {
        'selected': sorted(o.name for o in bpy.context.view_layer.objects if o.select_get()),
        'active': getattr(bpy.context.view_layer.objects.active, 'name', None),
        'names': sorted(o.name for o in bpy.data.objects),
    }
    area = next((a for a in window.screen.areas if a.type == 'VIEW_3D'), None)
    if area is not None:
        rv3d = area.spaces.active.region_3d
        state['rotation'] = list(rv3d.view_rotation)
        state['distance'] = rv3d.view_distance
        for name in ('Cube', 'Light', 'Camera'):
            obj = bpy.data.objects.get(name)
            if obj is not None:
                state[name] = screen_point(window, area, obj)
    os.write(1, ('TOUCH_STATE:' + json.dumps(state) + '\n').encode())
    return 0.25


for obj in bpy.context.view_layer.objects:
    obj.select_set(False)
bpy.app.timers.register(report, first_interval=0.1, persistent=True)
