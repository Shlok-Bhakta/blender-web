// SPDX-License-Identifier: GPL-2.0-or-later
// Imath preserves IEEE binary16 rounding, subnormals, infinities and NaNs.
#define IMATH_HALF_NO_LOOKUP_TABLE
#include <Imath/half.h>
extern "C" float oidn_web_half_to_float(uint32_t bits) {
 return imath_half_to_float(imath_half_bits_t(bits));
}
extern "C" uint32_t oidn_web_float_to_half(float value) {
 return imath_float_to_half(value);
}
