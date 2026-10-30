/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>
extern "C" unsigned owned_create(const void *, size_t, unsigned, uintptr_t, uintptr_t, unsigned);
extern "C" uintptr_t owned_symbol(unsigned, const char *);
extern "C" uintptr_t owned_data_symbol(unsigned, const char *);
extern "C" void owned_describe(unsigned, uint32_t *);
extern "C" int owned_retire(unsigned);
extern "C" int owned_clean();
extern "C" unsigned GHOST_web_osl_live_programs();
