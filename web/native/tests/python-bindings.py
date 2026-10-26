# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise original Python bindings with files that native Blender can read."""
import concurrent.futures
import hashlib
import json
from pathlib import Path


def run(directory):
    import numpy as np
    import OpenImageIO as oiio
    import PyOpenColorIO as ocio
    import openvdb
    import MaterialX as mx
    import MaterialX.PyMaterialXGenShader as gen
    from MaterialX.PyMaterialXGenGlsl import GlslShaderGenerator
    from MaterialX.PyMaterialXGenMsl import MslShaderGenerator
    from MaterialX.PyMaterialXGenOsl import OslShaderGenerator
    from MaterialX.PyMaterialXGenMdl import MdlShaderGenerator
    from MaterialX import PyMaterialXRender as render

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    assert all(hasattr(package, '__path__') for package in (oiio, ocio, mx))
    config = ocio.Config.CreateRaw()
    transform = ocio.MatrixTransform(matrix=[2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1],
                                     offset=[.1, .2, .3, 0])
    processor = config.getProcessor(transform)
    cpu = processor.getDefaultCPUProcessor()
    original = np.array([[.125, .25, .5], [1, .5, .25]], np.float32)
    expected = original * np.array([2, 3, 4], np.float32) + np.array([.1, .2, .3], np.float32)

    def apply_color(index):
        data = original.copy()
        cpu.applyRGB(data)
        assert np.allclose(data, expected, atol=1e-6)
        return data.tolist()

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        colors = list(pool.map(apply_color, range(9)))
    serialized = config.serialize()
    (directory / 'config.ocio').write_text(serialized)
    assert list(ocio.Config.CreateFromStream(serialized).getColorSpaceNames()) == list(config.getColorSpaceNames())
    try:
        config.getProcessor('absent', 'absent')
    except ocio.Exception:
        pass
    else:
        raise AssertionError('Invalid color space accepted')
    desc = ocio.GpuShaderDesc.CreateShaderDesc()
    desc.setLanguage(ocio.GPU_LANGUAGE_GLSL_4_0)
    desc.setFunctionName('blender_web_color_transform')
    processor.getDefaultGPUProcessor().extractGpuShaderInfo(desc)
    assert 'blender_web_color_transform' in desc.getShaderText()
    (directory / 'color-transform.glsl').write_text(desc.getShaderText())

    array = np.arange(23 * 37 * 4, dtype=np.float32).reshape(23, 37, 4) / 4096
    image = oiio.ImageBuf(oiio.ImageSpec(37, 23, 4, oiio.FLOAT))
    assert image.set_pixels(oiio.ROI(0, 37, 0, 23, 0, 1, 0, 4), array)
    assert np.array_equal(image.get_pixels(oiio.FLOAT), array)
    images = []
    for extension in ('exr', 'tif', 'png'):
        path = directory / ('image.' + extension)
        converted = oiio.ImageBufAlgo.copy(image, oiio.FLOAT if extension == 'exr' else oiio.UINT16)
        assert converted.write(str(path)), converted.geterror()
        pixels = oiio.ImageBuf(str(path)).get_pixels(oiio.FLOAT)
        assert pixels.shape == array.shape
        error = float(np.max(np.abs(pixels - array)))
        assert error < (1e-6 if extension == 'exr' else 2 / 65535), (extension, error)
        images.append(dict(format=extension, maxError=error))
    resized = oiio.ImageBufAlgo.resize(image, roi=oiio.ROI(0, 19, 0, 11, 0, 1, 0, 4))
    assert resized.spec().width == 19 and resized.spec().height == 11
    assert abs(oiio.ImageBufAlgo.computePixelStats(image).avg[0] - float(array[:, :, 0].mean())) < 1e-6

    def multiply_image(index):
        result = oiio.ImageBufAlgo.mul(image, (2., 2., 2., 1.))
        assert np.array_equal(result.get_pixels(oiio.FLOAT)[:, :, :3], array[:, :, :3] * 2)
        return index

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        assert list(pool.map(multiply_image, range(9))) == list(range(9))

    assert not hasattr(openvdb, 'ax'), 'The native release also disables OpenVDB AX'
    grids, grid_types = [], []
    for cls in openvdb.GridTypes:
        grid = cls()
        grid.name = cls.__name__
        value = grid.background
        if isinstance(value, tuple):
            value = tuple(i + 1 for i in range(len(value)))
        elif isinstance(value, bool):
            value = True
        elif isinstance(value, float):
            value = 1.25
        elif isinstance(value, int):
            value = (1 << 54) + 7 if '64' in cls.__name__ else 1234
        else:
            raise AssertionError((cls, value))
        accessor = grid.getAccessor()
        accessor.setValueOn((-1, 2, 3), value)
        assert accessor.getValue((-1, 2, 3)) == value and grid.activeVoxelCount() == 1
        assert not grid.deepCopy().sharesWith(grid)
        grid_types.append(dict(name=cls.__name__, value=value))
        grids.append(grid)
    assert len(grids) == 9
    density = openvdb.FloatGrid()
    density.name = 'density'
    density.transform = openvdb.createLinearTransform(.5)
    voxels = np.arange(5 * 7 * 9, dtype=np.float32).reshape(5, 7, 9) / 8
    density.copyFromArray(voxels)
    density['source'] = 'browser-python'
    grids.append(density)
    openvdb.write(str(directory / 'volume.vdb'), grids, metadata={'fixture': 'original Python bindings'})
    loaded, metadata = openvdb.readAll(str(directory / 'volume.vdb'))
    assert metadata['fixture'] == 'original Python bindings'
    by_name = {grid.name: grid for grid in loaded}
    for item in grid_types:
        assert by_name[item['name']].getAccessor().getValue((-1, 2, 3)) == item['value']
    restored_voxels = np.zeros_like(voxels)
    by_name['density'].copyToArray(restored_voxels)
    assert np.array_equal(voxels, restored_voxels)
    assert by_name['density'].transform.indexToWorld((2, 4, 6)) == (1., 2., 3.)
    try:
        openvdb.read(str(directory / 'volume.vdb'), 'absent')
    except KeyError:
        pass
    else:
        raise AssertionError('Missing grid accepted')

    def sample_volume(index):
        local = openvdb.FloatGrid()
        local.getAccessor().setValueOn((index, 0, 0), index + .5)
        return local.getAccessor().getValue((index, 0, 0))

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        assert list(pool.map(sample_volume, range(12))) == [i + .5 for i in range(12)]

    document = mx.createDocument()
    search = mx.getDefaultDataSearchPath()
    assert mx.loadLibraries(mx.getDefaultDataLibraryFolders(), search, document)
    node = document.addNode('constant', 'browser_color', 'color3')
    node.setInputValue('value', mx.Color3(.25, .5, .75))
    output = document.addOutput('browser_output', 'color3')
    output.setConnectedNode(node)
    assert document.validate()[0], document.validate()[1]
    write_options = mx.XmlWriteOptions()
    write_options.writeXIncludeEnable = False
    write_options.elementPredicate = lambda element: not element.hasSourceUri()
    # Export authored elements without absolute paths into the runtime's libraries.
    xml = mx.writeToXmlString(document, write_options)
    (directory / 'material.mtlx').write_text(xml)
    restored = mx.createDocument()
    mx.readFromXmlString(restored, xml)
    assert restored.getNode('browser_color').getInputValue('value') == mx.Color3(.25, .5, .75)
    shaders = []
    for cls in (GlslShaderGenerator, MslShaderGenerator, OslShaderGenerator, MdlShaderGenerator):
        generator = cls.create()
        context = gen.GenContext(generator)
        context.registerSourceCodeSearchPath(search)
        generator.registerTypeDefs(document)
        shader = generator.generate('browser_shader', output, context)
        code = shader.getSourceCode(gen.PIXEL_STAGE)
        assert len(code) > 100
        target = generator.getTarget()
        (directory / ('shader-' + target + '.txt')).write_text(code)
        shaders.append(dict(target=target, bytes=len(code.encode()), sha256=hashlib.sha256(code.encode()).hexdigest()))
    render_image = render.createUniformImage(8, 6, 4, render.BaseType.FLOAT, mx.Color4(.25, .5, .75, 1))
    assert render_image.getTexelColor(3, 2) == mx.Color4(.25, .5, .75, 1)
    render_image.setTexelColor(1, 1, mx.Color4(1, 0, 0, 1))
    assert render_image.copy(4, render.BaseType.FLOAT).getTexelColor(1, 1) == mx.Color4(1, 0, 0, 1)
    small = render_image.applyBoxDownsample(2)
    assert small.getWidth() == 4 and small.getHeight() == 3
    mesh = render.Mesh.create('browser_mesh')
    mesh.setVertexCount(3)
    assert mesh.getVertexCount() == 3
    try:
        mx.readFromXmlString(mx.createDocument(), '<materialx><invalid>')
    except mx.ExceptionParseError:
        pass
    else:
        raise AssertionError('Malformed XML accepted')

    def create_document(index):
        local = mx.createDocument()
        local.addNode('constant', 'node' + str(index), 'float')
        return local.getNode('node' + str(index)).getName()

    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        assert list(pool.map(create_document, range(9))) == ['node' + str(i) for i in range(9)]
    report = dict(versions=dict(OpenImageIO=oiio.__version__, PyOpenColorIO=ocio.__version__, MaterialX=mx.__version__),
        originalPackageWrappers=True,
        color=dict(numpyTransform=colors[0], parallelCalls=9, serialization=True, shaderGeneration=True, invalidColorSpaceRejected=True),
        images=dict(numpyRoundtrip=True, files=images, resize=True, pixelStatistics=True, parallelCalls=9),
        # Keep values beyond 53 bits exact when the report passes through JSON in JavaScript.
        volume=dict(gridTypes=[dict(item, value=str(item['value'])) if item['name'] == 'Int64Grid' else item
                               for item in grid_types], numpyRoundtrip=True, fileRoundtrip=True, transform=True, metadata=True,
                    missingGridRejected=True, parallelCalls=12, axDisabled=True),
        material=dict(documentRoundtrip=True, shaders=shaders, renderImage=True, imageDownsample=True,
                      mesh=True, xmlRejection=True, parallelCalls=9))
    (directory / 'python-bindings.json').write_text(json.dumps(report, indent=2) + '\n')
    return report
