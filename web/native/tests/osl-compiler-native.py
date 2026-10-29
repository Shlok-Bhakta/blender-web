# SPDX-License-Identifier: GPL-2.0-or-later
import bpy, json, pathlib, sys, oslquery, OpenImageIO as oiio
import numpy as np
from cycles.osl import update_script_node

base, directory = map(pathlib.Path, sys.argv[sys.argv.index('--') + 1:])
reference = json.loads((base / 'query-fixtures/native-query.json').read_text())

def parameter(p):
    return dict(name=p.name, type=str(p.type), output=p.isoutput,
                variableArray=p.varlenarray, structure=p.isstruct, closure=p.isclosure,
                value=p.value, metadata=[parameter(m) for m in p.metadata])

checked = []
for source in sorted(directory.glob('*.oso')):
    query = oslquery.OSLQuery(str(source))
    result = dict(name=query.shadername(), type=query.shadertype(),
                  parameters=[parameter(p) for p in query.parameters],
                  metadata=[parameter(p) for p in query.metadata])
    assert json.loads(json.dumps(result)) == reference, source.name
    checked.append(source.name)
assert len(checked) == 6

bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.shading_system = True
scene.cycles.samples = 1
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.render.resolution_x = scene.render.resolution_y = 16
scene.render.resolution_percentage = 100
scene.render.threads_mode = 'FIXED'
scene.render.threads = 4
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.image_settings.color_depth = '32'
bpy.ops.object.camera_add(location=(0, 0, 3))
scene.camera = bpy.context.object
scene.camera.data.type = 'ORTHO'
scene.camera.data.ortho_scale = 2
bpy.ops.mesh.primitive_plane_add(size=4)
plane = bpy.context.object
material = bpy.data.materials.new('Original compiled OSL')
material.use_nodes = True
plane.data.materials.append(material)
tree = material.node_tree
tree.nodes.clear()
script = tree.nodes.new('ShaderNodeScript')
script.mode = 'EXTERNAL'
emission = tree.nodes.new('ShaderNodeEmission')
output = tree.nodes.new('ShaderNodeOutputMaterial')
tree.links.new(emission.outputs[0], output.inputs['Surface'])
images = []
reports = []
for name, source in [('native', base/'query-fixtures/web_query.oso'),
                     ('browser', directory/'browser-file.oso')]:
    script.filepath = str(source)
    update_script_node(script, lambda level, message: reports.append([sorted(level), message]))
    assert 'result' in script.outputs, reports
    tree.links.new(script.outputs['result'], emission.inputs['Color'])
    tree.links.new(script.outputs['energy'], emission.inputs['Strength'])
    scene.render.filepath = str(directory / (name + '-render.exr'))
    bpy.ops.render.render(write_still=True)
    image = oiio.ImageBuf(scene.render.filepath)
    pixels = image.get_pixels(oiio.FLOAT)
    assert pixels.shape == (16, 16, 4), pixels.shape
    images.append(pixels.copy())
assert np.array_equal(images[0], images[1]), np.max(np.abs(images[0]-images[1]))
assert np.max(np.abs(images[1][8,8,:3] - [2.5,5.0,7.5])) < 1e-6
report = dict(compilerVersion=oslquery.VERSION_STRING, blenderRevision=bpy.app.build_hash.decode(),
              nativeMetadataExact=checked, nativeCyclesPixelExact=True,
              centerPixel=images[1][8,8].tolist(), reports=reports,
              browserShaderExecutionVerified=False)
(directory/'native-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
