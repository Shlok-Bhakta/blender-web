# SPDX-License-Identifier: GPL-2.0-or-later
"""Native/browser scene covering Cycles OpenPGL, OIDN, and compositor denoising."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time

import bpy
import _cycles
from mathutils import Vector

SCENE='Path guiding and denoising'
CASES=('noisy','denoised','guided','volume-guided','compositor')


def material(name,color):
    value=bpy.data.materials.new(name);value.diffuse_color=(*color,1);value.use_nodes=True
    shader=value.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value=(*color,1)
    shader.inputs['Roughness'].default_value=.7
    return value


def author_scene(directory):
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene;scene.name=SCENE
    scene.render.engine='CYCLES';scene.cycles.device='CPU'
    scene.cycles.samples=16;scene.cycles.seed=17
    scene.cycles.use_adaptive_sampling=False;scene.cycles.use_denoising=False
    scene.cycles.use_deterministic_guiding=True;scene.cycles.guiding_training_samples=4
    scene.render.threads_mode='FIXED';scene.render.threads=8
    scene.render.resolution_x,scene.render.resolution_y=192,144
    scene.render.resolution_percentage=100;scene.render.use_persistent_data=False
    scene.render.image_settings.file_format='PNG'
    scene.view_settings.view_transform='Standard';scene.view_settings.look='None'
    scene.world.color=(.005,.005,.005)
    cream=material('Cream',(.65,.62,.55));red=material('Red',(.55,.045,.025));green=material('Green',(.04,.35,.08))
    for name,location,scale,mat in [
        ('Floor',(0,0,-.1),(2.2,2.2,.1),cream),('Back',(0,2.1,2),(2.2,.1,2),cream),
        ('Left',(-2.1,0,2),(.1,2.2,2),red),('Right',(2.1,0,2),(.1,2.2,2),green),
        ('Ceiling',(0,0,4.1),(2.2,2.2,.1),cream),('Cube',(-.8,.5,.65),(.6,.6,.65),cream)]:
        bpy.ops.mesh.primitive_cube_add(size=2,location=location)
        obj=bpy.context.object;obj.name=name;obj.scale=scale;obj.data.materials.append(mat)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32,ring_count=16,radius=.7,location=(.75,.2,.7))
    bpy.context.object.name='Blue sphere';bpy.context.object.data.materials.append(material('Blue',(.08,.2,.6)))
    bpy.ops.mesh.primitive_cube_add(size=2,location=(0,0,2))
    fog=bpy.context.object;fog.name='Guiding volume';fog.scale=(1.95,1.95,1.95);fog.hide_render=True
    volume=bpy.data.materials.new('Fog');volume.use_nodes=True;nodes=volume.node_tree.nodes;nodes.clear()
    output=nodes.new('ShaderNodeOutputMaterial');scatter=nodes.new('ShaderNodeVolumePrincipled')
    scatter.inputs['Density'].default_value=.08;scatter.inputs['Anisotropy'].default_value=.3
    volume.node_tree.links.new(scatter.outputs['Volume'],output.inputs['Volume']);fog.data.materials.append(volume)
    light=bpy.data.lights.new('Ceiling light','AREA');light.energy=80;light.shape='DISK';light.size=1.4
    obj=bpy.data.objects.new('Ceiling light',light);scene.collection.objects.link(obj);obj.location=(0,.3,3.9)
    camera=bpy.data.cameras.new('Camera');obj=bpy.data.objects.new('Camera',camera);scene.collection.objects.link(obj)
    obj.location=(0,-7.8,2);obj.rotation_euler=(Vector((0,.4,1.9))-obj.location).to_track_quat('-Z','Y').to_euler()
    camera.lens=44;scene.camera=obj
    for area in bpy.context.screen.areas if bpy.context.screen else []:
        if area.type=='VIEW_3D':area.spaces.active.region_3d.view_perspective='CAMERA'
    # Float EXR input keeps more than eight bits before the HEVC conversion.
    image=bpy.data.images.new('HEVC gradient',width=96,height=64,float_buffer=True)
    pixels=[]
    for y in range(64):
        for x in range(96):pixels.extend((.01+.6*x/95,.02+.5*y/63,.03+.4*(x+y)/158,1))
    image.pixels.foreach_set(pixels)
    image.filepath_raw=str(directory/'gradient.exr');image.file_format='OPEN_EXR';image.save()
    bpy.data.images.remove(image)
    bpy.ops.wm.save_as_mainfile(filepath=str(directory/'scene.blend'))
    return scene


def load_scene(directory):
    with bpy.data.libraries.load(str(directory/'scene.blend'),link=False) as (source,destination):
        destination.scenes=[SCENE]
    scene=destination.scenes[0];bpy.context.window.scene=scene
    return scene


def configure_case(case):
    scene=bpy.context.scene
    scene.cycles.use_guiding=case in ('guided','volume-guided')
    scene.cycles.samples=64 if scene.cycles.use_guiding else 16
    scene.cycles.use_denoising=case in ('denoised','volume-guided')
    scene.cycles.denoiser='OPENIMAGEDENOISE';scene.cycles.denoising_use_gpu=False
    scene.cycles.denoising_prefilter='ACCURATE';scene.cycles.denoising_quality='HIGH'
    scene.objects['Guiding volume'].hide_render=case!='volume-guided'
    scene.compositing_node_group=None
    if case=='compositor':
        scene.view_layers[0].cycles.denoising_store_passes=True
        scene.render.compositor_device='CPU';scene.render.compositor_denoise_device='CPU'
        tree=bpy.data.node_groups.new('Denoise validation','CompositorNodeTree')
        scene.compositing_node_group=tree
        tree.interface.new_socket(name='Image',in_out='OUTPUT',socket_type='NodeSocketColor')
        layers=tree.nodes.new('CompositorNodeRLayers');layers.scene=scene
        denoise=tree.nodes.new('CompositorNodeDenoise');output=tree.nodes.new('NodeGroupOutput')
        tree.links.new(layers.outputs['Image'],denoise.inputs['Image'])
        tree.links.new(layers.outputs['Denoising Normal'],denoise.inputs['Normal'])
        tree.links.new(layers.outputs['Denoising Albedo'],denoise.inputs['Albedo'])
        tree.links.new(denoise.outputs['Image'],output.inputs['Image'])
    bpy.context.preferences.experimental.use_cycles_debug=True
    bpy.context.preferences.view.show_developer_ui=True
    scene.cycles.debug_bvh_layout='EMBREE';_cycles.debug_flags_update(scene.as_pointer())
    return scene


def render_case(directory,case):
    scene=configure_case(case);scene.render.filepath=str(directory/(case+'.png'))
    os.write(1,('RENDER_LIBRARY_BEGIN:'+case+'\n').encode())
    started=time.perf_counter();assert bpy.ops.render.render(write_still=True)=={'FINISHED'}
    elapsed=time.perf_counter()-started
    result=dict(seconds=elapsed,guiding=scene.cycles.use_guiding,denoising=scene.cycles.use_denoising,
                samples=scene.cycles.samples,bytes=Path(scene.render.filepath).stat().st_size)
    os.write(1,('RENDER_LIBRARY_END:'+json.dumps(dict(case=case,**result))+'\n').encode())
    return result


def export_hevc(directory):
    original=bpy.context.scene;scene=bpy.data.scenes.new('HEVC bit depths')
    bpy.context.window.scene=scene
    try:
        scene.render.engine='CYCLES';scene.cycles.samples=1;scene.cycles.use_denoising=False
        scene.render.resolution_x,scene.render.resolution_y=96,64;scene.render.resolution_percentage=100
        scene.render.threads_mode='FIXED';scene.render.threads=8
        scene.render.use_sequencer=True;scene.render.use_compositing=False
        scene.view_settings.view_transform='Standard';scene.view_settings.look='None'
        scene.frame_start=1;scene.frame_end=3;scene.render.fps=24
        editor=scene.sequence_editor_create()
        strip=editor.strips.new_image('Float gradient',str(directory/'gradient.exr'),1,1)
        strip.frame_final_duration=3
        output={}
        for depth in (8,10,12):
            scene.render.image_settings.media_type='VIDEO';scene.render.image_settings.file_format='FFMPEG'
            scene.render.ffmpeg.format='MKV';scene.render.ffmpeg.codec='H265'
            scene.render.image_settings.color_depth=str(depth)
            scene.render.ffmpeg.audio_codec='NONE';scene.render.ffmpeg.constant_rate_factor='LOSSLESS'
            scene.render.ffmpeg.ffmpeg_preset='REALTIME'
            scene.render.filepath=str(directory/f'hevc-{depth}.mkv')
            assert bpy.ops.render.render(animation=True)=={'FINISHED'}
            path=Path(scene.render.filepath);assert path.is_file()
            output[str(depth)]={'path':path.name,'bytes':path.stat().st_size,'frames':3}
        return output
    finally:
        bpy.context.window.scene=original;bpy.data.scenes.remove(scene)


def browser_check(directory,render=True):
    directory=Path(directory)
    try:
        scene=load_scene(directory)
        assert _cycles.with_path_guiding and _cycles.with_embree
        report=dict(sourceSha256=hashlib.sha256((directory/'check.py').read_bytes()).hexdigest(),
                    pathGuiding=_cycles.with_path_guiding,embree=_cycles.with_embree,cases={})
        if render:
            for case in CASES:report['cases'][case]=render_case(directory,case)
            report['hevc']=export_hevc(directory)
        configure_case('volume-guided')
        bpy.ops.wm.save_as_mainfile(filepath=str(directory/'render-libraries.blend'))
        (directory/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        os.write(1,('RENDER_LIBRARIES_DONE:'+json.dumps(report)+'\n').encode())
    except BaseException:
        import traceback
        os.write(1,('RENDER_LIBRARIES_FAILED:'+traceback.format_exc()+'\n').encode());raise
    finally:os.write(1,b'RENDER_LIBRARIES_FINISHED\n')


if __name__=='__main__' and '--' in sys.argv:
    directory=Path(sys.argv[sys.argv.index('--')+1]).resolve();directory.mkdir(parents=True,exist_ok=True)
    author_scene(directory)
    report=dict(sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                blenderVersion=bpy.app.version_string,cases={case:render_case(directory,case) for case in CASES},hevc=export_hevc(directory))
    (directory/'reference.json').write_text(json.dumps(report,indent=2)+'\n')
