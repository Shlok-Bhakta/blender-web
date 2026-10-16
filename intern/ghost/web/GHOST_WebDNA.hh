/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* DNA's 32-bit layout places CustomData_MeshMasks on four-byte boundaries.
 * Wasm32 otherwise aligns its uint64_t members to eight bytes, inserting
 * implicit padding into Scene that the serialized DNA does not describe.
 * Keep the type's 32-bit DNA layout, rather than disabling DNA verification.
 * These hooks are scoped to that record; Wasm64 keeps its native layout. */
#if defined(__wasm32__)
#  define GHOST_WEB_DNA_PACK_BEGIN() _Pragma("pack(push, 4)")
#  define GHOST_WEB_DNA_PACK_END() _Pragma("pack(pop)")
#else
#  define GHOST_WEB_DNA_PACK_BEGIN()
#  define GHOST_WEB_DNA_PACK_END()
#endif
