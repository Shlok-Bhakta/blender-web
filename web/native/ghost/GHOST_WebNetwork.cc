/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <cstdlib>

namespace {
struct Response {
  char *metadata;
  unsigned char *body;
  size_t size;
};
static_assert(sizeof(void *) == 4 && sizeof(Response) == 12,
              "The HTTP result layout must be ported before a memory64 build");
EM_JS_DEPS(network, "$UTF8ToString,$stringToNewUTF8,malloc,free");
EM_JS(void, request_http,
      (const char *method, const char *url, const char *headers,
       const unsigned char *data, size_t size, double timeout, Response *result), {
  let bytes = null, metadata;
  try {
    if (typeof XMLHttpRequest === 'undefined') {
      throw new Error('Browser HTTP transport is unavailable in this runtime');
    }
    const target = new URL(UTF8ToString(url));
    if (!['http:', 'https:'].includes(target.protocol)) throw new Error('Expected an HTTP or HTTPS URL');
    const xhr = new XMLHttpRequest();
    // Blender's interpreter runs on a dedicated pthread. Synchronous XHR can
    // block that worker without suspending a CPython callback or browser UI.
    xhr.open(UTF8ToString(method), target.href, false);
    xhr.responseType = 'arraybuffer';
    if (timeout > 0) xhr.timeout = Math.min(2147483647, Math.ceil(timeout * 1000));
    const browserHeaders = new Set(['user-agent', 'host', 'content-length',
      'accept-encoding', 'connection', 'cookie', 'origin', 'referer']);
    for (const [name, value] of Object.entries(JSON.parse(UTF8ToString(headers)))) {
      if (!browserHeaders.has(name.toLowerCase())) xhr.setRequestHeader(name, value);
    }
    xhr.send(data ? HEAPU8.slice(data, data + size) : null);
    if (!xhr.status) throw new Error('Browser blocked the request or the network is unavailable');
    bytes = new Uint8Array(xhr.response || new ArrayBuffer(0));
    if (bytes.length > 2147483647) throw new Error('HTTP response exceeds the Wasm32 buffer limit');
    metadata = {status:xhr.status, headers:xhr.getAllResponseHeaders(), url:xhr.responseURL};
  } catch (error) {
    metadata = {error:String(error.message || error), timeout:error.name === 'TimeoutError'};
    bytes = null;
  }
  const body = bytes?.length ? _malloc(bytes.length) : 0;
  if (bytes?.length && !body) {
    metadata = {error:'Could not allocate the HTTP response buffer'};
    bytes = null;
  }
  if (body && bytes) HEAPU8.set(bytes, body);
  const text = stringToNewUTF8(JSON.stringify(metadata));
  HEAPU32[result >> 2] = text;
  HEAPU32[(result + 4) >> 2] = body;
  HEAPU32[(result + 8) >> 2] = bytes?.length || 0;
});

PyObject *request(PyObject *, PyObject *args)
{
  const char *method, *url, *headers;
  PyObject *body;
  double timeout;
  if (!PyArg_ParseTuple(args, "sssOd", &method, &url, &headers, &body, &timeout)) return nullptr;
  if (emscripten_is_main_browser_thread()) {
    PyErr_SetString(PyExc_RuntimeError, "Browser HTTP requires a worker thread");
    return nullptr;
  }
  Py_buffer buffer{};
  if (body != Py_None && PyObject_GetBuffer(body, &buffer, PyBUF_CONTIG_RO)) return nullptr;
  Response response{};
  Py_BEGIN_ALLOW_THREADS
  request_http(method, url, headers, static_cast<const unsigned char *>(buffer.buf),
               buffer.len, timeout, &response);
  Py_END_ALLOW_THREADS
  if (buffer.obj) PyBuffer_Release(&buffer);
  if (!response.metadata) {
    std::free(response.body);
    return PyErr_NoMemory();
  }
  PyObject *metadata = PyUnicode_FromString(response.metadata);
  PyObject *data = PyBytes_FromStringAndSize(reinterpret_cast<char *>(response.body), response.size);
  std::free(response.metadata);
  std::free(response.body);
  PyObject *result = metadata && data ? PyTuple_Pack(2, metadata, data) : nullptr;
  Py_XDECREF(metadata);
  Py_XDECREF(data);
  return result;
}
PyMethodDef methods[] = {{"request", request, METH_VARARGS, nullptr}, {nullptr}};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "_bpy_ghost_web_http", nullptr, -1, methods};
}
PyMODINIT_FUNC PyInit__bpy_ghost_web_http()
{
  return PyModule_Create(&module);
}
