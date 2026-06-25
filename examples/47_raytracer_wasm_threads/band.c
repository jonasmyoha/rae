#include <stdio.h>
#include <stdint.h>

/* Pixel sink: raw RGB to stdout (the worker captures it). */
void rae_ext_fbPixel(int64_t r, int64_t g, int64_t b) {
    unsigned char px[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
    fwrite(px, 1, 3, stdout);
}

/* On wasm, bandY0/bandY1/imgW/imgH/imgSamples are left undefined so the Rae
 * `extern` calls become wasm imports (env.rae_ext_*) via -Wl,--allow-undefined,
 * and each Web Worker supplies them (N workers -> N bands of one image).
 *
 * For the native/compiled target there's no JS to supply them, so provide stub
 * definitions — this example is wasm-only, but the examples suite still
 * compile-links it for the compiled target and the link needs these symbols. */
#ifndef __wasm__
int64_t rae_ext_bandY0(void) { return 0; }
int64_t rae_ext_bandY1(void) { return 0; }
int64_t rae_ext_imgW(void) { return 1; }
int64_t rae_ext_imgH(void) { return 1; }
int64_t rae_ext_imgSamples(void) { return 1; }
#endif
