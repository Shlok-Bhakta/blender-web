# SPDX-License-Identifier: GPL-2.0-or-later
"""blender -b --factory-startup --python-exit-code 1 --python this.py -- evidence"""
import bpy
import hashlib
import json
from pathlib import Path
import runpy
import re
import subprocess
import sys

base=Path(sys.argv[sys.argv.index('--')+1]).resolve()
checks=runpy.run_path(str(Path(__file__).with_name('image_vector.py')),run_name='checks')
expected=json.loads((base/'results.json').read_text())
reference=json.loads((Path(__file__).parent/'fixtures/image-vector/reference.json').read_text())
errors={}
for fmt,ext,_,_ in checks['FORMATS']:
    image=bpy.data.images.load(str(base/('pattern.'+ext)),check_existing=False)
    error=checks['maximum_error'](checks['image_values'](image),reference['images'][fmt]['pixels'])
    assert error<1e-4,(fmt,error)
    errors[fmt]=error
actual=checks['gp_snapshot'](checks['import_svg'](base/'export.svg'))
svg_error=checks['geometry_error'](actual,expected['svgRoundTrip'])
assert svg_error<2e-5
pdf_info=subprocess.check_output(['pdfinfo',str(base/'export.pdf')],text=True)
assert re.search(r'^Pages:\s+1\s*$',pdf_info,re.MULTILINE),pdf_info
subprocess.run(['pdftoppm','-scale-to','512','-png','-singlefile',str(base/'export.pdf'),
                str(base/'pdf-preview')],check=True)
preview=bpy.data.images.load(str(base/'pdf-preview.png'),check_existing=False)
pixels=list(preview.pixels)
blue=sum(pixels[i+2]>.2 and pixels[i+1]>.1 and pixels[i+2]>pixels[i]*1.2
         for i in range(0,len(pixels),4))
assert blue>100,'PDF must rasterize with the authored blue artwork'
report=dict(blenderVersion=bpy.app.version_string,imagePixelErrors=errors,
            svgMaximumCoordinateError=svg_error,svgCurves=actual['curves'],
            pdfPages=1,pdfRasterBluePixels=blue,
            svgSha256=hashlib.sha256((base/'export.svg').read_bytes()).hexdigest(),
            pdfSha256=hashlib.sha256((base/'export.pdf').read_bytes()).hexdigest())
(base/'desktop-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('IMAGE_VECTOR_DESKTOP_VERIFIED:'+json.dumps(report))
