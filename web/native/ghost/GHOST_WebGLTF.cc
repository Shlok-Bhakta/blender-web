/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_WebGLTF.hh"
#include <Python.h>
#include <meshoptimizer.h>
#include <limits>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include "GHOST_WebDracoTypes.hh"
#include "intern/draco_bridge/intern/encoder.h"
#include "intern/draco_bridge/intern/decoder.h"
#include "intern/meshoptimizer_bridge/intern/encoder.h"
#include "intern/meshoptimizer_bridge/intern/decoder.h"

namespace {
/* The pinned desktop add-on maps COLOR to EXPONENTIAL. Expose the actual
 * meshoptimizer color API through the guarded web decoder hook. */
void decode_color(void *buffer, size_t count, size_t stride)
{
  meshopt_decodeFilterColor(buffer, count, stride);
}
void encode_color(void *buffer, size_t count, size_t stride, int bits, const float *data)
{
  meshopt_encodeFilterColor(buffer, count, stride, bits, data);
}
/* Fixed, compiled calls only. No dlopen, libffi, or integer-to-pointer conversion.
 * Buffer exports live until the original bridge function returns. */
template<typename T> struct Argument {
  T value{};
  Py_buffer buffer{};
  ~Argument() { if (buffer.obj) PyBuffer_Release(&buffer); }
  bool read(PyObject *object)
  {
    if constexpr (std::is_same_v<T, Encoder *> || std::is_same_v<T, Decoder *>) {
      value = static_cast<T>(PyCapsule_GetPointer(object,
          std::is_same_v<T, Encoder *> ? "gltf.Encoder" : "gltf.Decoder"));
      return value != nullptr;
    }
    else if constexpr (std::is_pointer_v<T>) {
      if (PyObject_GetBuffer(object, &buffer, PyBUF_CONTIG_RO)) return false;
      value = static_cast<T>(buffer.buf);
      return true;
    }
    else {
      const auto integer = PyLong_AsLongLong(object);
      if (PyErr_Occurred()) return false;
      if (integer < 0 || static_cast<unsigned long long>(integer) >
          static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
        PyErr_SetString(PyExc_ValueError, "glTF bridge integer out of range");
        return false;
      }
      value = static_cast<T>(integer);
      return true;
    }
  }
};
template<> bool Argument<encodeExpMode>::read(PyObject *object)
{
  const long n = PyLong_AsLong(object);
  if (PyErr_Occurred()) return false;
  if (n < 0 || n > 3) { PyErr_SetString(PyExc_ValueError, "Invalid exponent filter mode"); return false; }
  value = static_cast<encodeExpMode>(n); return true;
}
void release_encoder(PyObject *capsule)
{
  encoderRelease(static_cast<Encoder *>(PyCapsule_GetPointer(capsule, "gltf.Encoder")));
}
void release_decoder(PyObject *capsule)
{
  decoderRelease(static_cast<Decoder *>(PyCapsule_GetPointer(capsule, "gltf.Decoder")));
}
template<typename R, typename... A, size_t... I>
PyObject *invoke(R (*function)(A...), PyObject *args, std::index_sequence<I...>)
{
  if (PyTuple_GET_SIZE(args) != sizeof...(A)) {
    PyErr_SetString(PyExc_TypeError, "Incorrect glTF bridge argument count"); return nullptr;
  }
  std::tuple<Argument<A>...> converted;
  if (!(std::get<I>(converted).read(PyTuple_GET_ITEM(args, I)) && ...)) return nullptr;
  if constexpr (std::is_void_v<R>) {
    function(std::get<I>(converted).value...); Py_RETURN_NONE;
  }
  else {
    R result = function(std::get<I>(converted).value...);
    if constexpr (std::is_same_v<R, Encoder *>) {
      PyObject *capsule = PyCapsule_New(result, "gltf.Encoder", release_encoder);
      if (!capsule) { encoderRelease(result); return nullptr; }
      PyCapsule_SetContext(capsule, reinterpret_cast<void *>(
          uintptr_t(std::get<0>(converted).value) + 1));
      return capsule;
    }
    else if constexpr (std::is_same_v<R, Decoder *>)
      return PyCapsule_New(result, "gltf.Decoder", release_decoder);
    else if constexpr (std::is_same_v<R, bool>) return PyBool_FromLong(result);
    else if constexpr (std::is_signed_v<R>) return PyLong_FromLongLong(result);
    else return PyLong_FromUnsignedLongLong(result);
  }
}
template<auto Function> PyObject *call(PyObject *, PyObject *args)
{
  return []<typename R, typename... A>(R (*f)(A...), PyObject *a) {
    return invoke(f, a, std::index_sequence_for<A...>{});
  }(Function, args);
}
/* Release also invalidates the capsule, preventing a second release or reuse. */
template<typename T> PyObject *release(PyObject *, PyObject *object)
{
  const char *name = std::is_same_v<T, Encoder> ? "gltf.Encoder" : "gltf.Decoder";
  T *value = static_cast<T *>(PyCapsule_GetPointer(object, name));
  if (!value) return nullptr;
  if constexpr (std::is_same_v<T, Encoder>) encoderRelease(value);
  else decoderRelease(value);
  PyCapsule_SetDestructor(object, nullptr);
  PyCapsule_SetName(object, "gltf.released");
  Py_RETURN_NONE;
}
PyObject *new_bytes(size_t size)
{
  if (size > PY_SSIZE_T_MAX) return PyErr_NoMemory();
  return PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(size));
}
PyObject *encoder_vertex_count(PyObject *, PyObject *object)
{
  if (!PyCapsule_GetPointer(object, "gltf.Encoder")) return nullptr;
  return PyLong_FromSize_t(reinterpret_cast<uintptr_t>(PyCapsule_GetContext(object)) - 1);
}
PyObject *encoder_bytes(PyObject *, PyObject *object)
{
  auto *encoder = static_cast<Encoder *>(PyCapsule_GetPointer(object, "gltf.Encoder"));
  if (!encoder) return nullptr;
  PyObject *result = new_bytes(encoderGetByteLength(encoder));
  if (result) encoderCopy(encoder, reinterpret_cast<uint8_t *>(PyBytes_AS_STRING(result)));
  return result;
}
PyObject *decoder_indices(PyObject *, PyObject *object)
{
  auto *decoder = static_cast<Decoder *>(PyCapsule_GetPointer(object, "gltf.Decoder"));
  if (!decoder) return nullptr;
  PyObject *result = new_bytes(decoderGetIndicesByteLength(decoder));
  if (result) decoderCopyIndices(decoder, PyBytes_AS_STRING(result));
  return result;
}
PyObject *decoder_attribute(PyObject *, PyObject *args)
{
  PyObject *object; unsigned int id;
  if (!PyArg_ParseTuple(args, "OI", &object, &id)) return nullptr;
  auto *decoder = static_cast<Decoder *>(PyCapsule_GetPointer(object, "gltf.Decoder"));
  if (!decoder) return nullptr;
  PyObject *result = new_bytes(decoderGetAttributeByteLength(decoder, id));
  if (result) decoderCopyAttribute(decoder, id, PyBytes_AS_STRING(result));
  return result;
}
#define METHOD(name) {#name, call<name>, METH_VARARGS, nullptr}
PyMethodDef methods[] = {
  METHOD(encoderCreate), METHOD(encoderSetCompressionLevel), METHOD(encoderSetQuantizationBits),
  METHOD(encoderSetIndices), METHOD(encoderSetAttribute), METHOD(encoderEncode),
  METHOD(encoderGetEncodedVertexCount), METHOD(encoderGetEncodedIndexCount), METHOD(encoderGetByteLength),
  METHOD(decoderCreate), METHOD(decoderDecode), METHOD(decoderReadAttribute),
  METHOD(decoderGetVertexCount), METHOD(decoderGetIndexCount), METHOD(decoderAttributeIsNormalized),
  METHOD(decoderGetAttributeByteLength), METHOD(decoderReadIndices), METHOD(decoderGetIndicesByteLength),
  METHOD(encodeIndexVersion), METHOD(encodeVertexVersion), METHOD(encodeIndexBufferBound),
  METHOD(encodeIndexBuffer), METHOD(encodeVertexBufferBound), METHOD(encodeVertexBuffer),
  METHOD(encodeIndexSequenceBound), METHOD(encodeIndexSequence), METHOD(encodeFilterOct),
  METHOD(encodeFilterQuat), METHOD(encodeFilterExp), METHOD(decodeVertexBuffer),
  METHOD(decodeIndexBuffer), METHOD(decodeIndexSequence), METHOD(decodeFilterOct),
  METHOD(decodeFilterQuat), METHOD(decodeFilterExp),
  {"decodeFilterColor", call<decode_color>, METH_VARARGS, nullptr},
  {"encodeFilterColor", call<encode_color>, METH_VARARGS, nullptr},
  {"encoderRelease", release<Encoder>, METH_O, nullptr},
  {"decoderRelease", release<Decoder>, METH_O, nullptr},
  {"encoderBytes", encoder_bytes, METH_O, nullptr},
  {"encoderVertexCount", encoder_vertex_count, METH_O, nullptr},
  {"decoderIndices", decoder_indices, METH_O, nullptr},
  {"decoderAttribute", decoder_attribute, METH_VARARGS, nullptr},
  {nullptr, nullptr, 0, nullptr}
};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "_blender_web_gltf", nullptr, -1, methods};
PyObject *initialize() { return PyModule_Create(&module); }
}
void GHOST_web_register_gltf()
{
  if (PyImport_AppendInittab("_blender_web_gltf", initialize) == -1) {
    fputs("Cannot register static glTF bridges\n", stderr);
    abort();
  }
}
