# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare Blender's original USD operators and Python hooks across architectures."""
import bpy
import hashlib
import json
import math
import os
from pathlib import Path
import traceback

FORMATS = ('usda', 'usdc', 'usdz')


def clear():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)


def author_scene():
    clear()
    scene = bpy.context.scene
    scene.frame_start, scene.frame_end = 1, 10
    scene.world = bpy.data.worlds.new('ExchangeWorld')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes.get('Background').inputs['Color'].default_value = (.05, .05, .05, 1)
    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.object
    cube.name = 'PortCube'
    cube.rotation_euler = (.2, -.1, .35)
    cube.scale = (1, .7, 1.3)
    material = bpy.data.materials.new('ExchangeBlue')
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Base Color'].default_value = (.04, .3, .8, 1)
    bsdf.inputs['Roughness'].default_value = .32
    bsdf.inputs['Metallic'].default_value = .15
    cube.data.materials.append(material)
    for frame, location in [(1, (0, 0, 0)), (10, (2, -1, .5))]:
        cube.location = location
        cube.keyframe_insert(data_path='location', frame=frame)
    camera = bpy.data.objects.new('ExchangeCamera', bpy.data.cameras.new('ExchangeCamera'))
    scene.collection.objects.link(camera)
    camera.location = (7, -7, 5)
    camera.rotation_euler = (1.1, 0, .8)
    camera.data.lens = 42
    scene.camera = camera
    light = bpy.data.objects.new('ExchangeLight', bpy.data.lights.new('ExchangeLight', 'POINT'))
    scene.collection.objects.link(light)
    light.location = (2, -3, 5)
    light.data.energy = 750
    scene.frame_set(1)


def snapshot():
    scene = bpy.context.scene
    frames = {}
    for frame in (1, 10):
        scene.frame_set(frame)
        objects = []
        for obj in sorted(scene.objects, key=lambda o: (o.type, o.name)):
            if obj.type not in {'MESH', 'CAMERA', 'LIGHT'}:
                continue
            item = {'type': obj.type, 'matrix': [v for row in obj.matrix_world for v in row]}
            if obj.type == 'MESH':
                evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
                mesh = evaluated.to_mesh()
                item.update(vertices=[v for vertex in mesh.vertices for v in vertex.co],
                            polygons=[list(p.vertices) for p in mesh.polygons],
                            uv=[v for layer in mesh.uv_layers for uv in layer.uv for v in uv.vector],
                            materials=[])
                for material in obj.data.materials:
                    bsdf = material.node_tree.nodes.get('Principled BSDF')
                    item['materials'].append({name: list(bsdf.inputs[name].default_value)
                        if name == 'Base Color' else bsdf.inputs[name].default_value
                        for name in ('Base Color', 'Roughness', 'Metallic')})
                evaluated.to_mesh_clear()
            elif obj.type == 'CAMERA':
                item['lens'] = obj.data.lens
            else:
                item.update(energy=obj.data.energy, lightType=obj.data.type)
            objects.append(item)
        frames[str(frame)] = objects
    scene.frame_set(1)
    environments = [node.image for node in scene.world.node_tree.nodes
                    if node.type == 'TEX_ENVIRONMENT' and node.image]
    assert len(environments) == 1, 'USD world texture must be imported and decoded'
    frames['world'] = {'size': list(environments[0].size), 'pixels': list(environments[0].pixels)}
    return frames


def compare(actual, expected, path='scene'):
    if isinstance(expected, dict):
        assert actual.keys() == expected.keys(), path
        return max((compare(actual[k], v, path + '.' + k) for k, v in expected.items()), default=0)
    if isinstance(expected, list):
        assert len(actual) == len(expected), (path, len(actual), len(expected))
        return max((compare(a, b, path + f'[{i}]') for i, (a, b) in enumerate(zip(actual, expected))), default=0)
    if isinstance(expected, (int, float)):
        assert math.isfinite(actual), path
        error = abs(actual - expected)
        assert error < 2e-4, (path, actual, expected, error)
        return error
    assert actual == expected, (path, actual, expected)
    return 0


