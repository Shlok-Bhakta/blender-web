/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef WITH_OPENIMAGEDENOISE
#  include <OpenImageDenoise/oidn.h>
/* Query the actual Wasm device rather than testing native x86 CPUID bits. */
inline bool GHOST_web_denoising_supported()
{
  return oidnIsCPUDeviceSupported();
}
#endif
