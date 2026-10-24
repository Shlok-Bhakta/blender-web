# SPDX-License-Identifier: GPL-2.0-or-later
"""Authored compressed glTF exchange through Blender's original operators."""
import hashlib
import json
import math
from pathlib import Path
import struct

CASES = {
    'draco': {'export_draco_mesh_compression_enable': True},
    'meshopt-ext': {'export_meshopt_compression_enable': True,
                    'export_meshopt_extension': 'EXT_meshopt_compression'},
    'meshopt-khr': {'export_meshopt_compression_enable': True,
                    'export_meshopt_extension': 'KHR_meshopt_compression'},
}


def clear_scene():
    import bpy
    # Keep the window/screen context alive while running from Blender's console.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for mesh in list(bpy.data.meshes):
        if mesh.users == 0:
            bpy.data.meshes.remove(mesh)
    for action in list(bpy.data.actions):
        if action.users == 0:
            bpy.data.actions.remove(action)


def create_scene():
    import bpy
    clear_scene()
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12)
    obj = bpy.context.object
    obj.name = 'Compressed asset'
    # Fix quad diagonals before export instead of relying on native beauty
    # tessellation tie-breaking, which can reorder indices between processes.
    import bmesh
    mesh = bmesh.new()
    mesh.from_mesh(obj.data)
    bmesh.ops.triangulate(mesh, faces=list(mesh.faces), quad_method='FIXED', ngon_method='EAR_CLIP')
    mesh.to_mesh(obj.data)
    mesh.free()
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    colors = obj.data.color_attributes.new(name='Color', type='FLOAT_COLOR', domain='CORNER')
    for loop in obj.data.loops:
        co = obj.data.vertices[loop.vertex_index].co
        colors.data[loop.index].color = tuple((v + 1) / 2 for v in co) + (1.,)
    obj.shape_key_add(name='Basis')
    key = obj.shape_key_add(name='Stretch')
    for vertex in key.data:
        vertex.co.z *= 1.2
    for frame, angle, stretch in [(1, 0., 0.), (12, .4, .5), (24, .8, 1.)]:
        obj.rotation_euler.z = angle
        obj.keyframe_insert('rotation_euler', frame=frame)
        key.value = stretch
        key.keyframe_insert('value', frame=frame)
    bpy.context.scene.frame_end = 24
    bpy.context.scene.frame_set(1)
    return obj


def glb_json(path):
    data = Path(path).read_bytes()
    assert data[:4] == b'glTF'
    assert struct.unpack_from('<II', data, 4) == (2, len(data))
    length, kind = struct.unpack_from('<II', data, 12)
    assert kind == 0x4E4F534A
    return json.loads(data[20:20 + length])


def export(path, case):
    import bpy
    # The pinned desktop sampled-object exporter writes packed 8-byte quaternion
    # data into a 16-byte allocation. Zero its unused tail so that padding in
    # authored reference files contains no uninitialized data. Its original
    # encoder, metadata and decoder remain in use for the compatibility fixture.
    import sys
    import numpy as np
    original_empty = np.empty
    if sys.platform != 'emscripten':
        np.empty = np.zeros
    try:
        result = bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB',
            export_yup=True, export_animations=True, export_morph=True,
            export_vertex_color='ACTIVE', **CASES[case])
    finally:
        np.empty = original_empty
    assert result == {'FINISHED'}, result
    data = glb_json(path)
    extension = 'KHR_draco_mesh_compression' if case == 'draco' else CASES[case]['export_meshopt_extension']
    assert extension in data['extensionsUsed'] and extension in data['extensionsRequired']
    primitives = [p for mesh in data['meshes'] for p in mesh['primitives']]
    assert all('POSITION' in p['attributes'] and 'TEXCOORD_0' in p['attributes'] and 'COLOR_0' in p['attributes'] for p in primitives)
    assert data['animations'] and any(p.get('targets') for p in primitives)
    if case == 'draco':
        assert all(extension in p.get('extensions', {}) for p in primitives)
    else:
        compressed = [v['extensions'][extension] for v in data['bufferViews'] if extension in v.get('extensions', {})]
        assert all(v['byteLength'] == v['extensions'][extension]['count'] * v['extensions'][extension]['byteStride']
                   for v in data['bufferViews'] if extension in v.get('extensions', {}))
        assert compressed and any(v['mode'] == 'TRIANGLES' for v in compressed)
        assert any(v.get('filter') == 'EXPONENTIAL' for v in compressed)
        assert any(v.get('filter') == 'QUATERNION' for v in compressed)
        if sys.platform == 'emscripten':
            assert all(v['byteStride'] == 8 for v in compressed if v.get('filter') == 'QUATERNION')
    return {'bytes': Path(path).stat().st_size, 'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest(),
            'extension': extension, 'animations': len(data['animations'])}


