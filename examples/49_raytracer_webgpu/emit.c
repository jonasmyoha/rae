#include <stdio.h>
#include <stdint.h>
/* Rae `func emitFloat(v: Float) extern` -> rae_ext_emitFloat. Rae Float is a
 * C double; we narrow to 4-byte float and write it little-endian to stdout so
 * the host can read the scene as a Float32Array. fd 1 is captured by the
 * harness (node WASI / browser WASI shim). */
void rae_ext_emitFloat(double v) {
    float f = (float)v;
    fwrite(&f, sizeof(float), 1, stdout);
}
