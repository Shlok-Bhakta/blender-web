# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared desktop/browser checks for native oceans and Ogawa scene caches."""
import bpy
import json
import math
import os
import traceback
from pathlib import Path

FRAMES = (1, 6, 12)


def clear():
    for obj in list(bpy.context.scene.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.context.scene.frame_set(1)


def ocean():
    clear()
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.object
    obj.name = 'NativeOcean'
    mod = obj.modifiers.new('FFTW ocean', 'OCEAN')
    mod.viewport_resolution = mod.resolution = 5
    mod.spatial_size = 20
    mod.random_seed = 7
    mod.wave_scale = 1.3
    mod.choppiness = 1.0
    mod.use_normals = True
    mod.use_foam = True
    mod.foam_layer_name = 'OceanFoam'
    for frame, time in ((1, 0.0), (12, 2.0)):
        mod.time = time
        mod.keyframe_insert('time', frame=frame)
    return obj


def sample(obj):
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        coords = [list(evaluated.matrix_world @ v.co) for v in mesh.vertices]
        assert coords and all(math.isfinite(c) for v in coords for c in v)
        return dict(vertices=coords, faces=len(mesh.polygons),
                    uvLayers=len(mesh.uv_layers),
                    uv=[list(item.uv) for item in mesh.uv_layers.active.data] if mesh.uv_layers else [],
                    foam='OceanFoam' in mesh.color_attributes,
                    bounds=[[min(v[i] for v in coords), max(v[i] for v in coords)] for i in range(3)])
    finally:
        evaluated.to_mesh_clear()


def samples(obj):
    result = {}
    for frame in FRAMES:
        bpy.context.scene.frame_set(frame)
        result[str(frame)] = sample(obj)
    return result


def compare(actual, expected, tolerance=2e-4):
    maximum = 0.0
    for frame in FRAMES:
        a, b = actual[str(frame)], expected[str(frame)]
        assert len(a['vertices']) == len(b['vertices']), (frame, len(a['vertices']), len(b['vertices']))
        assert a['faces'] == b['faces'] and a['uvLayers'] == b['uvLayers'], (a['faces'], b['faces'])
        for va, vb in zip(a['vertices'], b['vertices']):
            maximum = max(maximum, *(abs(x-y) for x, y in zip(va, vb)))
        assert len(a['uv']) == len(b['uv'])
        assert all(abs(x-y) < tolerance for va, vb in zip(a['uv'], b['uv']) for x, y in zip(va, vb))
    assert maximum < tolerance, maximum
    return maximum


def export(path):
    assert bpy.ops.wm.alembic_export(filepath=str(path), start=1, end=12,
        selected=True, uvs=True, normals=True, vcolors=True,
        as_background_job=False, evaluation_mode='VIEWPORT') == {'FINISHED'}
    assert path.read_bytes().startswith(b'Ogawa'), path


def import_cache(path, name, deforming=True):
    clear()
    assert bpy.ops.wm.alembic_import(filepath=str(path), as_background_job=False,
        set_frame_range=False) == {'FINISHED'}
    obj = bpy.data.objects[name]
    if deforming:
        assert any(m.type == 'MESH_SEQUENCE_CACHE' for m in obj.modifiers)
    return obj


def desktop_fixture(directory):
    directory.mkdir(parents=True, exist_ok=True)
    obj = ocean()
    reference = samples(obj)
    assert len(reference['1']['vertices']) == 676
    assert reference['1']['foam']
    export(directory / 'desktop-ocean.abc')
    obj = import_cache(directory / 'desktop-ocean.abc', 'NativeOcean')
    error = compare(samples(obj), reference)
    # A separate authored hierarchy checks transforms and non-ocean geometry.
    clear()
    parent = bpy.data.objects.new('AnimatedParent', None)
    bpy.context.collection.objects.link(parent)
    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    cube.name = 'AnimatedCube'
    cube.parent = parent
    cube.location = (1, 2, 0.5)
    for frame, location, angle in ((1, (0, 0, 0), 0), (12, (4, -1, 2), 1.2)):
        parent.location = location
        parent.rotation_euler.z = angle
        parent.keyframe_insert('location', frame=frame)
        parent.keyframe_insert('rotation_euler', frame=frame)
    cube_samples = samples(cube)
    parent.select_set(True)
    export(directory / 'desktop-hierarchy.abc')
    cube = import_cache(directory / 'desktop-hierarchy.abc', 'AnimatedCube', deforming=False)
    hierarchy_error = compare(samples(cube), cube_samples)
    report = dict(blenderVersion=bpy.app.version_string, ocean=reference,
                  hierarchy=cube_samples, desktopRoundtripMaxError=error,
                  hierarchyRoundtripMaxError=hierarchy_error)
    (directory / 'reference.json').write_text(json.dumps(report) + '\n')
    print('OCEAN_DESKTOP_FIXTURE:' + json.dumps(dict(oceanError=error, hierarchyError=hierarchy_error)))


def run():
    directory = Path('/tmp/ocean-alembic')
    reference = json.loads((directory / 'reference.json').read_text())
    obj = ocean()
    generated = samples(obj)
    error = compare(generated, reference['ocean'])
    assert all(frame['foam'] for frame in generated.values())
    assert any(abs(a[2]-b[2]) > .01 for a, b in zip(generated['1']['vertices'], generated['12']['vertices']))
    target = Path('/user/blender-web/home/browser-ocean.abc')
    export(target)
    target.with_suffix('.reference.json').write_text(json.dumps(generated))
    obj = import_cache(target, 'NativeOcean')
    roundtrip = compare(samples(obj), generated)
    obj = import_cache(directory / 'desktop-ocean.abc', 'NativeOcean')
    desktop_import = compare(samples(obj), reference['ocean'])
    cube = import_cache(directory / 'desktop-hierarchy.abc', 'AnimatedCube', deforming=False)
    hierarchy_import = compare(samples(cube), reference['hierarchy'])
    assert cube.parent and cube.parent.name == 'AnimatedParent'
    obj = import_cache(target, 'NativeOcean')
    bpy.context.scene.frame_set(6)
    samples_path = directory / 'browser-samples.json'
    samples_path.write_text(json.dumps(generated))
    return dict(ocean=dict(resolution=5, vertices=len(generated['1']['vertices']),
                    faces=generated['1']['faces'], frames=list(FRAMES), foam=True,
                    desktopMaxCoordinateError=error),
                alembic=dict(format='Ogawa', bytes=target.stat().st_size,
                    path=str(target), roundtripMaxError=roundtrip,
                    desktopImportMaxError=desktop_import,
                    animatedHierarchyMaxError=hierarchy_import), samplesPath=str(samples_path))


if __name__ == '__main__':
    try:
        result = run()
        os.write(1, ('OCEAN_COMPLETE:' + json.dumps(result) + '\n').encode())
    except Exception:
        os.write(1, ('OCEAN_FAILED:' + traceback.format_exc() + '\n').encode())
    finally:
        os.write(1, b'OCEAN_DONE\n')
