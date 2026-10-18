# SPDX-License-Identifier: GPL-2.0-or-later
import bpy
import numpy as np
from mathutils import Quaternion


def reset():
    if bpy.context.object and bpy.context.object.mode!='OBJECT': bpy.ops.object.mode_set(mode='OBJECT')
    for obj in list(bpy.context.scene.objects):
        bpy.data.objects.remove(obj,do_unlink=True)


def view(distance=7):
    for area in bpy.context.screen.areas:
        for space in area.spaces:
            if space.type!='VIEW_3D': continue
            space.region_3d.view_distance=distance
            space.region_3d.view_location=(0,0,0)
            space.region_3d.view_rotation=Quaternion((0.880476,0.279849,0.115917,0.364705))
            space.overlay.show_floor=False
            space.overlay.show_axis_x=False
            space.overlay.show_axis_y=False
            space.overlay.show_stats=True


def boolean(solver):
    reset()
    bpy.ops.mesh.primitive_cube_add()
    cube=bpy.context.object
    cube.name=solver+' Boolean'
    bpy.ops.mesh.primitive_cylinder_add(vertices=48,radius=.62,depth=4,location=(.28,.1,0))
    cutter=bpy.context.object
    cutter.name='Hidden cylinder cutter'
    modifier=cube.modifiers.new(solver+' difference','BOOLEAN')
    modifier.operation='DIFFERENCE'
    modifier.solver=solver
    modifier.object=cutter
    cutter.hide_set(True)
    cutter.select_set(False)
    cube.select_set(True)
    bpy.context.view_layer.objects.active=cube
    for area in bpy.context.screen.areas:
        if area.type=='PROPERTIES': area.spaces.active.context='MODIFIER'
    view()


def remesh():
    reset()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=12,ring_count=8)
    obj=bpy.context.object
    obj.name='Native QuadriFlow'
    assert bpy.ops.object.quadriflow_remesh(target_faces=100,seed=0,use_preserve_sharp=False)=={'FINISHED'}
    obj.show_wire=True
    obj.show_all_edges=True
    view(6)


def wave():
    reset()
    n=40
    x,y=np.meshgrid(np.linspace(-2,2,n),np.linspace(-2,2,n))
    z=.55*np.sin(x*2.5)*np.cos(y*2.5)
    coordinates=np.column_stack([x.ravel(),y.ravel(),z.ravel()]).astype(np.float32)
    faces=[(j*n+i,j*n+i+1,(j+1)*n+i+1,(j+1)*n+i) for j in range(n-1) for i in range(n-1)]
    mesh=bpy.data.meshes.new('NumPy wave mesh')
    mesh.from_pydata(coordinates.tolist(),[],faces)
    mesh.update()
    obj=bpy.data.objects.new('NumPy '+np.__version__+' wave',mesh)
    bpy.context.collection.objects.link(obj)
    bpy.context.view_layer.objects.active=obj
    obj.select_set(True)
    for polygon in mesh.polygons: polygon.use_smooth=True
    view(9)


def save():
    assert bpy.ops.wm.save_as_mainfile(filepath='/user/blender-web/home/web-walkthrough.blend')=={'FINISHED'}
