/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* The browser currently uses OpenSubdiv's CPU evaluator. WebGPU subdivision
 * needs its patch-basis shader source and draw/evaluator checks before opting
 * into Blender's GPU subdivision path. Keep this policy at the platform edge. */
inline bool GHOST_web_subdivision_gpu_supported()
{
  return false;
}
