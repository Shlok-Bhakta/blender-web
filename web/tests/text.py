# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise Blender's original catalogs and software font drawing in both builds."""
import hashlib
import json
from pathlib import Path


def check(directory):
    import bpy
    import blf
    import imbuf
    output = Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    view = bpy.context.preferences.view
    original = view.language
    view.use_translate_interface = True
    strings = ['File', 'Edit', 'Render', 'Window', 'Help', 'Save', 'Open', 'Language']
    translations = {}
    states = {}
    try:
        for language in ['en_US', 'fr_FR', 'es', 'ja_JP', 'zh_HANS', 'ru_RU']:
            view.language = language
            translations[language] = {text:bpy.app.translations.pgettext_iface(text) for text in strings}
            states[language] = dict(locale=bpy.app.translations.locale,
                interface=view.use_translate_interface, directFile=bpy.app.translations.pgettext('File'))
        assert translations['en_US']['File'] == 'File'
        assert translations['fr_FR']['File'] == 'Fichier', (translations, states)
        assert all(translations[lang]['File'] != 'File' for lang in translations if lang != 'en_US')
    finally:
        view.language = original
    fonts = Path(bpy.utils.system_resource('DATAFILES', path='fonts'))
    locale = Path(bpy.utils.system_resource('DATAFILES', path='locale'))
    catalogs = {str(path.relative_to(locale)):dict(bytes=path.stat().st_size,
        sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for path in sorted(locale.rglob('*.mo'))}
    assert len(catalogs) == 49
    samples = [
        ('Inter.woff2', 'Café déjà vu – Ångström'),
        ('Noto Sans CJK Regular.woff2', '日本語 简体中文'),
        ('NotoSansDevanagari-Regular.woff2', 'कर्म हिन्दी'),
        ('NotoSansArabic-VariableFont_wdth,wght.woff2', 'العربية'),
    ]
    image = imbuf.new((640, 280))
    metrics = []
    for index, (filename, text) in enumerate(samples):
        path = fonts / filename
        font = blf.load(str(path))
        assert font >= 0
        try:
            blf.size(font, 30)
            blf.color(font, 1, 1, 1, 1)
            blf.position(font, 15, 230 - 60 * index, 0)
            dimensions = list(blf.dimensions(font, text))
            assert dimensions[0] > 10 and dimensions[1] > 10
            with blf.bind_imbuf(font, image):
                blf.draw_buffer(font, text)
            metrics.append(dict(font=filename, text=text, dimensions=dimensions,
                fontSha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        finally:
            blf.unload(str(path))
    imbuf.write(image, filepath=str(output / 'font-samples.png'))
    result = dict(blenderVersion=bpy.app.version_string,
        buildOptions={name:getattr(bpy.app.build_options, name) for name in dir(bpy.app.build_options)
                      if not name.startswith('_') and isinstance(getattr(bpy.app.build_options, name), bool)},
        translations=translations, translationStates=states, fonts=metrics, catalogs=catalogs,
        availableLanguages=sorted(item.identifier for item in view.bl_rna.properties['language'].enum_items
                                  if item.identifier),
        sourceSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (output / 'results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    return result


def browser_check(directory):
    try:
        return check(directory)
    except BaseException:
        import os
        import traceback
        os.write(1, ('TEXT_FAILED:' + traceback.format_exc() + '\n').encode())
        raise


if __name__ == '__main__':
    import sys
    if '--' in sys.argv:
        result = check(sys.argv[sys.argv.index('--') + 1])
        print('TEXT_COMPLETE:' + json.dumps(result, ensure_ascii=False))
