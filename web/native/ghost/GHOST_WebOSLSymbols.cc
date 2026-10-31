/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host symbols imported by OSL-generated Wasm programs. The application is not
 * a dynamic-linking main module, so this replaces `dlsym` for those programs. */
#include <cstdint>
#include <cstring>

#include "GHOST_WebOSLSymbols.inc"

namespace {
struct Symbol {
  const char *name;
  void *address;
  bool function;
};
const Symbol symbols[] = {GHOST_WEB_OSL_SYMBOL_ENTRIES};
}  // namespace

extern "C" void *GHOST_web_osl_symbol(const char *name)
{
  size_t first = 0, last = sizeof(symbols) / sizeof(symbols[0]);
  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const int order = std::strcmp(name, symbols[middle].name);
    if (order == 0) {
      return symbols[middle].address;
    }
    if (order < 0) {
      last = middle;
    }
    else {
      first = middle + 1;
    }
  }
  return nullptr;
}

extern "C" void GHOST_web_osl_define_symbols(void (*define)(const char *, void *, bool))
{
  for (const Symbol &symbol : symbols) {
    define(symbol.name, symbol.address, symbol.function);
  }
}

/* LLVM's EngineBuilder opens the program itself (`dlopen(nullptr)`) before it
 * creates any engine. Answer only that request; other handles pass through. */
extern "C" void *__real_dlopen(const char *file, int mode);
extern "C" void *__real_dlsym(void *handle, const char *name);
static char process_handle;
extern "C" void *__wrap_dlopen(const char *file, int mode)
{
  return file ? __real_dlopen(file, mode) : &process_handle;
}
extern "C" void *__wrap_dlsym(void *handle, const char *name)
{
  return handle == &process_handle ? GHOST_web_osl_symbol(name) : __real_dlsym(handle, name);
}
