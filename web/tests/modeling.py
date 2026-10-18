# SPDX-License-Identifier: GPL-2.0-or-later
"""Native geometry and NumPy checks, run inside Blender's embedded interpreter."""
import bpy
import bmesh
import json
import math
import os
import traceback
from pathlib import Path


def clear():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)


def signature(obj):
    mesh = bmesh.new()
    mesh.from_mesh(obj.data)
    result = dict(vertices=len(mesh.verts), faces=len(mesh.faces), volume=abs(mesh.calc_volume()),
                  manifold=all(edge.is_manifold for edge in mesh.edges),
                  finite=all(math.isfinite(component) for vertex in mesh.verts for component in vertex.co),
                  bounds=[[min(vertex.co[i] for vertex in mesh.verts),max(vertex.co[i] for vertex in mesh.verts)] for i in range(3)])
    mesh.free()
    return result


def run():
    import numpy as np
    report = dict(numpy={'version':np.__version__}, booleans=[], remesh=[], uv=[])
    # Check real compiled ufuncs, linear algebra, FFT and random extensions.
    a = np.array([[3.,1.],[1.,2.]])
    x = np.linalg.solve(a, np.array([9.,8.]))
    np.testing.assert_allclose(x,[2.,3.])
    np.testing.assert_allclose(np.linalg.svd(a)[1],[3.618033988749895,1.381966011250105])
    spectrum = np.fft.rfft([0.,1.,0.,-1.])
    np.testing.assert_allclose(spectrum,[0.,-2j,0.],atol=1e-12)
    assert np.random.default_rng(42).integers(0,100,size=5).tolist()==[8,77,65,43,43]
    np.testing.assert_allclose(np.sin(np.array([0.,np.pi/2,np.pi])),[0.,1.,0.],atol=1e-12)
    fortran = np.asfortranarray(np.arange(12).reshape(3,4))
    assert fortran.flags.f_contiguous and fortran.sum()==66
    # Exact seeded outputs from the pinned desktop release also check the
    # separately compiled 32-bit legacy and 64-bit Generator distributions.
    assert np.random.default_rng(42).multinomial(10,[.2,.3,.5],size=4).tolist()==[[3,2,5],[3,3,4],[0,7,3],[3,4,3]]
    assert np.random.RandomState(42).multinomial(10,[.2,.3,.5],size=4).tolist()==[[1,6,3],[3,3,4],[1,2,7],[0,5,5]]
    report['numpy'].update(linalg=True,fft=True,random=True,legacyRandomMatchesDesktop=True,modernRandomMatchesDesktop=True,ufuncs=True,stridedArrays=True)
    for solver in ('EXACT','MANIFOLD'):
        for operation,volume in [('UNION',12.),('INTERSECT',4.),('DIFFERENCE',4.)]:
            clear()
            bpy.ops.mesh.primitive_cube_add()
            obj=bpy.context.object
            bpy.ops.mesh.primitive_cube_add(location=(1,0,0))
            cutter=bpy.context.object
            bpy.context.view_layer.objects.active=obj
            modifier=obj.modifiers.new('WebBoolean','BOOLEAN')
            modifier.solver=solver
            modifier.operation=operation
            modifier.object=cutter
            assert bpy.ops.object.modifier_apply(modifier=modifier.name)=={'FINISHED'}
            result=signature(obj)
            assert result['finite'] and result['manifold'], result
            assert abs(result['volume']-volume)<1e-5,(solver,operation,result)
            report['booleans'].append(dict(solver=solver,operation=operation,**result))
    clear()
    bpy.ops.mesh.primitive_cube_add()
    obj=bpy.context.object
    obj.name='WebNumpyFBX'
    assert bpy.ops.preferences.addon_enable(module='io_scene_fbx')=={'FINISHED'}
    export=Path('/tmp/web-numpy-roundtrip.fbx')
    assert bpy.ops.export_scene.fbx(filepath=str(export),use_selection=True,bake_anim=False)=={'FINISHED'}
    assert export.read_bytes().startswith(b'Kaydara FBX Binary')
    clear()
    assert bpy.ops.wm.fbx_import(filepath=str(export))=={'FINISHED'}
    obj=next(obj for obj in bpy.context.scene.objects if obj.type=='MESH')
    assert not obj.data.validate(verbose=False), 'Imported FBX mesh needed additional repairs'
    fbx=signature(obj)
    assert fbx['manifold'] and abs(fbx['volume']-8)<1e-4,fbx
    report['fbxExportImport']=dict(bytes=export.stat().st_size,**fbx)
    clear()
    assert bpy.ops.import_scene.fbx(filepath=str(export),use_anim=False)=={'FINISHED'}
    obj=next(obj for obj in bpy.context.scene.objects if obj.type=='MESH')
    legacy=signature(obj)
    assert legacy['manifold'] and abs(legacy['volume']-8)<1e-4,legacy
    report['fbxLegacyImport']=legacy
    # Octree remeshing exercises the newly enabled dual-contouring library.
    for mode in ('SMOOTH','SHARP'):
        clear()
        bpy.ops.mesh.primitive_cube_add()
        obj=bpy.context.object
        modifier=obj.modifiers.new('WebRemesh','REMESH')
        modifier.mode=mode
        modifier.octree_depth=4
        assert bpy.ops.object.modifier_apply(modifier=modifier.name)=={'FINISHED'}
        result=signature(obj)
        assert result['finite'] and result['manifold'] and result['vertices']>8,result
        assert 4 < result['volume'] < 10,result
        report['remesh'].append(dict(mode=mode,**result))
    # Unwrap an open plane with a known nonzero surface area using native SLIM.
    clear()
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=5,y_subdivisions=5,size=2)
    obj=bpy.context.object
    for vertex in obj.data.vertices: vertex.co.z=.2*math.sin(vertex.co.x*2)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    assert bpy.ops.uv.unwrap(method='MINIMUM_STRETCH',iterations=10)=={'FINISHED'}
    bpy.ops.object.mode_set(mode='OBJECT')
    uv=[tuple(loop.uv) for loop in obj.data.uv_layers.active.data]
    assert all(math.isfinite(value) for point in uv for value in point)
    assert max(p[0] for p in uv)-min(p[0] for p in uv)>.1
    assert max(p[1] for p in uv)-min(p[1] for p in uv)>.1
    report['uv'].append(dict(method='MINIMUM_STRETCH',loops=len(uv),finite=True))
    # QuadriFlow runs the native bundled solver on a closed triangulated sphere.
    clear()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=12,ring_count=8)
    obj=bpy.context.object
    assert bpy.ops.object.quadriflow_remesh(target_faces=100,seed=0,use_preserve_sharp=False)=={'FINISHED'}
    result=signature(obj)
    assert result['finite'] and result['manifold'] and result['faces']>20,result
    assert all(len(polygon.vertices)==4 for polygon in obj.data.polygons)
    report['quadriflow']=result
    # Use NumPy buffers with Blender's real bulk RNA accessors.
    coordinates=np.empty(len(obj.data.vertices)*3,dtype=np.float32)
    obj.data.vertices.foreach_get('co',coordinates)
    assert np.isfinite(coordinates).all()
    obj.data.vertices.foreach_set('co',coordinates)
    report['numpy']['rnaBuffers']=True
    Path('/tmp/modeling-acceptance.json').write_text(json.dumps(report,indent=2))
    os.write(1,('MODELING_COMPLETE:'+json.dumps(report)+'\n').encode())

try:
    run()
except BaseException:
    os.write(1,('MODELING_FAILED:'+traceback.format_exc()+'\n').encode())
    raise
finally:
    os.write(1,b'MODELING_DONE\n')
