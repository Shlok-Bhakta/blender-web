/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <emscripten/heap.h>
#include <ffi.h>
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

/* Reserve virtual memory sparsely. This checks libffi pointer arithmetic across
 * 2 GiB, not Blender's ability to fill a scene with this much physical RAM. */
namespace {
struct Pair {
  double number;
  int count;
};
void require(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "libffi high-address check failed: %s\n", message);
    std::abort();
  }
}
Pair combine(Pair value, int64_t count)
{
  return {value.number + .5, value.count + int(count)};
}
int64_t wide(int64_t a, double b, int c)
{
  return a + int64_t(b) + c;
}
void callback(ffi_cif *, void *result, void **args, void *)
{
  *static_cast<Pair *>(result) = combine(*static_cast<Pair *>(args[0]),
                                       *static_cast<int64_t *>(args[1]));
}
void *check(void *)
{
  auto *cif = reinterpret_cast<ffi_cif *>(uintptr_t(0x80010000));
  auto *types = reinterpret_cast<ffi_type **>(uintptr_t(0x80010100));
  auto *args = reinterpret_cast<void **>(uintptr_t(0x80010200));
  auto *result = reinterpret_cast<int64_t *>(uintptr_t(0x80010300));
  auto *a = reinterpret_cast<int64_t *>(uintptr_t(0x80010400));
  *a = (1LL << 54) + 13;
  double b = 4.;
  int c = -2;
  types[0] = &ffi_type_sint64;
  types[1] = &ffi_type_double;
  types[2] = &ffi_type_sint32;
  args[0] = a;
  args[1] = &b;
  args[2] = &c;
  require(ffi_prep_cif(cif, FFI_DEFAULT_ABI, 3, &ffi_type_sint64, types) == FFI_OK,
          "prepare high CIF");
  ffi_call(cif, reinterpret_cast<void (*)()>(wide), result, args);
  require(*result == (1LL << 54) + 15, "high metadata and scalar result");

  ffi_type *fields[] = {&ffi_type_double, &ffi_type_sint32, nullptr};
  ffi_type pair{};
  pair.type = FFI_TYPE_STRUCT;
  pair.elements = fields;
  Pair value{1.25, 9}, output{};
  int64_t count = 7;
  types[0] = &pair;
  types[1] = &ffi_type_sint64;
  args[0] = &value;
  args[1] = &count;
  require(ffi_prep_cif(cif, FFI_DEFAULT_ABI, 2, &pair, types) == FFI_OK, "prepare struct CIF");
  ffi_call(cif, reinterpret_cast<void (*)()>(combine), &output, args);
  require(output.number == 1.75 && output.count == 16, "aggregate call on high stack");
  void *code = nullptr;
  auto *memory = static_cast<ffi_closure *>(ffi_closure_alloc(sizeof(ffi_closure), &code));
  require(memory != nullptr, "allocate closure");
  require(ffi_prep_closure_loc(memory, cif, callback, nullptr, code) == FFI_OK, "prepare closure");
  output = reinterpret_cast<Pair (*)(Pair, int64_t)>(code)(value, count);
  require(output.number == 1.75 && output.count == 16, "aggregate callback on high stack");
  ffi_closure_free(memory);
  return nullptr;
}
}  // namespace

int main()
{
  require(emscripten_resize_heap(0x90200000), "grow sparse heap");
  check(nullptr);
  pthread_attr_t attributes;
  require(pthread_attr_init(&attributes) == 0, "initialize attributes");
  require(pthread_attr_setstack(&attributes, reinterpret_cast<void *>(uintptr_t(0x88000000)),
                               1048576) == 0, "custom high stack");
  pthread_t thread;
  require(pthread_create(&thread, &attributes, check, nullptr) == 0, "start high stack worker");
  require(pthread_join(thread, nullptr) == 0, "join high stack worker");
  pthread_attr_destroy(&attributes);
  std::puts("{\"heapBytes\":2418016256,\"highMetadata\":true,\"highStack\":true,"
            "\"aggregateCallbacks\":true,\"sparseAllocation\":true}");
}
