/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* The browser's device handles belong to the Blender UI worker. Keep Cycles'
 * session controller there; its oneTBB CPU kernels still use the worker pool. */
inline bool GHOST_web_cycles_session_use_thread()
{
  return false;
}

template<class Run, class Unsupported>
inline bool GHOST_web_cycles_session_start(void *thread, bool background,
                                           Run &&run, Unsupported &&unsupported)
{
  if (thread) return false;
  if (background) run();
  else unsupported("Cycles rendered viewport is unavailable in this browser. Use Render Image.");
  return true;
}

template<class Cleanup>
inline bool GHOST_web_cycles_session_join(void *thread, Cleanup &&cleanup)
{
  if (thread) return true;
  cleanup();
  return false;
}

/* CPython's Wasm call trampoline cannot suspend through JavaScript. CPU
 * renders write the complete Render Result through BlenderOutputDriver;
 * the Image Editor uploads it after Python returns to the event loop. */
inline bool GHOST_web_cycles_progressive_display()
{
  return false;
}

/* OpenPGL's CPU-4 kernels compile through its SSE-to-Wasm SIMD path. This
 * identifies the compiled library variant without claiming a native x86 ISA. */
inline int GHOST_web_cycles_guiding_device_type()
{
  return 4;
}

#ifdef WITH_OPENIMAGEDENOISE
#  include "GHOST_WebDenoise.hh"
/* Query the actual Wasm device rather than testing native x86 CPUID bits. */
inline bool GHOST_web_cycles_denoising_supported()
{
  return GHOST_web_denoising_supported();
}
#endif
