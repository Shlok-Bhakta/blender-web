/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone ABI probe. This does not link Blender's wasm32 libraries. */
#include <assert.h>
#include <emscripten/heap.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *check_high_address(void *argument)
{
  volatile unsigned char *bytes = argument;
  const size_t boundary = (size_t)UINT32_MAX + 1;
  assert(bytes[0] == 17);
  assert(bytes[boundary - 1] == 31);
  assert(bytes[boundary] == 47);
  bytes[boundary + 65535] = 93;
  return NULL;
}

int main(void)
{
  _Static_assert(sizeof(void *) == 8, "This probe requires memory64");
  const size_t size = (size_t)UINT32_MAX + 1 + 65536;
  volatile unsigned char *bytes = malloc(size);
  assert(bytes);
  const size_t boundary = (size_t)UINT32_MAX + 1;
  bytes[0] = 17;
  bytes[boundary - 1] = 31;
  bytes[boundary] = 47;
  pthread_t worker;
  assert(pthread_create(&worker, NULL, check_high_address, (void *)bytes) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(bytes[boundary + 65535] == 93);
  printf("MEMORY64_PROBE:{\"pointerBytes\":%zu,\"allocationBytes\":%zu,"
         "\"heapBytes\":%zu,\"highAddress\":%" PRIuPTR ",\"crossThreadReadWrite\":true,"
         "\"sparseTouchOnly\":true}\n", sizeof(void *), size,
         emscripten_get_heap_size(), (uintptr_t)(bytes + boundary));
  free((void *)bytes);
  return 0;
}
