#include <stdint.h>
#include "tinyexpr.h"
#include "rae_tinyexpr.h"
#include "tinyexpr.c"

int64_t tinyExprEval(const char* expr) {
  int err = 0;
  double value = te_interp(expr ? expr : "", &err);
  return (int64_t)value;
}
