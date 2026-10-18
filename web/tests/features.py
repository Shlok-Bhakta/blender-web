# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise real importers, both IK solvers, Bullet and native blend files."""
import bpy
import json
import math
import os
import traceback
from pathlib import Path
from mathutils import Vector


def emit(kind, value):
    os.write(1, (kind + ':' + json.dumps(value) + '\n').encode())


def clear():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)


def mesh_signature(obj):
    points = [obj.matrix_world @ v.co for v in obj.data.vertices]
    obj.data.calc_loop_triangles()
    area = volume = 0.0
    for triangle in obj.data.loop_triangles:
        a, b, c = (points[i] for i in triangle.vertices)
        area += (b-a).cross(c-a).length / 2
        volume += a.dot(b.cross(c)) / 6
    return dict(vertices=len(points), polygons=len(obj.data.polygons),
                uniquePositions=len({tuple(round(v, 6) for v in p) for p in points}),
                surfaceArea=area, volume=abs(volume),
                bounds=[[min(p[i] for p in points), max(p[i] for p in points)] for i in range(3)])


def run():
    result = {'buildOptions': {key: getattr(bpy.app.build_options, key) for key in
                              ('bullet', 'io_wavefront_obj', 'io_stl', 'io_ply', 'io_fbx')},
              'importExport': {}, 'ik': {}}
    assert all(result['buildOptions'].values()), result['buildOptions']
    directory = Path('/tmp/web-features')
    directory.mkdir(exist_ok=True)
    for extension, exporter, importer in (
            ('obj', bpy.ops.wm.obj_export, bpy.ops.wm.obj_import),
            ('stl', bpy.ops.wm.stl_export, bpy.ops.wm.stl_import),
            ('ply', bpy.ops.wm.ply_export, bpy.ops.wm.ply_import)):
        clear()
        bpy.ops.mesh.primitive_cube_add()
        filepath = directory / ('cube.' + extension)
        assert exporter(filepath=str(filepath)) == {'FINISHED'}
        size = filepath.stat().st_size
        assert size > 100
        clear()
        assert importer(filepath=str(filepath)) == {'FINISHED'}
        meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
        assert len(meshes) == 1
        signature = mesh_signature(meshes[0])
        # PLY's default UV export splits vertices at seams. Preserve that output
        # and validate the geometric surface instead of assuming welded topology.
        assert signature['uniquePositions'] == 8, signature
        assert signature['polygons'] >= 6, signature
        assert abs(signature['surfaceArea'] - 24) < 1e-4, signature
        assert abs(signature['volume'] - 8) < 1e-4, signature
        assert all(abs(lo + 1) < 1e-4 and abs(hi - 1) < 1e-4
                   for lo, hi in signature['bounds']), signature
        result['importExport'][extension] = dict(bytes=size, **signature)
        emit('FEATURE_PROGRESS', extension)

    # Independently authored FBX input, rather than a round trip through an exporter.
    clear()
    fbx = directory / 'triangle.fbx'
    fbx.write_text('''; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: {
 Version: 1000
 Properties70: {
  P: "UpAxis", "int", "Integer", "",2
  P: "UpAxisSign", "int", "Integer", "",1
  P: "FrontAxis", "int", "Integer", "",1
  P: "FrontAxisSign", "int", "Integer", "",-1
  P: "CoordAxis", "int", "Integer", "",0
  P: "CoordAxisSign", "int", "Integer", "",1
  P: "UnitScaleFactor", "double", "Number", "",100
 }
}
Objects: {
 Geometry: 1001, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
 }
 Model: 1002, "Model::Triangle", "Mesh" { Version: 232 }
}
Connections: {
 C: "OO",1001,1002
 C: "OO",1002,0
}
''')
    assert bpy.ops.wm.fbx_import(filepath=str(fbx)) == {'FINISHED'}
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    assert len(meshes) == 1
    signature = mesh_signature(meshes[0])
    assert signature['vertices'] == 3 and signature['polygons'] == 1, signature
    lengths = sorted((meshes[0].data.vertices[e.vertices[0]].co -
                      meshes[0].data.vertices[e.vertices[1]].co).length for e in meshes[0].data.edges)
    assert all(abs(a-b) < 1e-4 for a, b in zip(lengths, [1, 1, math.sqrt(2)])), lengths
    result['importExport']['fbx'] = dict(bytes=fbx.stat().st_size, edgeLengths=lengths, **signature)
    emit('FEATURE_PROGRESS', 'fbx')

    for solver in ('LEGACY', 'ITASC'):
        clear()
        bpy.ops.object.armature_add()
        rig = bpy.context.object
        rig.name = 'WebIK'
        bpy.ops.object.mode_set(mode='EDIT')
        root = rig.data.edit_bones[0]
        root.name = 'Root'
        root.head, root.tail = (0, 0, 0), (0, 1, 0)
        tip = rig.data.edit_bones.new('Tip')
        tip.head, tip.tail = (0, 1, 0), (0, 2, 0)
        tip.parent, tip.use_connect = root, True
        bpy.ops.object.mode_set(mode='OBJECT')
        bpy.ops.object.empty_add()
        target = bpy.context.object
        rig.pose.ik_solver = solver
        ik = rig.pose.bones['Tip'].constraints.new('IK')
        ik.target, ik.chain_count = target, 2
        measurements = []
        for position in ((1, 1, 0), (-0.75, 1.25, 0)):
            target.location = position
            bpy.context.view_layer.update()
            evaluated = rig.evaluated_get(bpy.context.evaluated_depsgraph_get())
            endpoint = evaluated.matrix_world @ evaluated.pose.bones['Tip'].tail
            error = (endpoint - Vector(position)).length
            assert error < 0.03, (solver, position, tuple(endpoint), error)
            measurements.append(dict(target=position, endpoint=list(endpoint), error=error))
        result['ik'][solver] = measurements
        emit('FEATURE_PROGRESS', solver)

    clear()
    scene = bpy.context.scene
    scene.frame_set(1)
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, -0.5))
    floor = bpy.context.object
    floor.name = 'WebFloor'
    floor.scale = (10, 10, 1)
    bpy.ops.rigidbody.object_add()
    floor.rigid_body.type = 'PASSIVE'
    bpy.ops.mesh.primitive_cube_add(size=1, location=(0, 0, 4))
    falling = bpy.context.object
    falling.name = 'WebFallingCube'
    bpy.ops.rigidbody.object_add()
    falling.rigid_body.collision_shape = 'BOX'
    falling.rigid_body.use_margin = False
    floor.rigid_body.collision_shape = 'BOX'
    floor.rigid_body.use_margin = False
    scene.rigidbody_world.point_cache.frame_end = 100
    heights = {}
    for frame in range(1, 101):
        scene.frame_set(frame)
        if frame in (1, 10, 50, 100):
            heights[frame] = falling.evaluated_get(bpy.context.evaluated_depsgraph_get()).matrix_world.translation.z
    assert abs(heights[1] - 4) < 1e-4, heights
    assert 0.5 < heights[10] < heights[1] - 0.1, heights
    assert abs(heights[100] - 0.5) < 0.08, heights
    result['rigidBody'] = {'heights': heights, 'frames': 100, 'collision': True}
    emit('FEATURE_PROGRESS', 'bullet')

    scene.frame_set(1)
    blend = Path('/user/blender-web/home/web-feature-scene.blend')
    emit('FEATURE_PROGRESS', 'save-begin')
    preview = bpy.context.preferences.filepaths.file_preview_type
    assert bpy.ops.wm.save_as_mainfile(filepath=str(blend)) == {'FINISHED'}
    assert bpy.context.preferences.filepaths.file_preview_type == preview
    # Exercise an overwrite, its native backup, and an explicit preview setting.
    bpy.context.preferences.filepaths.file_preview_type = 'CAMERA'
    assert bpy.ops.wm.save_as_mainfile(filepath=str(blend)) == {'FINISHED'}
    assert bpy.context.preferences.filepaths.file_preview_type == 'CAMERA'
    bpy.context.preferences.filepaths.file_preview_type = preview
    backup_size = Path(str(blend) + '1').stat().st_size
    assert backup_size > 1000
    result['blendFile'] = dict(path=str(blend), bytes=blend.stat().st_size,
                              saves=2, backupBytes=backup_size, previewPreferencePreserved=True)
    # Read OPFS on Blender's pthread; the browser main thread only exports memory FS.
    (directory / 'feature-scene.blend').write_bytes(blend.read_bytes())
    result['blendFile']['expectedObjects'] = sorted(o.name for o in scene.objects)
    Path('/tmp/web-features/acceptance.json').write_text(json.dumps(result, indent=2))
    emit('FEATURE_COMPLETE', result)


try:
    run()
except BaseException:
    emit('FEATURE_FAILED', traceback.format_exc())
    raise
finally:
    emit('FEATURE_DONE', True)
