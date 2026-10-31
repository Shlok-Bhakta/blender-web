# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministic Cycles OSL scene shared by native and Wasm acceptance.

The cube uses original node shaders (noise, color ramp, wave bump and a
generated image texture). The plane uses an internal OSL script that Cycles
compiles at render time.
"""
import bpy

SCRIPT = '''
shader web_checker(point Pos = P, float Scale = 3.0, output color Color = 0)
{
    point p = Pos * Scale;
    int parity = (int(floor(p[0])) + int(floor(p[1])) + int(floor(p[2]))) & 1;
    Color = parity ? color(0.9, 0.25, 0.1) : color(0.1, 0.3, 0.85);
}
'''


def material_nodes(name):
    material = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    output = nodes.new('ShaderNodeOutputMaterial')
    bsdf = nodes.new('ShaderNodeBsdfPrincipled')
    links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])
    return material, nodes, links, bsdf


def configure_cycles_osl(size=128, samples=16):
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.shading_system = True
    scene.cycles.samples = samples
    scene.cycles.use_denoising = False
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.seed = 0
    scene.render.resolution_x = size
    scene.render.resolution_y = size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'

    cube = bpy.data.objects['Cube']
    material, nodes, links, bsdf = material_nodes('WebOSLNodes')
    noise = nodes.new('ShaderNodeTexNoise')
    noise.inputs['Scale'].default_value = 6.0
    ramp = nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].color = (0.05, 0.4, 0.1, 1.0)
    ramp.color_ramp.elements[1].color = (0.95, 0.85, 0.2, 1.0)
    image = bpy.data.images.get('WebOSLGrid') or bpy.data.images.new('WebOSLGrid', 64, 64)
    image.generated_type = 'UV_GRID'
    texture = nodes.new('ShaderNodeTexImage')
    texture.image = image
    texture.interpolation = 'Closest'
    mix = nodes.new('ShaderNodeMix')
    mix.data_type = 'RGBA'
    mix.inputs['Factor'].default_value = 0.5
    wave = nodes.new('ShaderNodeTexWave')
    bump = nodes.new('ShaderNodeBump')
    bump.inputs['Strength'].default_value = 0.3
    links.new(noise.outputs['Fac'], ramp.inputs['Fac'])
    links.new(ramp.outputs['Color'], mix.inputs['A'])
    links.new(texture.outputs['Color'], mix.inputs['B'])
    links.new(mix.outputs['Result'], bsdf.inputs['Base Color'])
    links.new(wave.outputs['Fac'], bump.inputs['Height'])
    links.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
    cube.data.materials.clear()
    cube.data.materials.append(material)

    if 'WebOSLPlane' not in bpy.data.objects:
        bpy.ops.mesh.primitive_plane_add(size=8, location=(0, 0, -1))
        bpy.context.active_object.name = 'WebOSLPlane'
    plane = bpy.data.objects['WebOSLPlane']
    material, nodes, links, bsdf = material_nodes('WebOSLScript')
    text = bpy.data.texts.get('web_checker.osl') or bpy.data.texts.new('web_checker.osl')
    text.from_string(SCRIPT)
    script = nodes.new('ShaderNodeScript')
    script.mode = 'INTERNAL'
    script.script = text
    # Compile with Cycles' own OSL compiler; this creates the node's sockets.
    from cycles import osl
    messages = []
    osl.update_script_node(script, lambda kind, message: messages.append((sorted(kind), message)))
    if not script.bytecode or 'Color' not in script.outputs:
        raise RuntimeError(f'OSL script compilation failed: {messages}')
    links.new(script.outputs['Color'], bsdf.inputs['Base Color'])
    plane.data.materials.clear()
    plane.data.materials.append(material)
    return scene
