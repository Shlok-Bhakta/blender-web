# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared Cycles scene for BVH2/Embree correctness and paired timings."""
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys
import time

import bpy
import _cycles
from mathutils import Vector


def material(name, color, metallic=0):
    value=bpy.data.materials.new(name)
    value.diffuse_color=(*color,1)
    value.use_nodes=True
    shader=value.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value=(*color,1)
    shader.inputs['Roughness'].default_value=.3
    shader.inputs['Metallic'].default_value=metallic
    return value


def author_scene(directory):
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene
    scene.name='Embree validation'
    scene.render.engine='CYCLES'
    scene.cycles.device='CPU'
    scene.cycles.samples=16
    scene.cycles.use_denoising=False
    scene.cycles.use_adaptive_sampling=False
    scene.cycles.seed=0
    scene.cycles_curves.shape='THICK'
    scene.render.resolution_x,scene.render.resolution_y=256,192
    scene.render.resolution_percentage=100
    scene.render.threads_mode='FIXED'
    scene.render.threads=8
    scene.render.use_persistent_data=False
    scene.render.use_motion_blur=True
    scene.render.motion_blur_shutter=.5
    scene.render.image_settings.media_type='IMAGE'
    scene.render.image_settings.file_format='PNG'
    scene.view_settings.view_transform='Standard'
    scene.view_settings.look='None'
    scene.world.color=(.12,.12,.12)
    scene.frame_start,scene.frame_end=1,3
    palette=[material('Ocean blue',(.035,.24,.42),.25),material('Orange',(.8,.22,.035),.1),
             material('Jade',(.055,.4,.25),.3),material('Cream',(.7,.65,.5),.1)]
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=3,radius=.38)
    prototype=bpy.context.object
    prototype.name='Instance 00'
    prototype.data.materials.append(palette[0])
    for i in range(64):
        obj=prototype if i==0 else bpy.data.objects.new(f'Instance {i:02}',prototype.data)
        if i:scene.collection.objects.link(obj)
        x,y=i%8,i//8
        obj.location=(x-3.5,y-3.5,.42)
        obj.scale=(.9,.9,1+.4*math.sin(i))
        obj.material_slots[0].link='OBJECT'
        obj.material_slots[0].material=palette[i%4]
        if i%9==0:
            for frame,offset in [(0,-.16),(1,0),(2,.16),(3,.24),(4,-.16)]:
                obj.location.z=.42+offset
                obj.keyframe_insert(data_path='location',frame=frame)
    bpy.ops.mesh.primitive_plane_add(size=200,location=(0,0,-.01))
    bpy.context.object.name='Ground'
    bpy.context.object.data.materials.append(material('Ground',(.09,.115,.15)))
    curves=bpy.data.hair_curves.new('Round curves')
    curves.add_curves([8]*7)
    for strand in range(7):
        for point in range(8):
            item=curves.points[strand*8+point]
            item.position=(-3.2+strand*.24+math.sin(point*.45)*.18,-4.6+point*.08,.1+point*.23)
            item.radius=.035
    curve_obj=bpy.data.objects.new('Round curves',curves)
    scene.collection.objects.link(curve_obj)
    curves.materials.append(palette[1])
    curve_obj.hide_set(True)  # Keep the untested Workbench curve path out of this CPU render test.
    points=bpy.data.meshes.new('Point positions')
    points.from_pydata([(i*.28-1.2,-4.5,.17+(i%3)*.16) for i in range(10)],[],[])
    cloud=bpy.data.objects.new('Sphere points',points)
    scene.collection.objects.link(cloud)
    tree=bpy.data.node_groups.new('Sphere points','GeometryNodeTree')
    tree.interface.new_socket(name='Geometry',in_out='INPUT',socket_type='NodeSocketGeometry')
    tree.interface.new_socket(name='Geometry',in_out='OUTPUT',socket_type='NodeSocketGeometry')
    source=tree.nodes.new('NodeGroupInput');out=tree.nodes.new('NodeGroupOutput')
    convert=tree.nodes.new('GeometryNodeMeshToPoints');convert.mode='VERTICES'
    convert.inputs['Radius'].default_value=.15
    assign=tree.nodes.new('GeometryNodeSetMaterial');assign.inputs['Material'].default_value=palette[2]
    tree.links.new(source.outputs['Geometry'],convert.inputs['Mesh'])
    tree.links.new(convert.outputs['Points'],assign.inputs['Geometry'])
    tree.links.new(assign.outputs['Geometry'],out.inputs['Geometry'])
    cloud.modifiers.new('Sphere points','NODES').node_group=tree
    cloud.hide_set(True)
    bpy.ops.mesh.primitive_plane_add(size=2,location=(2,-1,1.7))
    transparent=bpy.context.object;transparent.name='Transparency filter';transparent.rotation_euler=(0,.3,.15)
    alpha=material('Transparent mix',(.5,.15,.08))
    nodes=alpha.node_tree.nodes;links=alpha.node_tree.links
    diffuse=nodes.get('Principled BSDF');clear=nodes.new('ShaderNodeBsdfTransparent');mix=nodes.new('ShaderNodeMixShader')
    mix.inputs[0].default_value=.65
    links.new(diffuse.outputs[0],mix.inputs[1]);links.new(clear.outputs[0],mix.inputs[2]);links.new(mix.outputs[0],nodes.get('Material Output').inputs['Surface'])
    transparent.data.materials.append(alpha)
    for name,location,power,size in [('Key',(-4,-6,10),1700,7),('Rim',(5,4,7),1200,5)]:
        light=bpy.data.lights.new(name,'AREA');light.energy=power;light.shape='DISK';light.size=size
        obj=bpy.data.objects.new(name,light);scene.collection.objects.link(obj);obj.location=location
        obj.rotation_euler=(Vector((0,0,.5))-obj.location).to_track_quat('-Z','Y').to_euler()
    camera=bpy.data.cameras.new('Camera');obj=bpy.data.objects.new('Camera',camera);scene.collection.objects.link(obj)
    obj.location=(10,-15,11);obj.rotation_euler=(Vector((0,-.3,.4))-obj.location).to_track_quat('-Z','Y').to_euler()
    camera.type='ORTHO';camera.ortho_scale=12;scene.camera=obj
    scene.frame_set(1)
    for area in bpy.context.screen.areas if bpy.context.screen else []:
        if area.type=='VIEW_3D':area.spaces.active.region_3d.view_perspective='CAMERA'
    bpy.ops.wm.save_as_mainfile(filepath=str(directory/'scene.blend'))
    return scene


