/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

// Initialize once before constructing any OSL shading system or LLVM engine.
void GHOST_web_osl_initialize_runtime();

template<typename ShadingSystem> bool GHOST_web_osl_configure_system(ShadingSystem &system)
{
  return system.attribute("llvm_jit_target", "none");
}
