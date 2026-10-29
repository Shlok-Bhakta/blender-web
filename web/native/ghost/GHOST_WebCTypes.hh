/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Only force-include this header when compiling CPython's original _ctypes
 * sources. Other libraries retain their own dynamic-loader behavior. */
#include <dlfcn.h>
#ifdef __cplusplus
extern "C" {
#endif
void *GHOST_web_ctypes_dlopen(const char *name, int flags);
void *GHOST_web_ctypes_dlsym(void *handle, const char *name);
int GHOST_web_ctypes_dlclose(void *handle);
char *GHOST_web_ctypes_dlerror(void);
#ifdef __cplusplus
}
#endif
#define dlopen GHOST_web_ctypes_dlopen
#define dlsym GHOST_web_ctypes_dlsym
#define dlclose GHOST_web_ctypes_dlclose
#define dlerror GHOST_web_ctypes_dlerror
