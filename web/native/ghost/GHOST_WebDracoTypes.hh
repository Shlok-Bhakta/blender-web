/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <draco/compression/decode.h>
#include <draco/compression/encode.h>

/* The desktop bridge is a separate shared library. In a static Wasm executable,
 * its global Encoder/Decoder classes collide with libheif's classes, including
 * their destructors. Rename the bridge's opaque types while preserving Draco's
 * namespaced classes and the original exported C entry points. */
namespace draco {
using GHOST_WebDracoEncoder = Encoder;
using GHOST_WebDracoDecoder = Decoder;
}  // namespace draco

#define Encoder GHOST_WebDracoEncoder
#define Decoder GHOST_WebDracoDecoder
