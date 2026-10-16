/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cmath>
/* WebAssembly has no hardware floating-point status register. Keep expression
 * errors per thread; preserve overflow as a result, as the upstream evaluator
 * only tests invalid operations and division by zero. */
#ifndef FE_DIVBYZERO
#  define FE_DIVBYZERO 4
#  define FE_INVALID 1
#endif
inline thread_local int GHOST_web_float_errors = 0;
inline double GHOST_web_record_math(double value)
{
  if (std::isnan(value)) GHOST_web_float_errors |= FE_INVALID;
  return value;
}
inline double GHOST_web_divide(double a, double b)
{
  if (b == 0 && a != 0 && !std::isnan(a)) GHOST_web_float_errors |= FE_DIVBYZERO;
  return GHOST_web_record_math(a / b);
}
inline double GHOST_web_log(double value)
{
  if (value == 0) GHOST_web_float_errors |= FE_DIVBYZERO;
  return GHOST_web_record_math(std::log(value));
}
inline double GHOST_web_pow(double a, double b)
{
  if (a == 0 && b < 0) GHOST_web_float_errors |= FE_DIVBYZERO;
  return GHOST_web_record_math(std::pow(a, b));
}
inline int GHOST_web_test_float_errors(int mask, double result)
{
  GHOST_web_record_math(result);
  return GHOST_web_float_errors & mask;
}
#define feclearexcept(mask) (GHOST_web_float_errors = 0)
#define fetestexcept(mask) GHOST_web_test_float_errors(mask, result)
#define log GHOST_web_log
#define pow GHOST_web_pow
