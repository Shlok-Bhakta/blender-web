# SPDX-License-Identifier: GPL-2.0-or-later
"""Identical CPU and scene-memory workloads for desktop and browser Blender."""
import bpy
import gc
import json
import math
import os
import time
import traceback
import numpy as np


def emit(kind, value):
    os.write(1, (kind + ':' + json.dumps(value) + '\n').encode())


def memory(stage):
    emit('PERF_MEMORY', dict(stage=stage))
    # Give the external process-tree sampler a window with this scene alive.
    time.sleep(.25)


def clear():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for mesh in list(bpy.data.meshes):
        if not mesh.users:
            bpy.data.meshes.remove(mesh)
    gc.collect()


def grid(n):
    x, y = np.meshgrid(np.linspace(-5, 5, n, dtype=np.float32), np.linspace(-5, 5, n, dtype=np.float32))
    xyz = np.column_stack((x.ravel(), y.ravel(), (.25*np.sin(x)*np.cos(y)).ravel()))
    base = (np.arange(n-1)[:, None]*n + np.arange(n-1)[None, :]).ravel().astype(np.int32)
    corners = np.column_stack((base, base+1, base+n+1, base+n)).ravel()
    mesh = bpy.data.meshes.new('BenchmarkGrid')
    mesh.vertices.add(n*n)
    mesh.vertices.foreach_set('co', xyz.ravel())
    mesh.loops.add(len(corners))
    mesh.loops.foreach_set('vertex_index', corners)
    mesh.polygons.add(len(base))
    mesh.polygons.foreach_set('loop_start', np.arange(len(base), dtype=np.int32)*4)
    mesh.polygons.foreach_set('loop_total', np.full(len(base), 4, dtype=np.int32))
    mesh.update()
    obj = bpy.data.objects.new('BenchmarkGrid', mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def run():
    rounds = 3
    report = dict(blenderVersion=bpy.app.version_string, numpyVersion=np.__version__,
                  platform=__import__('sys').platform, cpuCount=os.cpu_count(), rounds=rounds,
                  grids=[], oceans=[])
    clear()
    memory('baseline')
    # This includes the real NumPy/RNA transfer, topology construction, mesh
    # update and native CPU subdivision. No viewport drawing is timed.
    for n in (128, 256, 384):
        for repeat in range(rounds):
            clear()
            started = time.perf_counter()
            obj = grid(n)
            created = time.perf_counter()
            modifier = obj.modifiers.new('CPU subdivision', 'SUBSURF')
            modifier.subdivision_type = 'CATMULL_CLARK'
            modifier.levels = modifier.render_levels = 2
            bpy.context.preferences.system.use_gpu_subdivision = False
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            finished = time.perf_counter()
            expected = (n-1)*(n-1)*16
            assert len(mesh.polygons) == expected
            vertices = len(mesh.vertices)
            assert vertices > expected and all(math.isfinite(v.co.z) for v in (mesh.vertices[0], mesh.vertices[-1]))
            result = dict(grid=n, repeat=repeat, baseVertices=n*n,
                          evaluatedVertices=vertices, evaluatedFaces=expected,
                          createSeconds=created-started, subdivisionSeconds=finished-created,
                          totalSeconds=finished-started)
            report['grids'].append(result)
            emit('PERF_CASE', result)
            memory(f'grid-{n}-repeat-{repeat}')
            evaluated.to_mesh_clear()
    for repeat in range(rounds):
        clear()
        bpy.ops.mesh.primitive_plane_add()
        obj = bpy.context.object
        modifier = obj.modifiers.new('FFTW ocean', 'OCEAN')
        modifier.viewport_resolution = modifier.resolution = 16
        modifier.random_seed = 7
        modifier.use_normals = modifier.use_foam = True
        started = time.perf_counter()
        for frame in range(1, 13):
            modifier.time = frame / 6
            obj.update_tag()
            evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
            mesh = evaluated.to_mesh()
            assert len(mesh.vertices) == 66049 and len(mesh.polygons) == 65536
            evaluated.to_mesh_clear()
        seconds = time.perf_counter()-started
        result = dict(repeat=repeat, resolution=16, frames=12, vertices=66049,
                      faces=65536, seconds=seconds)
        report['oceans'].append(result)
        emit('PERF_CASE', result)
        memory(f'ocean-repeat-{repeat}')
    clear()
    memory('after-cleanup')
    return report


if __name__ == '__main__':
    try:
        emit('PERF_COMPLETE', run())
    except Exception:
        emit('PERF_FAILED', traceback.format_exc())
    finally:
        emit('PERF_DONE', True)
