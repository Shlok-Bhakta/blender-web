# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared desktop/browser OpenVDB evaluations, using the original modifiers."""
import bpy
import json
import math
import os
import tempfile
import traceback
from pathlib import Path


def clear():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)


def snapshot(obj):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    assert mesh and len(mesh.vertices) > 100
    positions = sorted([list(vertex.co) for vertex in mesh.vertices],
                       key=lambda point: tuple(round(value, 5) for value in point))
    result = dict(vertices=len(mesh.vertices), faces=len(mesh.polygons),
                  positions=positions,
                  bounds=[[min(point[axis] for point in positions),
                           max(point[axis] for point in positions)] for axis in range(3)])
    assert all(math.isfinite(value) for point in positions for value in point)
    evaluated.to_mesh_clear()
    return result


def mesh_from_volume(volume, name, grid='density', threshold=0.5):
    mesh = bpy.data.meshes.new(name)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new('Native OpenVDB surface', 'VOLUME_TO_MESH')
    modifier.object = volume
    modifier.grid_name = grid
    modifier.threshold = threshold
    modifier.resolution_mode = 'GRID'
    return obj


def fluid(cache_directory):
    assert bpy.app.build_options.fluid
    Path(cache_directory).mkdir(parents=True, exist_ok=True)
    cache_directory = Path(tempfile.mkdtemp(prefix='gas-', dir=cache_directory))
    clear()
    scene = bpy.context.scene
    scene.frame_set(1)
    bpy.ops.mesh.primitive_cube_add(size=4)
    domain = bpy.context.object
    domain.name = 'Native gas domain'
    modifier = domain.modifiers.new('Mantaflow domain', 'FLUID')
    modifier.fluid_type = 'DOMAIN'
    settings = modifier.domain_settings
    settings.domain_type = 'GAS'
    settings.resolution_max = 16
    settings.use_adaptive_domain = False
    settings.use_noise = False
    settings.cache_type = 'REPLAY'
    settings.cache_directory = str(cache_directory)
    settings.cache_frame_start = 1
    settings.cache_frame_end = 4
    settings.cache_data_format = 'OPENVDB'
    bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=8, radius=0.5,
                                       location=(0, 0, -1))
    emitter = bpy.context.object
    modifier = emitter.modifiers.new('Smoke source', 'FLUID')
    modifier.fluid_type = 'FLOW'
    modifier.flow_settings.flow_type = 'SMOKE'
    modifier.flow_settings.flow_behavior = 'INFLOW'
    frames = []
    for frame in range(1, 5):
        scene.frame_set(frame)
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = domain.evaluated_get(depsgraph)
        values = list(evaluated.modifiers['Mantaflow domain'].domain_settings.density_grid)
        assert len(values) == 16**3, len(values)
        assert all(math.isfinite(value) and value >= -1e-4 for value in values)
        frames.append(dict(frame=frame, cells=len(values), density=values,
                           totalDensity=sum(values), maximumDensity=max(values)))
    assert frames[-1]['maximumDensity'] > 0.1
    caches = sorted(str(path.relative_to(cache_directory))
                    for path in Path(cache_directory).rglob('*.vdb'))
    assert caches, 'Mantaflow did not write its OpenVDB cache'
    return dict(frames=frames, cacheFiles=caches)


def run(directory, cache_directory=None):
    assert bpy.app.build_options.openvdb
    gas = fluid(Path(cache_directory or Path(directory) / 'gas-cache'))
    clear()
    cases = {}
    for codec in ('zip', 'blosc'):
        volume = bpy.data.volumes.new('OpenVDB ' + codec)
        volume.filepath = str(Path(directory) / f'sphere-{codec}.vdb')
        obj = bpy.data.objects.new('Volume ' + codec, volume)
        bpy.context.collection.objects.link(obj)
        volume.grids.load()
        assert volume.grids.is_loaded, volume.grids.error_message
        assert set(grid.name for grid in volume.grids) == {'density', 'surface'}
        cases[codec] = snapshot(mesh_from_volume(obj, 'Surface ' + codec))
        cases[codec]['grids'] = sorted([dict(name=grid.name, dataType=grid.data_type)
                                        for grid in volume.grids], key=lambda grid: grid['name'])
    assert cases['zip'] == cases['blosc']

    clear()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=16)
    source = bpy.context.object
    source.name = 'Source mesh'
    volume = bpy.data.volumes.new('Native mesh to volume')
    obj = bpy.data.objects.new('Mesh to volume', volume)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new('Mesh to volume', 'MESH_TO_VOLUME')
    modifier.object = source
    modifier.resolution_mode = 'VOXEL_SIZE'
    modifier.voxel_size = 0.125
    modifier.density = 1.0
    cases['meshToVolumeToMesh'] = snapshot(mesh_from_volume(obj, 'Browser volume surface'))
    source.hide_set(True)
    obj.hide_set(True)
    target = bpy.data.objects['Browser volume surface']
    bpy.context.view_layer.objects.active = target
    target.select_set(True)
    target.color = (0.05, 0.45, 0.8, 1.0)
    return dict(blenderVersion=bpy.app.version_string, cases=cases, fluid=gas)


def compare(actual, reference):
    assert actual['blenderVersion'] == reference['blenderVersion']
    maximum = 0.0
    for name, case in actual['cases'].items():
        expected = reference['cases'][name]
        assert (case['vertices'], case['faces']) == (expected['vertices'], expected['faces']), name
        assert case.get('grids') == expected.get('grids'), name
        for point, original in zip(case['positions'], expected['positions']):
            maximum = max(maximum, *(abs(a-b) for a, b in zip(point, original)))
    assert maximum < 2e-5, maximum
    density_error = 0.0
    for frame, expected in zip(actual['fluid']['frames'], reference['fluid']['frames']):
        assert (frame['frame'], frame['cells']) == (expected['frame'], expected['cells'])
        density_error = max(density_error, *(abs(a-b) for a,b in
                            zip(frame['density'], expected['density'])))
    assert density_error < 1e-3, density_error
    actual['fluid']['maximumDesktopDensityError'] = density_error
    return maximum


def summary(actual):
    return dict(cases={name:{key:value for key,value in case.items() if key != 'positions'}
                       for name,case in actual['cases'].items()},
                fluid={**actual['fluid'], 'frames':[
                    {key:value for key,value in frame.items() if key != 'density'}
                    for frame in actual['fluid']['frames']]})


def browser_check(directory, prefix='VOLUME'):
    try:
        actual = run(directory)
        error = compare(actual, json.loads((Path(directory) / 'reference.json').read_text()))
        report = dict(maximumCoordinateError=error, **summary(actual))
        os.write(1, (prefix + '_DONE:' + json.dumps(report) + '\n').encode())
    except Exception:
        traceback.print_exc()
        os.write(1, (prefix + '_FAILED\n').encode())
    finally:
        os.write(1, (prefix + '_FINISHED:\n').encode())
