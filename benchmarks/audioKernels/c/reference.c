#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define FRAMES 4096
#define ROUNDS 2000
static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (t.tv_sec * 1000000000LL + t.tv_nsec) / 1000; }
__attribute__((noinline)) static void sum_plain(float* out, const float* a, const float* b, long n) {
  for (long i = 0; i < n; i++) out[i] = a[i] * 0.5f + b[i] * 0.5f;
}
__attribute__((noinline)) static void sum_restrict(float* restrict out, const float* restrict a, const float* restrict b, long n) {
  for (long i = 0; i < n; i++) out[i] = a[i] * 0.5f + b[i] * 0.5f;
}
__attribute__((noinline)) static float sinc_dot(const float* table, const float* pending, long rowA, long rowB, long first, float blend) {
  float sum = 0.0f;
  for (long k = 0; k < 32; k++) { float a = table[rowA + k], b = table[rowB + k]; sum += pending[first + k] * (a + (b - a) * blend); }
  return sum;
}
int main(void) {
  float* out = calloc(FRAMES, 4); float* a = calloc(FRAMES, 4); float* b = calloc(FRAMES, 4); float* table = calloc(257 * 32, 4);
  for (int i = 0; i < FRAMES; i++) { a[i] = (i % 97) * 0.01f; b[i] = (i % 89) * 0.02f; }
  for (int i = 0; i < 257 * 32; i++) table[i] = (i % 13) * 0.001f;
  long long bp = 1LL << 60, br = 1LL << 60, bs = 1LL << 60; float total = 0;
  for (int pass = 0; pass < 6; pass++) {
    long long t = now_us(); for (int r = 0; r < ROUNDS; r++) sum_plain(out, a, b, FRAMES); long long p = now_us() - t;
    t = now_us(); for (int r = 0; r < ROUNDS; r++) sum_restrict(out, a, b, FRAMES); long long q = now_us() - t;
    t = now_us(); for (int r = 0; r < ROUNDS * 48; r++) for (int f = 0; f < 64; f++) total += sinc_dot(table, a, (f % 200) * 32, (f % 200 + 1) * 32, f * 8, 0.25f); long long s = now_us() - t;
    if (pass > 0) { if (p < bp) bp = p; if (q < br) br = q; if (s < bs) bs = s; }
  }
  printf("C plain %lld us, restrict %lld us, sinc %lld us (check %g %d)\n", bp, br, bs, out[100], total > 0);
  return 0;
}