def snapshot():
    import bpy
    obj, = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    # Compare sorted geometric samples because Draco can reorder vertices.
    bpy.context.scene.frame_set(1)
    mesh = obj.data
    vertices = sorted(tuple(float(c) for c in v.co) for v in mesh.vertices)
    triangles = sum(len(p.vertices) - 2 for p in mesh.polygons)
    assert len(mesh.uv_layers) == 1 and len(mesh.color_attributes) >= 1
    assert len(mesh.shape_keys.key_blocks) == 2
    key = mesh.shape_keys.key_blocks[1]
    assert max(v.co.z for v in key.data) > 1.19
    # Attribute moments include geometry correlations, so missing or scrambled
    # UV/color/normal arrays cannot pass merely by keeping the layer names.
    attributes = []
    uv = mesh.uv_layers.active.data
    color = mesh.color_attributes[0]
    for loop in mesh.loops:
        co = mesh.vertices[loop.vertex_index].co
        rgba = color.data[loop.index if color.domain == 'CORNER' else loop.vertex_index].color
        attributes.append(list(co) + list(uv[loop.index].uv) + list(rgba) + list(mesh.corner_normals[loop.index].vector))
    moments = [math.fsum(row[c] for row in attributes) / len(attributes) for c in range(12)]
    moments += [math.fsum(row[c] ** 2 for row in attributes) / len(attributes) for c in range(12)]
    moments += [math.fsum(row[c] * row[axis] for row in attributes) / len(attributes)
                for c in range(3, 12) for axis in range(3)]
    motion = []
    for frame in (1, 12, 24):
        bpy.context.scene.frame_set(frame)
        motion.append({'rotation': [float(v) for v in obj.matrix_local.to_euler()],
                       'stretch': float(key.value)})
    assert abs(motion[-1]['rotation'][2] - .8) < .005, motion
    assert abs(motion[-1]['stretch'] - 1.) < .005
    return {'vertices': vertices, 'triangles': triangles,
            'uvLayers': len(mesh.uv_layers), 'colorLayers': len(mesh.color_attributes),
            'shapeKeys': len(mesh.shape_keys.key_blocks), 'motion': motion, 'attributeMoments': moments}


def imported(path):
    import bpy
    clear_scene()
    assert bpy.ops.import_scene.gltf(filepath=str(path)) == {'FINISHED'}
    return snapshot()


def compare(actual, expected):
    assert actual['triangles'] == expected['triangles']
    assert actual['shapeKeys'] == expected['shapeKeys']
    assert max(abs(a-b) for a,b in zip(actual['attributeMoments'], expected['attributeMoments'])) < .005
    # Use nearest points, allowing compression quantization and split UV seams.
    maximum = max(min(math.dist(a, b) for b in expected['vertices']) for a in actual['vertices'])
    assert maximum < .005, maximum
    assert abs(len(actual['vertices']) - len(expected['vertices'])) == 0
    for a, b in zip(actual['motion'], expected['motion']):
        assert math.dist(a['rotation'], b['rotation']) < .005
        assert abs(a['stretch'] - b['stretch']) < .005
    return maximum


