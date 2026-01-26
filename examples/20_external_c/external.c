#include <stdio.h>

int rae_ext_external_c_func(int a, int b) {
    printf("[C] external_c_func called with %d and %d\n", a, b);
    return a + b;
}