def load_scene(directory):
    with bpy.data.libraries.load(str(directory/'scene.blend'),link=False) as (source,destination):
        destination.scenes=['Embree validation']
    scene=destination.scenes[0]
    bpy.context.window.scene=scene
    return scene


def select_backend(scene,backend):
    # The original engine resets these flags unless both developer settings are enabled.
    bpy.context.preferences.experimental.use_cycles_debug=True
    bpy.context.preferences.view.show_developer_ui=True
    scene.cycles.debug_bvh_layout=backend
    _cycles.debug_flags_update(scene.as_pointer())


def render_case(directory,backend,frames=(1,3),repeats=1):
    scene=bpy.context.scene
    select_backend(scene,backend)
    outputs={}
    for frame in frames:
        scene.frame_set(frame)
        path=directory/f'{backend.lower()}-{frame}.png'
        scene.render.filepath=str(path)
        runs=[]
        for repeat in range(repeats):
            os.write(1,('EMBREE_RENDER_BEGIN:'+json.dumps(dict(backend=backend,frame=frame,repeat=repeat))+'\n').encode())
            started=time.perf_counter()
            assert bpy.ops.render.render(write_still=True)=={'FINISHED'}
            runs.append(time.perf_counter()-started)
            os.write(1,b'EMBREE_RENDER_END\n')
        outputs[str(frame)]=dict(path=path.name,bytes=path.stat().st_size,
                                seconds=statistics.median(runs),runs=runs)
    return outputs


def inspect_scene():
    scene=bpy.context.scene
    instances=[obj for obj in scene.objects if obj.name.startswith('Instance ')]
    assert len(instances)==64 and len({obj.data.as_pointer() for obj in instances})==1
    animated=sum(bool(obj.animation_data and obj.animation_data.action) for obj in instances)
    assert animated==8
    curves=next(obj for obj in scene.objects if obj.type=='CURVES')
    assert len(curves.data.curves)==7 and len(curves.data.points)==56
    cloud=scene.objects['Sphere points']
    assert len(cloud.data.vertices)==10 and cloud.modifiers[0].node_group is not None
    return dict(instances=64,sharedMeshes=1,animatedObjects=animated,sourceTriangles=len(instances[0].data.polygons),
                curvePoints=56,spherePoints=10,transparency=True,motionBlur=scene.render.use_motion_blur,
                withEmbree=_cycles.with_embree,threads=scene.render.threads)


def browser_check(directory,render=True,benchmark=False):
    directory=Path(directory)
    try:
        scene=load_scene(directory)
        result=dict(scene=inspect_scene(),sourceSha256=hashlib.sha256((directory/'check.py').read_bytes()).hexdigest())
        assert result['scene']['withEmbree']
        if render:
            result['images']={backend:render_case(directory,backend) for backend in ['BVH2','EMBREE']}
            if benchmark:
                result['benchmark']={backend:render_case(directory,backend,frames=(2,),repeats=3) for backend in ['BVH2','EMBREE']}
        select_backend(scene,'EMBREE');scene.frame_set(1)
        bpy.ops.wm.save_as_mainfile(filepath=str(directory/'embree.blend'))
        (directory/'results.json').write_text(json.dumps(result,indent=2)+'\n')
        os.write(1,('EMBREE_DONE:'+json.dumps(result)+'\n').encode())
    except BaseException:
        import traceback
        os.write(1,('EMBREE_FAILED:'+traceback.format_exc()+'\n').encode())
        raise
    finally:os.write(1,b'EMBREE_FINISHED\n')


if __name__=='__main__' and '--' in sys.argv:
    directory=Path(sys.argv[sys.argv.index('--')+1]).resolve();directory.mkdir(parents=True,exist_ok=True)
    scene=author_scene(directory)
    result=dict(scene=inspect_scene(),images={backend:render_case(directory,backend) for backend in ['BVH2','EMBREE']},
                benchmark={backend:render_case(directory,backend,frames=(2,),repeats=3) for backend in ['BVH2','EMBREE']},
                sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (directory/'reference.json').write_text(json.dumps(result,indent=2)+'\n')
