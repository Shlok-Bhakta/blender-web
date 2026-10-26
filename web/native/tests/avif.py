# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Blender's original ImBuf AVIF path, including odd-width byte input."""
from pathlib import Path


def run(directory):
    import bpy
    import numpy as np
    import OpenImageIO as oiio
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    old_settings = (scene.render.image_settings.file_format, scene.render.image_settings.color_mode,
                    scene.render.image_settings.color_depth, scene.render.image_settings.quality,
                    scene.view_settings.view_transform, scene.view_settings.look)
    array = np.array([((x * 7 + y * 11 + c * 31) % 256) / 255 if c < 3 else (x % 5 + 1) / 5
                      for y in range(23) for x in range(37) for c in range(4)], np.float32)
    cases = []
    image = loaded = None
    try:
        scene.render.image_settings.file_format = 'AVIF'
        scene.render.image_settings.quality = 100
        scene.view_settings.view_transform = 'Standard'
        scene.view_settings.look = 'None'
        for bits in (8, 10, 12):
            for mode in ('BW', 'RGB', 'RGBA'):
                scene.render.image_settings.color_mode = mode
                scene.render.image_settings.color_depth = str(bits)
                image = bpy.data.images.new('Original AVIF byte/float input', width=37, height=23,
                                            alpha=True, float_buffer=bits > 8)
                assert image.is_float == (bits > 8)
                image.pixels.foreach_set(array)
                name = f'avif-{bits}-{mode}.avif'
                path = directory / name
                image.save_render(str(path), scene=scene)
                loaded = bpy.data.images.load(str(path), check_existing=False)
                assert tuple(loaded.size) == (37, 23)
                pixels = np.array(loaded.pixels[:], np.float32)
                assert np.isfinite(pixels).all()
                reader = oiio.ImageInput.open(str(path))
                assert reader, oiio.geterror()
                spec = reader.spec()
                assert spec.get_int_attribute('oiio:BitsPerSample', 8) == bits
                channels = spec.nchannels
                assert channels == {'BW': 1, 'RGB': 3, 'RGBA': 4}[mode]
                decoded = reader.read_image(format=oiio.FLOAT)
                assert decoded.shape == (23, 37, channels)
                assert reader.close()
                decoded.astype('<f4').tofile(directory / (name + '.f32'))
                pixels.astype('<f4').tofile(directory / (name + '.blender.f32'))
                cases.append(dict(name=name, bits=bits, mode=mode, channels=channels,
                                  bytes=path.stat().st_size, byteInput=bits == 8))
                bpy.data.images.remove(loaded)
                loaded = None
                bpy.data.images.remove(image)
                image = None
        bad = directory / 'avif-truncated.avif'
        bad.write_bytes((directory / cases[0]['name']).read_bytes()[:32])
        assert oiio.ImageInput.open(str(bad)) is None
        assert oiio.geterror(), 'The rejected input must provide its original decoder error'
        reader = oiio.ImageInput.open(str(directory / cases[-1]['name']))
        assert reader and reader.read_image(format=oiio.FLOAT) is not None
        assert reader.close()
    finally:
        if loaded is not None:
            bpy.data.images.remove(loaded)
        if image is not None:
            bpy.data.images.remove(image)
        settings = scene.render.image_settings
        settings.file_format, settings.color_mode, settings.color_depth, settings.quality = old_settings[:4]
        scene.view_settings.view_transform, scene.view_settings.look = old_settings[4:]
    return dict(originalBlenderAVIF=True, oddWidth=37, height=23, cases=cases,
                truncatedInputRejected=True, decoderRecovery=True)