class ExchangeHook(bpy.types.USDHook):
    bl_idname = 'USDHookWebExchange'
    bl_label = 'Web exchange validation'
    events = []

    @staticmethod
    def on_export(context):
        from pxr import Usd
        stage = context.get_stage()
        assert isinstance(stage, Usd.Stage)
        stage.GetRootLayer().customLayerData = {'blender_web_hook': 'original-python-hook'}
        ExchangeHook.events.append('export')
        return True

    @staticmethod
    def on_import(context):
        from pxr import Usd
        assert isinstance(context.get_stage(), Usd.Stage)
        assert context.get_prim_map()
        ExchangeHook.events.append('import')
        return True


def export(path):
    result = bpy.ops.wm.usd_export(filepath=str(path), export_animation=True,
        generate_materialx_network=True, generate_preview_surface=True,
        export_uvmaps=True, export_materials=True, export_cameras=True,
        export_lights=True, overwrite_textures=True, evaluation_mode='RENDER')
    assert result == {'FINISHED'}, result


def import_file(path):
    clear()
    bpy.context.scene.world = bpy.data.worlds.new('ImportedWorld')
    bpy.context.scene.world.use_nodes = True
    result = bpy.ops.wm.usd_import(filepath=str(path), import_materials=True,
        import_cameras=True, import_lights=True, set_frame_range=True)
    assert result == {'FINISHED'}, result


def inspect_stage(path):
    from pxr import Ar, Sdf, Usd, UsdGeom, UsdShade
    stage = Usd.Stage.Open(str(path))
    assert stage
    prims = list(stage.Traverse())
    assert any(prim.IsA(UsdGeom.Mesh) for prim in prims)
    shaders = [UsdShade.Shader(prim) for prim in prims if prim.IsA(UsdShade.Shader)]
    identifiers = sorted(str(shader.GetIdAttr().Get()) for shader in shaders)
    assert 'UsdPreviewSurface' in identifiers, identifiers
    assert any(identifier.startswith('ND_') for identifier in identifiers), identifiers
    surface = next(shader for shader in shaders
                   if str(shader.GetIdAttr().Get()) == 'ND_open_pbr_surface_surfaceshader')
    materialx_inputs = {'base_color': list(surface.GetInput('base_color').Get()),
                       'base_metalness': float(surface.GetInput('base_metalness').Get()),
                       'specular_roughness': float(surface.GetInput('specular_roughness').Get())}
    hook = stage.GetRootLayer().customLayerData.get('blender_web_hook')
    assert hook == 'original-python-hook', hook
    assets = []
    for prim in prims:
        for attribute in prim.GetAttributes():
            if attribute.GetTypeName() != Sdf.ValueTypeNames.Asset:
                continue
            value = attribute.Get()
            if value and value.path:
                assert value.resolvedPath, (str(attribute.GetPath()), value.path)
                asset = Ar.GetResolver().OpenAsset(Ar.ResolvedPath(value.resolvedPath))
                assert asset and asset.GetSize() > 0, value.path
                assets.append({'attribute': str(attribute.GetPath()), 'bytes': asset.GetSize()})
    assert assets, 'The exported world texture must resolve, including inside USDZ'
    return dict(prims=len(prims), shaderIdentifiers=identifiers, hookMetadata=hook,
                materialXSurfaceInputs=materialx_inputs, startTime=stage.GetStartTimeCode(),
                endTime=stage.GetEndTimeCode(), assets=assets)


