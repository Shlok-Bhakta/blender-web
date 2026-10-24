# SPDX-License-Identifier: GPL-2.0-or-later
import array
import gc
import json
import math
import _bpy_ghost_web_gltf as bridge


def check():
    lib = bridge.CDLL(bridge.__file__)
    rejected = []

    def rejects(name, function):
        try:
            function()
        except (ValueError, TypeError, BufferError, OverflowError):
            rejected.append(name)
        else:
            raise AssertionError('Accepted invalid glTF input: ' + name)

    indices = array.array('I', [0, 1, 2, 2, 1, 3])
    source = array.array('f', [0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0])
    for sequence in (False, True):
        suffix = 'IndexSequence' if sequence else 'IndexBuffer'
        bound = getattr(lib, 'encode' + suffix + 'Bound')(6, 4)
        encoded = bytearray(bound)
        written = getattr(lib, 'encode' + suffix)(encoded, bound, indices, 6)
        decoded = array.array('I', [0] * 6)
        assert getattr(lib, 'decode' + suffix)(decoded, 6, 4, encoded, written) == 0
        assert decoded == indices
    for version in (0, 1):
        lib.encodeVertexVersion(version)
        bound = lib.encodeVertexBufferBound(4, 12)
        encoded = bytearray(bound)
        size = lib.encodeVertexBuffer(encoded, bound, source, 4, 12)
        decoded = bytearray(48)
        assert lib.decodeVertexBuffer(decoded, 4, 12, encoded, size) == 0
        assert decoded == source.tobytes()
        assert lib.decodeVertexBuffer(decoded, 4, 12, encoded, 1) != 0
    filters = [('Oct', 4, 8, array.array('f', [0, 0, 1, 0] * 4)),
               ('Quat', 8, 8, array.array('f', [0, 0, 0, 1] * 4)),
               ('Exp', 12, 12, source),
               ('Color', 4, 8, array.array('f', [1, 0, 0, 1] * 4))]
    for kind, stride, bits, values in filters:
        output = bytearray(4 * stride)
        args = (output, 4, stride, bits, values) + ((1,) if kind == 'Exp' else ())
        getattr(lib, 'encodeFilter' + kind)(*args)
        getattr(lib, 'decodeFilter' + kind)(output, 4, stride)
        if kind == 'Color':
            assert max(abs(a-b) for a,b in zip(output, bytes([255, 0, 0, 255] * 4))) <= 2
        if kind == 'Exp':
            decoded = array.array('f')
            decoded.frombytes(output)
            assert max(abs(a - b) for a, b in zip(decoded, source)) < .001

    # Compatibility with the pinned desktop sampled-object exporter's layout.
    canonical = bytearray(32)
    lib.encodeFilterQuat(canonical, 4, 8, 8, array.array('f', [0, 0, 0, 1] * 4))
    legacy = canonical + bytearray(32)
    lib.decodeFilterQuat(canonical, 4, 8)
    lib.decodeFilterQuat(legacy, 4, 16)
    assert legacy[:32] == canonical and legacy[32:] == bytes(32)

    for sequential in (0, 1):
        handle = lib.encoderCreate(4)
        lib.encoderSetCompressionLevel(handle, 6)
        lib.encoderSetQuantizationBits(handle, 14, 10, 12, 10, 12)
        attribute = lib.encoderSetAttribute(handle, b'POSITION', 5126, b'VEC3', source, None)
        lib.encoderSetIndices(handle, 5125, 6, indices)
        assert lib.encoderEncode(handle, sequential)
        data = lib.encoderBytes(handle)
        lib.encoderRelease(handle)
        rejects('released encoder', lambda: lib.encoderBytes(handle))
        rejects('double release', lambda: lib.encoderRelease(handle))
        decoder = lib.decoderCreate()
        assert lib.decoderDecode(decoder, data, len(data))
        assert lib.decoderGetVertexCount(decoder) == 4
        assert lib.decoderGetIndexCount(decoder) == 6
        assert lib.decoderReadAttribute(decoder, attribute, 5126, b'VEC3')
        assert len(lib.decoderAttribute(decoder, attribute)) == 48
        assert lib.decoderReadIndices(decoder, 5125)
        assert len(lib.decoderIndices(decoder)) == 24
        lib.decoderRelease(decoder)
    decoder = lib.decoderCreate()
    assert not lib.decoderDecode(decoder, b'bad draco', 9)
    lib.decoderRelease(decoder)
    rejects('short source', lambda: lib.decodeVertexBuffer(bytearray(48), 4, 12, b'x', 4))
    rejects('short destination', lambda: lib.decodeVertexBuffer(bytearray(1), 4, 12, b'x', 1))
    rejects('read-only output', lambda: lib.decodeVertexBuffer(bytes(48), 4, 12, b'x', 1))
    rejects('vertex stride', lambda: lib.decodeVertexBuffer(bytearray(48), 4, 3, b'x', 1))
    rejects('triangle count', lambda: lib.decodeIndexBuffer(bytearray(16), 4, 4, b'x', 1))
    rejects('index stride', lambda: lib.decodeIndexSequence(bytearray(24), 6, 1, b'x', 1))
    rejects('negative count', lambda: lib.decodeVertexBuffer(bytearray(48), -4, 12, b'x', 1))
    rejects('invalid version', lambda: lib.encodeVertexVersion(2))
    rejects('integer pointer', lambda: lib.decodeVertexBuffer(123, 4, 12, b'x', 1))
    rejects('invalid filter', lambda: lib.decodeFilterQuat(bytearray(48), 4, 12))
    rejects('invalid filter precision', lambda: lib.encodeFilterQuat(bytearray(32), 4, 8, 1, array.array('f', [0]*16)))
    rejects('unknown library', lambda: bridge.CDLL('/unknown.so'))
    # Capsule destructors must also reclaim codecs when an import raises early.
    for _ in range(100):
        lib.encoderCreate(4)
        lib.decoderCreate()
    gc.collect()
    return {'draco': True, 'meshoptVertexVersions': [0, 1],
            'indexModes': ['TRIANGLES', 'INDICES'], 'filters': ['Oct', 'Quat', 'Exp', 'Color'],
            'rejectedCases': rejected, 'truncatedDataRejected': True, 'capsuleCleanup': True, 'legacyQuaternionStride': True}


