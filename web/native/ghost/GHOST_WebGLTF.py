# SPDX-License-Identifier: GPL-2.0-or-later
"""glTF's fixed desktop bridge API backed by compiled Python calls on the web.

This is local to the glTF add-on. It does not install or emulate ctypes globally.
All array arguments export Python buffers. Integer addresses are never accepted.
"""
import _blender_web_gltf as _native
from pathlib import Path


class _Type:
    def __mul__(self, length):
        if length < 0:
            raise ValueError('Negative glTF buffer size')

        class Array:
            def __new__(cls):
                return bytearray(length)

            @staticmethod
            def from_buffer(value):
                view = memoryview(value).cast('B')
                if view.readonly or len(view) < length:
                    raise ValueError('glTF output buffer is read-only or too small')
                return view[:length]

            @staticmethod
            def from_buffer_copy(value):
                view = memoryview(value).cast('B')
                if len(view) < length:
                    raise ValueError('glTF input buffer is too small')
                # This scoped API is only used for const decoder inputs.
                # Keep the source alive through its buffer export and avoid a copy.
                return view[:length]

        return Array


c_void_p = c_size_t = c_int = c_uint8 = c_uint32 = c_uint64 = c_char_p = c_bool = c_ubyte = _Type()


def POINTER(value):
    return value


def _buffer(value, size, writable=False):
    view = memoryview(value)
    if not view.c_contiguous or view.nbytes < size or (writable and view.readonly):
        raise ValueError('Invalid glTF bridge buffer length, layout, or write access')


class _Function:
    def __init__(self, name):
        self.name = name
        self.restype = self.argtypes = None

    def __call__(self, *args):
        name = self.name
        # The native codec APIs assert on invalid shapes. Reject bad glTF metadata
        # before calling them so a corrupt import cannot terminate Blender.
        if name in {'encodeIndexVersion', 'encodeVertexVersion'}:
            if args[0] not in (0, 1):
                raise ValueError('Unsupported meshoptimizer format version')
        elif name in {'decodeVertexBuffer', 'decodeIndexBuffer', 'decodeIndexSequence'}:
            destination, count, stride, source, size = args
            if count < 0 or stride <= 0 or size < 0:
                raise ValueError('Invalid compressed buffer dimensions')
            if name == 'decodeVertexBuffer':
                if stride > 256 or stride % 4:
                    raise ValueError('Invalid meshoptimizer vertex stride')
            elif stride not in (2, 4):
                raise ValueError('Invalid meshoptimizer index stride')
            if name == 'decodeIndexBuffer' and count % 3:
                raise ValueError('Triangle index count must be divisible by three')
            _buffer(destination, count * stride, True)
            _buffer(source, size)
        elif name in {'encodeIndexBuffer', 'encodeIndexSequence'}:
            destination, size, source, count = args
            if count < 0 or (name == 'encodeIndexBuffer' and count % 3):
                raise ValueError('Invalid meshoptimizer index count')
            _buffer(destination, size, True)
            _buffer(source, count * 4)
        elif name in {'encodeVertexBuffer', 'encodeVertexBufferBound'}:
            count, stride = args[-2:]
            if count < 0 or stride <= 0 or stride > 256 or stride % 4:
                raise ValueError('Invalid meshoptimizer vertex dimensions')
            if name == 'encodeVertexBuffer':
                _buffer(args[0], args[1], True)
                _buffer(args[2], count * stride)
        elif name.startswith(('encodeFilter', 'decodeFilter')):
            destination, count, stride = args[:3]
            kind = name[-3:]
            if count < 0 or stride <= 0 or stride > 256 or stride % 4:
                raise ValueError('Invalid meshoptimizer filter dimensions')
            if kind in ('Oct', 'lor') and stride not in (4, 8):
                raise ValueError('Invalid octahedral filter stride')
            if name == 'decodeFilterQuat' and stride == 16:
                # Blender 5.2's sampled-object exporter advertises 16-byte
                # vertices but packs all quaternion shorts into the first half.
                # Its decoder ignores that stride. Preserve this bounded legacy
                # layout while passing the codec its required eight-byte stride.
                _buffer(destination, count * stride, True)
                return _native.decodeFilterQuat(destination, count, 8)
            if kind == 'uat' and stride != 8:
                raise ValueError('Invalid quaternion filter stride')
            _buffer(destination, count * stride, True)
            if name.startswith('encode'):
                bits, source = args[3:5]
                if not ((1 if kind == 'Exp' else 4 if kind == 'uat' else 2) <= bits <= (24 if kind == 'Exp' else 16)):
                    raise ValueError('Invalid meshoptimizer filter precision')
                _buffer(source, count * (16 if kind in ('Oct', 'uat', 'lor') else stride))
        elif name == 'encoderSetIndices':
            handle, component, count, source = args
            width = {5121: 1, 5123: 2, 5125: 4}.get(component)
            if width is None or count < 0 or count % 3:
                raise ValueError('Invalid Draco triangle indices')
            _buffer(source, count * width)
        elif name == 'encoderSetAttribute':
            handle, semantic, component, kind, source, normalized = args
            # glTF represents an omitted normalized flag as None; ctypes c_bool
            # converts it to false before the desktop bridge call.
            args = (*args[:-1], bool(normalized))
            width = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}.get(component)
            components = {b'SCALAR': 1, b'VEC2': 2, b'VEC3': 3, b'VEC4': 4}.get(kind)
            if width is None or components is None or not isinstance(semantic, bytes) or b'\0' in semantic:
                raise ValueError('Invalid Draco attribute metadata')
            _buffer(source, _native.encoderVertexCount(handle) * width * components)
        elif name == 'decoderReadAttribute':
            if args[2] not in (5120, 5121, 5122, 5123, 5125, 5126) or args[3] not in (b'SCALAR', b'VEC2', b'VEC3', b'VEC4'):
                raise ValueError('Invalid Draco accessor metadata')
        elif name == 'decoderReadIndices':
            if args[1] not in (5121, 5123, 5125):
                raise ValueError('Invalid Draco index component type')
        elif name == 'decoderDecode':
            _buffer(args[1], args[2])
        elif name == 'encoderSetCompressionLevel' and not 0 <= args[1] <= 10:
            raise ValueError('Invalid Draco compression level')
        elif name == 'encoderSetQuantizationBits' and any(not 0 <= bit <= 30 for bit in args[1:]):
            raise ValueError('Invalid Draco quantization')
        return getattr(_native, name)(*args)


