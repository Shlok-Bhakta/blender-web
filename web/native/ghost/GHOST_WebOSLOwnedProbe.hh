/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
extern "C" void GHOST_web_osl_retire_mapped_range(void *base, size_t size);

#include <atomic>
extern std::atomic<unsigned> GHOST_web_osl_program_epoch;
extern thread_local unsigned GHOST_web_osl_loaded_epoch;
extern "C" void GHOST_web_osl_refresh_programs();
inline void GHOST_web_osl_ensure_programs()
{
  if (GHOST_web_osl_loaded_epoch != GHOST_web_osl_program_epoch.load(std::memory_order_acquire))
    GHOST_web_osl_refresh_programs();
}
