# SPDX-License-Identifier: GPL-2.0-or-later
"""Original Blender image codecs, SVG import, Potrace and vector exports."""
import bpy
import hashlib
import json
import math
import os
from pathlib import Path
import traceback

FORMATS = [('WEBP','webp','RGBA','8'), ('JPEG2000','jp2','RGB','16'),
           ('CINEON','cin','RGB','10'), ('DPX','dpx','RGBA','16')]
SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="128mm" height="128mm" viewBox="0 0 128 128"><path fill="#218bd1" stroke="#123e67" stroke-width="3" d="M16 16H112V112H16Z"/><path fill="#f7b13b" d="M64 28L102 92H26Z"/><circle cx="64" cy="69" r="12" fill="#9d3c68"/></svg>'''


def clear():
    for obj in list(bpy.data.objects):
        if obj.type != 'CAMERA':
            bpy.data.objects.remove(obj, do_unlink=True)


def image_values(image):
    values = list(image.pixels)
    assert image.size[:] == (64,64) and len(values)==16384
    assert all(math.isfinite(value) for value in values)
    assert max(values)-min(values)>.5
    return values


def write_images(directory):
    scene=bpy.context.scene
    scene.view_settings.view_transform='Standard'
    scene.view_settings.look='None'
    scene.view_settings.exposure=0
    scene.view_settings.gamma=1
    source=bpy.data.images.new('Codec pattern',width=64,height=64,alpha=True,float_buffer=True)
    source.pixels=[value for y in range(64) for x in range(64)
                   for value in (x/63,y/63,((x//8+y//8)%2),.25+.75*((x+y)%4)/3)]
    result={}
    for fmt,ext,mode,depth in FORMATS:
        settings=scene.render.image_settings
        settings.file_format=fmt;settings.color_mode=mode;settings.color_depth=depth
        settings.quality=100
        if fmt=='JPEG2000':settings.jpeg2k_codec='JP2'
        path=directory/('pattern.'+ext)
        source.save_render(str(path),scene=scene)
        loaded=bpy.data.images.load(str(path),check_existing=False)
        result[fmt]=dict(bytes=path.stat().st_size,pixels=image_values(loaded))
        bpy.data.images.remove(loaded)
    settings.file_format='PNG';settings.color_mode='RGBA';settings.color_depth='8'
    trace=bpy.data.images.new('Trace source',width=16,height=16,alpha=True)
    trace.pixels=[value for y in range(16) for x in range(16)
                  for value in ((0.,0.,0.,1.) if 3<=x<=12 and 3<=y<=12 else (1.,1.,1.,1.))]
    trace.save_render(str(directory/'trace.png'),scene=scene)
    return result


def gp_snapshot(obj):
    assert obj.type=='GREASEPENCIL'
    drawings=[frame.drawing for layer in obj.data.layers for frame in layer.frames]
    assert drawings
    points=[list(point.position) for drawing in drawings for stroke in drawing.strokes for point in stroke.points]
    assert points and all(math.isfinite(value) for point in points for value in point)
    return dict(curves=sum(len(drawing.strokes) for drawing in drawings),points=points,
                materials=len(obj.data.materials))


def import_svg(path):
    clear()
    assert bpy.ops.wm.grease_pencil_import_svg(filepath=str(path),resolution=12,scale=10)=={'FINISHED'}
    return bpy.context.object


def trace_image(path):
    clear()
    image=bpy.data.images.load(str(path),check_existing=False)
    empty=bpy.data.objects.new('Trace image',None)
    empty.empty_display_type='IMAGE';empty.data=image
    bpy.context.collection.objects.link(empty)
    bpy.context.view_layer.objects.active=empty;empty.select_set(True)
    assert bpy.ops.grease_pencil.trace_image(threshold=.5,mode='SINGLE',target='NEW')=={'FINISHED'}
    traced=next(obj for obj in bpy.context.scene.objects if obj.type=='GREASEPENCIL')
    return gp_snapshot(traced)


def fixtures(directory):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    (directory/'shapes.svg').write_text(SVG)
    images=write_images(directory)
    svg=gp_snapshot(import_svg(directory/'shapes.svg'))
    traced=trace_image(directory/'trace.png')
    report=dict(images=images,svg=svg,traced=traced,
        blenderVersion=bpy.app.version_string,sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (directory/'reference.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def maximum_error(actual,expected):
    assert len(actual)==len(expected)
    return max(abs(a-b) for a,b in zip(actual,expected))


def geometry_error(actual,expected):
    assert actual['curves']==expected['curves'] and len(actual['points'])==len(expected['points'])
    return maximum_error([value for point in actual['points'] for value in point],
                         [value for point in expected['points'] for value in point])


def run(directory):
    directory=Path(directory);ref=json.loads((directory/'reference.json').read_text())
    input_errors={}
    for fmt,ext,_,_ in FORMATS:
        os.write(1,('IMAGE_VECTOR_READ:'+fmt+'\n').encode())
        image=bpy.data.images.load(str(directory/('pattern.'+ext)),check_existing=False)
        input_errors[fmt]=maximum_error(image_values(image),ref['images'][fmt]['pixels'])
        bpy.data.images.remove(image)
        assert input_errors[fmt]<1e-4,(fmt,input_errors[fmt])
    os.write(1,b'IMAGE_VECTOR_WRITE_FORMATS\n')
    written=write_images(directory)
    result={'images':{}}
    for fmt,ext,_,_ in FORMATS:
        err=maximum_error(written[fmt]['pixels'],ref['images'][fmt]['pixels'])
        assert err<1e-4,(fmt,err)
        result['images'][fmt]=dict(bytes=written[fmt]['bytes'],maximumDesktopPixelError=err,
                                  desktopInputPixelError=input_errors[fmt])
    os.write(1,b'IMAGE_VECTOR_IMPORT_SVG\n')
    svg=gp_snapshot(import_svg(directory/'shapes.svg'))
    result['svg']={**svg,'maximumDesktopCoordinateError':geometry_error(svg,ref['svg'])}
    assert result['svg']['maximumDesktopCoordinateError']<2e-5
    os.write(1,b'IMAGE_VECTOR_TRACE\n')
    traced=trace_image(directory/'trace.png')
    result['trace']={**traced,'maximumDesktopCoordinateError':geometry_error(traced,ref['traced'])}
    assert result['trace']['maximumDesktopCoordinateError']<2e-5
    obj=import_svg(directory/'shapes.svg')
    bpy.context.view_layer.objects.active=obj
    for selected in bpy.context.selected_objects:selected.select_set(False)
    obj.select_set(True)
    # Export projection uses the actual running 3D View; desktop verifies the
    # exported SVG by importing that file, rather than assuming matching UI size.
    for ext in ('svg','pdf'):
        os.write(1,('IMAGE_VECTOR_EXPORT:'+ext+'\n').encode())
        path=directory/('export.'+ext)
        operator=getattr(bpy.ops.wm,'grease_pencil_export_'+ext)
        assert operator(filepath=str(path),selected_object_type='ACTIVE',use_fill=True)=={'FINISHED'}
        data=path.read_bytes();assert len(data)>100
        if ext=='pdf':assert data.startswith(b'%PDF-') and b'%%EOF' in data
        else:
            import xml.etree.ElementTree as ET
            tree=ET.fromstring(data);assert tree.tag.endswith('svg')
            assert any(element.tag.endswith(('path','polygon','polyline')) for element in tree.iter())
            assert b'nan' not in data.lower() and b'inf' not in data.lower()
        result['export'+ext.upper()]=dict(bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
    result['svgRoundTrip']=gp_snapshot(import_svg(directory/'export.svg'))
    assert max(point[0] for point in result['svgRoundTrip']['points'])-min(
        point[0] for point in result['svgRoundTrip']['points'])>1e-4
    import_svg(directory/'shapes.svg')
    return result


def browser_check(directory,marker='IMAGE_VECTOR',draw=True):
    spare=next(area for area in bpy.context.screen.areas if area.type=='VIEW_3D')
    # The original Python console replaces the main 3D View. Export operators
    # still need a real View3D region while that console is active.
    try:
        report=run(directory)
        if not draw:
            bpy.context.object.hide_set(True)
        Path(directory,'results.json').write_text(json.dumps(report,indent=2)+'\n')
        summary=json.loads(json.dumps(report))
        for key in ('svg','trace','svgRoundTrip'):summary[key].pop('points')
        os.write(1,(marker+'_DONE:'+json.dumps(summary)+'\n').encode())
    except Exception:
        os.write(2,traceback.format_exc().encode());os.write(1,(marker+'_FAILED\n').encode())
    finally:
        spare.type='PROPERTIES'
        os.write(1,(marker+'_FINISHED:\n').encode())