def fixtures(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    bpy.utils.register_class(ExchangeHook)
    try:
        author_scene()
        for ext in FORMATS:
            export(directory / ('desktop.' + ext))
        report = {'formats': {}, 'blenderVersion': bpy.app.version_string,
                  'sourceSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
        for ext in FORMATS:
            path = directory / ('desktop.' + ext)
            stage = inspect_stage(path)
            import_file(path)
            report['formats'][ext] = {'scene': snapshot(), 'stage': stage}
        report['hookEvents'] = ExchangeHook.events
        assert report['hookEvents'].count('export') == 3 and report['hookEvents'].count('import') == 3
        (directory / 'reference.json').write_text(json.dumps(report, indent=2) + '\n')
        return report
    finally:
        bpy.utils.unregister_class(ExchangeHook)


def run(directory):
    directory = Path(directory)
    exported = directory / 'export'
    exported.mkdir(exist_ok=True)
    reference = json.loads((directory / 'reference.json').read_text())
    from pxr import Usd, UsdMtlx, Sdf, Gf, Vt
    import importlib
    import sys
    static_modules = sorted(name for name in sys.builtin_module_names if name.startswith('pxr.'))
    if sys.platform == 'emscripten':
        assert len(static_modules) >= 29, static_modules
        for module in static_modules:
            importlib.import_module(module.rsplit('.', 1)[0])
    report = {'usdVersion': Usd.GetVersion(), 'formats': {},
              'staticPythonModules': static_modules,
              'buildOptions': {n: getattr(bpy.app.build_options, n) for n in dir(bpy.app.build_options)
                               if not n.startswith('_') and isinstance(getattr(bpy.app.build_options, n), bool)}}
    bpy.utils.register_class(ExchangeHook)
    try:
        for ext in FORMATS:
            import_file(directory / ('desktop.' + ext))
            error = compare(snapshot(), reference['formats'][ext]['scene'])
            author_scene()
            path = exported / ('browser.' + ext)
            export(path)
            stage = inspect_stage(path)
            materialx_error = compare(stage['materialXSurfaceInputs'],
                                      reference['formats'][ext]['stage']['materialXSurfaceInputs'])
            assert stage['shaderIdentifiers'] == reference['formats'][ext]['stage']['shaderIdentifiers']
            import_file(path)
            scene = snapshot()
            own_error = compare(scene, reference['formats'][ext]['scene'])
            report['formats'][ext] = {'desktopImportMaximumError': error,
                'roundtripMaximumError': own_error, 'maximumMaterialXInputError': materialx_error,
                'bytes': path.stat().st_size,
                'stage': stage, 'scene': scene}
        report['hookEvents'] = ExchangeHook.events[:]
        assert report['hookEvents'].count('export') == 3 and report['hookEvents'].count('import') == 6
        bpy.ops.wm.save_as_mainfile(filepath=str(directory / 'usd-exchange.blend'))
        (directory / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        return report
    finally:
        bpy.utils.unregister_class(ExchangeHook)


def browser_check(directory, marker='USD_EXCHANGE'):
    try:
        result = run(directory)
        os.write(1, (marker + '_DONE:' + json.dumps(result) + '\n').encode())
    except Exception:
        traceback.print_exc()
        os.write(1, (marker + '_FAILED\n').encode())
    finally:
        os.write(1, (marker + '_FINISHED:\n').encode())


def native_crosscheck(directory):
    directory = Path(directory)
    reference = json.loads((directory / 'results.json').read_text())
    exported = directory / 'export' if (directory / 'export').is_dir() else directory
    result = {}
    for ext in FORMATS:
        path = exported / ('browser.' + ext)
        stage = inspect_stage(path)
        materialx_error = compare(stage['materialXSurfaceInputs'],
                                  reference['formats'][ext]['stage']['materialXSurfaceInputs'])
        import_file(path)
        error = compare(snapshot(), reference['formats'][ext]['scene'])
        result[ext] = {'maximumBrowserSceneError': error,
                       'maximumBrowserMaterialXInputError': materialx_error, 'stage': stage}
    (directory / 'native-crosscheck.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    import sys
    action, directory = sys.argv[sys.argv.index('--') + 1:]
    print(json.dumps({'fixtures': fixtures, 'native-crosscheck': native_crosscheck}[action](directory)))