def fixtures(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    result = {}
    for case in CASES:
        create_scene()
        path = directory / (case + '.glb')
        metadata = export(path, case)
        metadata['snapshot'] = imported(path)
        result[case] = metadata
    report = {'cases': result, 'sourceSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def check(directory):
    import bpy
    directory = Path(directory)
    reference = json.loads((directory / 'results.json').read_text())
    assert reference['sourceSha256'] == hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    result = {}
    for case in CASES:
        expected = reference['cases'][case]
        source = directory / (case + '.glb')
        assert hashlib.sha256(source.read_bytes()).hexdigest() == expected['sha256']
        native_import = imported(source)
        first_error = compare(native_import, expected['snapshot'])
        output = directory / ('browser-' + case + '.glb')
        metadata = export(output, case)
        exported_import = imported(output)
        second_error = compare(exported_import, expected['snapshot'])
        result[case] = dict(metadata, nativeImportError=first_error,
            roundtripError=second_error, snapshot=exported_import)
    bpy.ops.wm.save_as_mainfile(filepath=str(directory / 'compressed-assets.blend'))
    return {'cases': result, 'saved': snapshot(),
        'sourceSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def malformed_check(directory):
    import bpy
    directory = Path(directory)
    original = (directory / 'meshopt-khr.glb').read_bytes()
    json_length = struct.unpack_from('<I', original, 12)[0]
    binary_length, kind = struct.unpack_from('<II', original, 20 + json_length)
    assert kind == 0x004E4942
    binary = original[28 + json_length:28 + json_length + binary_length]
    rejected = []
    for case in ('truncated-header', 'invalid-vertex-stride', 'truncated-codec-data'):
        path = directory / ('invalid-' + case + '.glb')
        if case == 'truncated-header':
            path.write_bytes(original[:24])
        else:
            doc = glb_json(directory / 'meshopt-khr.glb')
            view = next(v['extensions']['KHR_meshopt_compression'] for v in doc['bufferViews']
                        if v.get('extensions', {}).get('KHR_meshopt_compression', {}).get('mode') == 'ATTRIBUTES')
            if case == 'invalid-vertex-stride':
                view['byteStride'] = 3
            else:
                view['byteLength'] = 1
            encoded = json.dumps(doc, separators=(',', ':')).encode()
            encoded += b' ' * (-len(encoded) % 4)
            data = (struct.pack('<4sII', b'glTF', 2, 28 + len(encoded) + len(binary)) +
                    struct.pack('<II', len(encoded), 0x4E4F534A) + encoded +
                    struct.pack('<II', len(binary), 0x004E4942) + binary)
            path.write_bytes(data)
        clear_scene()
        try:
            result = bpy.ops.import_scene.gltf(filepath=str(path))
        except (RuntimeError, ValueError, TypeError, ImportError):
            rejected.append(case)
        else:
            assert result == {'CANCELLED'}, (case, result)
            rejected.append(case)
    # Exercise a good import in the same Python/Wasm session after every failure.
    actual = imported(directory / 'meshopt-khr.glb')
    expected = json.loads((directory / 'results.json').read_text())['cases']['meshopt-khr']['snapshot']
    compare(actual, expected)
    return {'rejectedFiles': rejected, 'validImportAfterFailures': True}


def browser_check(directory):
    try:
        malformed = malformed_check(directory)
        result = check(directory)
        result['malformed'] = malformed
        return result
    except BaseException:
        import os
        import traceback
        os.write(1, ('GLTF_FAILED:' + traceback.format_exc() + '\n').encode())
        raise


if __name__ == '__main__':
    import sys
    args = sys.argv[sys.argv.index('--') + 1:]
    result = fixtures(args[1]) if args[0] == 'fixtures' else check(args[1])
    print(json.dumps({'cases': list(result['cases']), 'sourceSha256': result['sourceSha256']}))
