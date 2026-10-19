Generated with official Blender 5.2.2 LTS, revision d13f752e3b9c.
These are authored test scenes, not third-party production assets.

desktop-ocean.abc: 12-frame native Ocean modifier, resolution 5, seed 7,
UVs, vertex colors, normals and animated deformation. Ogawa format.
desktop-hierarchy.abc: keyframed parent transform and an offset cube.
reference.json: evaluated world-space vertices and UV coordinates at frames
1, 6 and 12, plus topology counts and foam presence before export.

Regenerate from the repository root with the pinned desktop Blender:
blender -b --factory-startup --python-exit-code 1 --python-expr "import runpy; from pathlib import Path; m=runpy.run_path('web/tests/ocean_alembic.py',run_name='fixture'); m['desktop_fixture'](Path('web/tests/fixtures/ocean-alembic'))"

The generator first reads its exported files back in desktop Blender and checks
their samples against the originals. The browser check imports these fixtures,
compares all samples, then exports its own native ocean animation. Verify that
browser export independently using web/tests/verify_ocean_desktop.py.