def addon_color_check():
    from types import SimpleNamespace
    from io_scene_gltf2.io.imp.gltf2_io_binary_meshopt import MeshoptDecoder
    lib = bridge.CDLL(bridge.__file__)
    colors = array.array('f', [1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1])
    filtered = bytearray(16)
    lib.encodeFilterColor(filtered, 4, 4, 8, colors)
    encoded = bytearray(lib.encodeVertexBufferBound(4, 4))
    lib.encodeVertexVersion(1)
    length = lib.encodeVertexBuffer(encoded, len(encoded), filtered, 4, 4)
    ext = dict(buffer=0, byteLength=length, byteStride=4, count=4, mode='ATTRIBUTES', filter='COLOR')
    view = SimpleNamespace(extensions={'KHR_meshopt_compression': ext})
    gltf = SimpleNamespace(data=SimpleNamespace(buffer_views=[view]), buffers={0: encoded[:length]})
    actual = MeshoptDecoder.get_buffer_view(gltf, 0)
    expected = [255 if c else 0 for c in colors]
    maximum = max(abs(a-b) for a,b in zip(actual,expected))
    assert maximum <= 2, (list(actual),expected)
    assert MeshoptDecoder.get_buffer_view(gltf, 0) is actual
    return {'knownColors':4, 'maximumUnorm8Error':maximum, 'cachedBufferReused':True}


if __name__ == '__main__':
    print(json.dumps(check()))
