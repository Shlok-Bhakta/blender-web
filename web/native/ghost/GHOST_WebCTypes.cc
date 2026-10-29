/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <frameobject.h>
#include <marshal.h>
#include <structmember.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
thread_local const char *last_error = nullptr;
char process_namespace;
struct Export {
  const char *name;
  void *address;
};
// Legacy declarations remain available through ctypes.pythonapi. Suppress only
// the address-taking warnings in this generated compatibility table.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
const Export python_exports[] = {
#include <blender-web-ctypes-exports.inc>
};
#pragma clang diagnostic pop
const Export libc_exports[] = {
    {"free", reinterpret_cast<void *>(&free)},
    {"ldexp", reinterpret_cast<void *>(static_cast<double (*)(double, int)>(&ldexp))},
    {"llabs", reinterpret_cast<void *>(&llabs)},
    {"malloc", reinterpret_cast<void *>(&malloc)},
    {"memmove", reinterpret_cast<void *>(&memmove)},
    {"memset", reinterpret_cast<void *>(&memset)},
    {"qsort", reinterpret_cast<void *>(&qsort)},
    {"snprintf", reinterpret_cast<void *>(&snprintf)},
    {"strlen", reinterpret_cast<void *>(&strlen)},
};
template<size_t N> void *lookup(const Export (&entries)[N], const char *name)
{
  size_t first = 0, last = N;
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const int order = std::strcmp(name, entries[middle].name);
    if (order == 0) return entries[middle].address;
    if (order < 0) last = middle;
    else first = middle + 1;
  }
  return nullptr;
}
}  // namespace

extern "C" void *GHOST_web_ctypes_dlopen(const char *name, int)
{
  last_error = nullptr;
  if (!name) return &process_namespace;
  last_error = "This browser build supports the static process namespace only";
  return nullptr;
}
extern "C" void *GHOST_web_ctypes_dlsym(void *handle, const char *name)
{
  last_error = nullptr;
  if (handle != &process_namespace || !name) {
    last_error = "Invalid static process namespace or symbol";
    return nullptr;
  }
  if (void *address = lookup(python_exports, name)) return address;
  if (void *address = lookup(libc_exports, name)) return address;
  last_error = "Symbol is not registered in the static process namespace";
  return nullptr;
}
extern "C" int GHOST_web_ctypes_dlclose(void *handle)
{
  last_error = nullptr;
  if (handle == &process_namespace) return 0;
  last_error = "Invalid static process namespace";
  return -1;
}
extern "C" char *GHOST_web_ctypes_dlerror()
{
  const char *error = last_error;
  last_error = nullptr;
  return const_cast<char *>(error);
}
