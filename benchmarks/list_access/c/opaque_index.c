#include <stdint.h>

int64_t rae_ext_benchmarkOpaqueIndex(int64_t index) {
  volatile int64_t opaque_index = index;
  return opaque_index;
}
