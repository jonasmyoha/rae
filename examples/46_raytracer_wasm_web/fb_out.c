#include <stdio.h>
#include <stdint.h>
/* Rae `func fbPixel(r,g,b: Int) extern` -> rae_ext_fbPixel. Emits raw RGB
 * bytes to stdout; the harness (node WASI / browser WASI shim) captures fd 1. */
void rae_ext_fbPixel(int64_t r, int64_t g, int64_t b) {
    unsigned char px[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
    fwrite(px, 1, 3, stdout);
}
