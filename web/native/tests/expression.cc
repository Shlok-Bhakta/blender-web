/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "BLI_expr_pylike_eval.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace blender;
struct Case { const char *expression; double x; eExprPyLike_EvalStatus expected; };
int main()
{
  /* Error fixtures from Blender's BLI_expr_pylike_eval_test.cc, plus cases
   * which distinguish Wasm's software status from just inspecting the output. */
  const Case cases[] = {
    {"0 / 0", 0, EXPR_PYLIKE_MATH_ERROR},
    {"1 / 0", 0, EXPR_PYLIKE_DIV_BY_ZERO},
    {"1 / x", 0, EXPR_PYLIKE_DIV_BY_ZERO},
    {"1 / x", 1, EXPR_PYLIKE_SUCCESS},
    {"sqrt(-1)", 0, EXPR_PYLIKE_MATH_ERROR},
    {"sqrt(x)", -1, EXPR_PYLIKE_MATH_ERROR},
    {"sqrt(x)", 0, EXPR_PYLIKE_SUCCESS},
    {"pow(-1, 0.5)", 0, EXPR_PYLIKE_MATH_ERROR},
    {"pow(-1, x)", 0.5, EXPR_PYLIKE_MATH_ERROR},
    {"pow(-1, x)", 2, EXPR_PYLIKE_SUCCESS},
    {"sqrt(x) + 1 / max(0, x)", -1, EXPR_PYLIKE_MATH_ERROR},
    {"sqrt(x) + 1 / max(0, x)", 0, EXPR_PYLIKE_DIV_BY_ZERO},
    {"sqrt(x) + 1 / max(0, x)", 1, EXPR_PYLIKE_SUCCESS},
    {"sqrt(x) > 0", -1, EXPR_PYLIKE_MATH_ERROR},
    {"log(0)", 0, EXPR_PYLIKE_DIV_BY_ZERO},
    {"log(2, 1)", 0, EXPR_PYLIKE_DIV_BY_ZERO},
    {"pow(0, -1)", 0, EXPR_PYLIKE_DIV_BY_ZERO},
    {"pow(10, 400)", 0, EXPR_PYLIKE_SUCCESS},
    {"1 if x == 0 else 1 / x", 0, EXPR_PYLIKE_SUCCESS},
    {"1 / x if x != 0 else 2", 0, EXPR_PYLIKE_SUCCESS},
    {"sin(x) + cos(x)", 0, EXPR_PYLIKE_SUCCESS},
  };
  const char *names[] = {"x"};
  for (const auto &test : cases) {
    auto *parsed = BLI_expr_pylike_parse(test.expression, names, 1);
    double result = 0;
    auto status = BLI_expr_pylike_eval(parsed, &test.x, 1, &result);
    if (status != test.expected) {
      std::fprintf(stderr, "%s: expected %d, got %d (result %g)\n", test.expression,
                   int(test.expected), int(status), result);
      return 1;
    }
    BLI_expr_pylike_free(parsed);
  }
  std::printf("{\"expressionCases\":%zu}\n", sizeof(cases)/sizeof(*cases));
}
