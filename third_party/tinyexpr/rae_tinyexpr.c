#include <stdint.h>
#include "tinyexpr.h"
#include "rae_tinyexpr.h"

int64_t tinyexpr_eval(const char* expr) {
  int err = 0;
  double value = te_interp(expr ? expr : "", &err);
  return (int64_t)value;
}