class _Library:
    def __getattr__(self, name):
        # Old copy signatures are assigned by upstream, but the web hooks below
        # use immutable byte-returning functions instead of writing into bytes.
        if not hasattr(_native, name) and name not in {
            'encoderCopy', 'decoderCopyAttribute', 'decoderCopyIndices'}:
            raise AttributeError(name)
        function = _Function(name)
        setattr(self, name, function)
        return function


_library = _Library()


def dll_path(lib_name, lib_display_name):
    if lib_name not in {'bf_intern_draco_bridge', 'bf_intern_meshopt_bridge'}:
        raise ValueError('Unknown static glTF library')
    return Path(__file__)


def CDLL(path):
    if Path(path).resolve() != Path(__file__).resolve():
        raise ValueError('Only compiled glTF libraries are available')
    return _library


class _Loader:
    LoadLibrary = staticmethod(CDLL)


cdll = _Loader()


def meshopt_stride(attribute, stride):
    """Quaternion filters pack four signed shorts, including sampled objects."""
    return 8 if attribute in {'ROTATION', 'SK_ROTATION', 'GPU_ROTATION'} else stride


def meshopt_fallback_data(binary_data, extension):
    """Match the placeholder buffer view to its decoded, quantized dimensions."""
    ext = getattr(binary_data, 'extensions', {}).get(extension)
    if ext is None:
        return binary_data
    length = ext['count'] * ext['byteStride']
    if binary_data.byte_length == length:
        return binary_data
    # The add-on retains original float data while changing the accessor to
    # normalized shorts. This buffer is only a placeholder for required meshopt
    # decoding, and is not written to the GLB. Keep the cached input immutable.
    result = type(binary_data)(bytes(length), binary_data.bufferViewTarget)
    result.extensions = binary_data.extensions
    return result
