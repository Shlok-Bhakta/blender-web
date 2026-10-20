Generated with native Blender 5.2.2 LTS, revision d13f752e3b9c,
using web/tests/usd_exchange.py and the release's USD/MaterialX libraries.

Regenerate from the repository root:
  blender --background --factory-startup --python-exit-code 1 \
    --python web/tests/usd_exchange.py -- fixtures web/tests/fixtures/usd-exchange

The three files contain the same animated cube, UVs, blue Principled material,
camera, point light and constant EXR world texture. They include USD Preview
Surface and MaterialX OpenPBR networks, plus metadata written by an original
Blender USDHook. The USDZ contains the texture inside the package.

reference.json records native imports at frames 1 and 10, including geometry,
UVs, material inputs, transforms, camera/light settings and decoded world
texture pixels. It also records shader identifiers, resolved assets and the
test script's SHA-256. These small fixtures test scene exchange, not capacity
or performance on production USD stages.
