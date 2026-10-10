/* runtime_float4.h — the lowering of lib/Float4.rae (docs/float4-design.md §0).
 *
 * Float4 is four Float lanes (`struct rae_Float4 { float x, y, z, w; }`, the
 * layout of a 128-bit vector) and Mask4 four 32-bit lane masks (`struct
 * rae_Mask4`). Every operation is inline code here — never a call into the
 * runtime object — in four lowerings: NEON (arm64), SSE2 (x86-64), wasm
 * SIMD128, and the scalar definitions as the fallback. -DRAE_FLOAT4_SCALAR
 * forces the fallback (`rae run --float4-scalar`), which is how the
 * bit-exactness fixture checks the SIMD paths against it.
 *
 * Bit-exactness: every lowering produces the scalar definition's bits.
 * - add/sub/mul/div/sqrt are IEEE single precision per lane (true of NEON,
 *   SSE2 and wasm SIMD128); -ffp-contract=off keeps them unfused, and
 *   mulAdd is a multiply, then an add — never an FMA instruction.
 * - min/max are compare-and-select (`a <= b ? a : b`, `a >= b ? a : b`), not
 *   the min/max instructions, which differ on -0 and NaN.
 * - abs is `a < 0 ? -a : a` (keeps -0, and a NaN's sign); negate flips the
 *   sign bit, as C's `-a` does.
 * - A mask lane is true when any of its bits is set. Comparisons produce all
 *   ones or all zeros; select, both, either and invert are BITWISE in every
 *   lowering, so a hand-built Mask4 with arbitrary bits gives the same result
 *   on SIMD and on the scalar path.
 * - The horizontal operations read the lanes and apply the scalar definition
 *   in its fixed order, on every path.
 *
 * Each operation is an always-inline function over float[4] / uint32_t[4]
 * that writes its result through a pointer; the `rae_f4_*` macro around it
 * names the result type, which the generated code declares after this
 * header. The macro evaluates each argument exactly once (it hands it to the
 * function). The C compiler keeps the values in vector registers after
 * inlining. Arguments are pointers to Float4 / Mask4 (`view` parameters). */
#ifndef RAE_RUNTIME_FLOAT4_H
#define RAE_RUNTIME_FLOAT4_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(RAE_FLOAT4_SCALAR)
#define RAE_F4_PATH_SCALAR 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define RAE_F4_PATH_NEON 1
#include <arm_neon.h>
#elif defined(__wasm_simd128__)
#define RAE_F4_PATH_WASM 1
#include <wasm_simd128.h>
#elif defined(__SSE2__) || defined(_M_X64)
#define RAE_F4_PATH_SSE2 1
#include <emmintrin.h>
#else
#define RAE_F4_PATH_SCALAR 1
#endif

#define RAE_F4_INLINE static inline __attribute__((always_inline, unused))

/* ---- The vector primitives of each lowering --------------------------- */

#if defined(RAE_F4_PATH_NEON)
typedef float32x4_t rae_f4_v;
typedef uint32x4_t rae_m4_v;
#define RAE_F4_LOAD(p) vld1q_f32((const float*)(p))
#define RAE_F4_STORE(p, v) vst1q_f32((float*)(p), (v))
#define RAE_M4_LOAD(p) vld1q_u32((const uint32_t*)(p))
#define RAE_M4_STORE(p, v) vst1q_u32((uint32_t*)(p), (v))
#define RAE_F4_SPLAT(s) vdupq_n_f32(s)
#define RAE_F4_ADD(a, b) vaddq_f32(a, b)
#define RAE_F4_SUB(a, b) vsubq_f32(a, b)
#define RAE_F4_MUL(a, b) vmulq_f32(a, b)
#define RAE_F4_DIV(a, b) vdivq_f32(a, b)
#define RAE_F4_SQRT(a) vsqrtq_f32(a)
#define RAE_F4_NEG(a) vnegq_f32(a)
#define RAE_F4_LT(a, b) vcltq_f32(a, b)
#define RAE_F4_GT(a, b) vcgtq_f32(a, b)
#define RAE_F4_LE(a, b) vcleq_f32(a, b)
#define RAE_F4_GE(a, b) vcgeq_f32(a, b)
#define RAE_F4_EQ(a, b) vceqq_f32(a, b)
/* mask ? t : f, bit by bit */
#define RAE_F4_SELECT(m, t, f) vbslq_f32((m), (t), (f))
#define RAE_M4_AND(a, b) vandq_u32(a, b)
#define RAE_M4_OR(a, b) vorrq_u32(a, b)
#define RAE_M4_NOT(a) vmvnq_u32(a)
#define RAE_M4_ANY(m) (vmaxvq_u32(m) != 0)
#define RAE_M4_ALL(m) (vminvq_u32(m) != 0)
#elif defined(RAE_F4_PATH_SSE2)
typedef __m128 rae_f4_v;
typedef __m128i rae_m4_v;
#define RAE_F4_LOAD(p) _mm_loadu_ps((const float*)(p))
#define RAE_F4_STORE(p, v) _mm_storeu_ps((float*)(p), (v))
#define RAE_M4_LOAD(p) _mm_loadu_si128((const __m128i*)(p))
#define RAE_M4_STORE(p, v) _mm_storeu_si128((__m128i*)(p), (v))
#define RAE_F4_SPLAT(s) _mm_set1_ps(s)
#define RAE_F4_ADD(a, b) _mm_add_ps(a, b)
#define RAE_F4_SUB(a, b) _mm_sub_ps(a, b)
#define RAE_F4_MUL(a, b) _mm_mul_ps(a, b)
#define RAE_F4_DIV(a, b) _mm_div_ps(a, b)
#define RAE_F4_SQRT(a) _mm_sqrt_ps(a)
#define RAE_F4_NEG(a) _mm_xor_ps((a), _mm_set1_ps(-0.0f))
#define RAE_F4_LT(a, b) _mm_castps_si128(_mm_cmplt_ps(a, b))
#define RAE_F4_GT(a, b) _mm_castps_si128(_mm_cmpgt_ps(a, b))
#define RAE_F4_LE(a, b) _mm_castps_si128(_mm_cmple_ps(a, b))
#define RAE_F4_GE(a, b) _mm_castps_si128(_mm_cmpge_ps(a, b))
#define RAE_F4_EQ(a, b) _mm_castps_si128(_mm_cmpeq_ps(a, b))
#define RAE_F4_SELECT(m, t, f) \
  _mm_or_ps(_mm_and_ps(_mm_castsi128_ps(m), (t)), _mm_andnot_ps(_mm_castsi128_ps(m), (f)))
#define RAE_M4_AND(a, b) _mm_and_si128(a, b)
#define RAE_M4_OR(a, b) _mm_or_si128(a, b)
#define RAE_M4_NOT(a) _mm_xor_si128((a), _mm_set1_epi32(-1))
/* A lane is true when any bit is set: compare each 32-bit lane with zero. */
#define RAE_M4_ANY(m) (_mm_movemask_epi8(_mm_cmpeq_epi32((m), _mm_setzero_si128())) != 0xFFFF)
#define RAE_M4_ALL(m) (_mm_movemask_epi8(_mm_cmpeq_epi32((m), _mm_setzero_si128())) == 0)
#elif defined(RAE_F4_PATH_WASM)
typedef v128_t rae_f4_v;
typedef v128_t rae_m4_v;
#define RAE_F4_LOAD(p) wasm_v128_load(p)
#define RAE_F4_STORE(p, v) wasm_v128_store((p), (v))
#define RAE_M4_LOAD(p) wasm_v128_load(p)
#define RAE_M4_STORE(p, v) wasm_v128_store((p), (v))
#define RAE_F4_SPLAT(s) wasm_f32x4_splat(s)
#define RAE_F4_ADD(a, b) wasm_f32x4_add(a, b)
#define RAE_F4_SUB(a, b) wasm_f32x4_sub(a, b)
#define RAE_F4_MUL(a, b) wasm_f32x4_mul(a, b)
#define RAE_F4_DIV(a, b) wasm_f32x4_div(a, b)
#define RAE_F4_SQRT(a) wasm_f32x4_sqrt(a)
#define RAE_F4_NEG(a) wasm_f32x4_neg(a)
#define RAE_F4_LT(a, b) wasm_f32x4_lt(a, b)
#define RAE_F4_GT(a, b) wasm_f32x4_gt(a, b)
#define RAE_F4_LE(a, b) wasm_f32x4_le(a, b)
#define RAE_F4_GE(a, b) wasm_f32x4_ge(a, b)
#define RAE_F4_EQ(a, b) wasm_f32x4_eq(a, b)
/* wasm_v128_bitselect(a, b, mask) = (a & mask) | (b & ~mask) */
#define RAE_F4_SELECT(m, t, f) wasm_v128_bitselect((t), (f), (m))
#define RAE_M4_AND(a, b) wasm_v128_and(a, b)
#define RAE_M4_OR(a, b) wasm_v128_or(a, b)
#define RAE_M4_NOT(a) wasm_v128_not(a)
#define RAE_M4_ANY(m) wasm_v128_any_true(m)
#define RAE_M4_ALL(m) wasm_i32x4_all_true(m)
#endif

/* ---- Lane-wise operations --------------------------------------------- */

#if !defined(RAE_F4_PATH_SCALAR)

RAE_F4_INLINE void rae_f4_splat_impl(float* out, float value) { RAE_F4_STORE(out, RAE_F4_SPLAT(value)); }
RAE_F4_INLINE void rae_f4_add_impl(float* out, const void* a, const void* b) {
  RAE_F4_STORE(out, RAE_F4_ADD(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_sub_impl(float* out, const void* a, const void* b) {
  RAE_F4_STORE(out, RAE_F4_SUB(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_mul_impl(float* out, const void* a, const void* b) {
  RAE_F4_STORE(out, RAE_F4_MUL(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_div_impl(float* out, const void* a, const void* b) {
  RAE_F4_STORE(out, RAE_F4_DIV(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
/* a * b + c: two roundings, never fused */
RAE_F4_INLINE void rae_f4_mul_add_impl(float* out, const void* a, const void* b, const void* c) {
  RAE_F4_STORE(out, RAE_F4_ADD(RAE_F4_MUL(RAE_F4_LOAD(a), RAE_F4_LOAD(b)), RAE_F4_LOAD(c)));
}
RAE_F4_INLINE void rae_f4_sqrt_impl(float* out, const void* a) { RAE_F4_STORE(out, RAE_F4_SQRT(RAE_F4_LOAD(a))); }
RAE_F4_INLINE void rae_f4_negate_impl(float* out, const void* a) { RAE_F4_STORE(out, RAE_F4_NEG(RAE_F4_LOAD(a))); }
RAE_F4_INLINE void rae_f4_abs_impl(float* out, const void* a) {
  rae_f4_v x = RAE_F4_LOAD(a);
  RAE_F4_STORE(out, RAE_F4_SELECT(RAE_F4_LT(x, RAE_F4_SPLAT(0.0f)), RAE_F4_NEG(x), x));
}
RAE_F4_INLINE void rae_f4_min_impl(float* out, const void* a, const void* b) {
  rae_f4_v x = RAE_F4_LOAD(a), y = RAE_F4_LOAD(b);
  RAE_F4_STORE(out, RAE_F4_SELECT(RAE_F4_LE(x, y), x, y));
}
RAE_F4_INLINE void rae_f4_max_impl(float* out, const void* a, const void* b) {
  rae_f4_v x = RAE_F4_LOAD(a), y = RAE_F4_LOAD(b);
  RAE_F4_STORE(out, RAE_F4_SELECT(RAE_F4_GE(x, y), x, y));
}
/* r = value <= limit ? value : limit; r <= -limit ? -limit : r */
RAE_F4_INLINE void rae_f4_clamp_symmetric_impl(float* out, const void* value, const void* limit) {
  rae_f4_v x = RAE_F4_LOAD(value), l = RAE_F4_LOAD(limit);
  rae_f4_v r = RAE_F4_SELECT(RAE_F4_LE(x, l), x, l);
  rae_f4_v n = RAE_F4_NEG(l);
  RAE_F4_STORE(out, RAE_F4_SELECT(RAE_F4_LE(r, n), n, r));
}
RAE_F4_INLINE void rae_f4_less_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_F4_LT(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_greater_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_F4_GT(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_less_or_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_F4_LE(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_greater_or_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_F4_GE(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_F4_EQ(RAE_F4_LOAD(a), RAE_F4_LOAD(b)));
}
RAE_F4_INLINE void rae_f4_select_impl(float* out, const void* mask, const void* if_true, const void* if_false) {
  RAE_F4_STORE(out, RAE_F4_SELECT(RAE_M4_LOAD(mask), RAE_F4_LOAD(if_true), RAE_F4_LOAD(if_false)));
}
RAE_F4_INLINE void rae_m4_both_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_M4_AND(RAE_M4_LOAD(a), RAE_M4_LOAD(b)));
}
RAE_F4_INLINE void rae_m4_either_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M4_STORE(out, RAE_M4_OR(RAE_M4_LOAD(a), RAE_M4_LOAD(b)));
}
RAE_F4_INLINE void rae_m4_invert_impl(uint32_t* out, const void* a) { RAE_M4_STORE(out, RAE_M4_NOT(RAE_M4_LOAD(a))); }
RAE_F4_INLINE bool rae_m4_any_lane(const void* mask) { return RAE_M4_ANY(RAE_M4_LOAD(mask)); }
RAE_F4_INLINE bool rae_m4_all_lanes(const void* mask) { return RAE_M4_ALL(RAE_M4_LOAD(mask)); }

#else /* the scalar definitions: the reference every lowering above matches */

RAE_F4_INLINE uint32_t rae_f4_bits_of(float value) { uint32_t bits; memcpy(&bits, &value, sizeof bits); return bits; }
RAE_F4_INLINE float rae_f4_float_of(uint32_t bits) { float value; memcpy(&value, &bits, sizeof value); return value; }
#define RAE_F4_LANES(out, expression) \
  for (int lane = 0; lane < 4; ++lane) (out)[lane] = (expression)

RAE_F4_INLINE void rae_f4_splat_impl(float* out, float value) { RAE_F4_LANES(out, value); }
RAE_F4_INLINE void rae_f4_add_impl(float* out, const void* a, const void* b) {
  const float *x = a, *y = b; RAE_F4_LANES(out, x[lane] + y[lane]);
}
RAE_F4_INLINE void rae_f4_sub_impl(float* out, const void* a, const void* b) {
  const float *x = a, *y = b; RAE_F4_LANES(out, x[lane] - y[lane]);
}
RAE_F4_INLINE void rae_f4_mul_impl(float* out, const void* a, const void* b) {
  const float *x = a, *y = b; RAE_F4_LANES(out, x[lane] * y[lane]);
}
RAE_F4_INLINE void rae_f4_div_impl(float* out, const void* a, const void* b) {
  const float *x = a, *y = b; RAE_F4_LANES(out, x[lane] / y[lane]);
}
RAE_F4_INLINE void rae_f4_mul_add_impl(float* out, const void* a, const void* b, const void* c) {
  const float *x = a, *y = b, *z = c;
  for (int lane = 0; lane < 4; ++lane) { float product = x[lane] * y[lane]; out[lane] = product + z[lane]; }
}
RAE_F4_INLINE void rae_f4_sqrt_impl(float* out, const void* a) { const float* x = a; RAE_F4_LANES(out, sqrtf(x[lane])); }
RAE_F4_INLINE void rae_f4_negate_impl(float* out, const void* a) { const float* x = a; RAE_F4_LANES(out, -x[lane]); }
RAE_F4_INLINE void rae_f4_abs_impl(float* out, const void* a) {
  const float* x = a; RAE_F4_LANES(out, x[lane] < 0.0f ? -x[lane] : x[lane]);
}
RAE_F4_INLINE void rae_f4_min_impl(float* out, const void* a, const void* b) {
  const float *x = a, *y = b; RAE_F4_LANES(out, x[lane] <= y[lane] ? x[lane] : y[lane]);
}
RAE_F4_INLINE void rae_f4_max_impl(float* out, const void* a, const void* b) {
  const float *x = a, *y = b; RAE_F4_LANES(out, x[lane] >= y[lane] ? x[lane] : y[lane]);
}
RAE_F4_INLINE void rae_f4_clamp_symmetric_impl(float* out, const void* value, const void* limit) {
  const float *x = value, *l = limit;
  for (int lane = 0; lane < 4; ++lane) {
    float r = x[lane] <= l[lane] ? x[lane] : l[lane];
    float n = -l[lane];
    out[lane] = r <= n ? n : r;
  }
}
#define RAE_F4_COMPARE(out, a, b, op) \
  const float *x = (a), *y = (b); RAE_F4_LANES(out, (x[lane] op y[lane]) ? 0xFFFFFFFFu : 0u)
RAE_F4_INLINE void rae_f4_less_impl(uint32_t* out, const void* a, const void* b) { RAE_F4_COMPARE(out, a, b, <); }
RAE_F4_INLINE void rae_f4_greater_impl(uint32_t* out, const void* a, const void* b) { RAE_F4_COMPARE(out, a, b, >); }
RAE_F4_INLINE void rae_f4_less_or_equal_impl(uint32_t* out, const void* a, const void* b) { RAE_F4_COMPARE(out, a, b, <=); }
RAE_F4_INLINE void rae_f4_greater_or_equal_impl(uint32_t* out, const void* a, const void* b) { RAE_F4_COMPARE(out, a, b, >=); }
RAE_F4_INLINE void rae_f4_equal_impl(uint32_t* out, const void* a, const void* b) { RAE_F4_COMPARE(out, a, b, ==); }
RAE_F4_INLINE void rae_f4_select_impl(float* out, const void* mask, const void* if_true, const void* if_false) {
  const uint32_t* m = mask; const float *t = if_true, *f = if_false;
  RAE_F4_LANES(out, rae_f4_float_of((rae_f4_bits_of(t[lane]) & m[lane]) | (rae_f4_bits_of(f[lane]) & ~m[lane])));
}
RAE_F4_INLINE void rae_m4_both_impl(uint32_t* out, const void* a, const void* b) {
  const uint32_t *x = a, *y = b; RAE_F4_LANES(out, x[lane] & y[lane]);
}
RAE_F4_INLINE void rae_m4_either_impl(uint32_t* out, const void* a, const void* b) {
  const uint32_t *x = a, *y = b; RAE_F4_LANES(out, x[lane] | y[lane]);
}
RAE_F4_INLINE void rae_m4_invert_impl(uint32_t* out, const void* a) { const uint32_t* x = a; RAE_F4_LANES(out, ~x[lane]); }
RAE_F4_INLINE bool rae_m4_any_lane(const void* mask) {
  const uint32_t* m = mask; return (m[0] | m[1] | m[2] | m[3]) != 0;
}
RAE_F4_INLINE bool rae_m4_all_lanes(const void* mask) {
  const uint32_t* m = mask; return m[0] != 0 && m[1] != 0 && m[2] != 0 && m[3] != 0;
}

#endif

/* Which lowering this program was compiled with: 0 scalar, 1 NEON, 2 SSE2,
 * 3 wasm SIMD128 (Float4.lowering). */
RAE_F4_INLINE int64_t rae_f4_lowering(void) {
#if defined(RAE_F4_PATH_NEON)
  return 1;
#elif defined(RAE_F4_PATH_SSE2)
  return 2;
#elif defined(RAE_F4_PATH_WASM)
  return 3;
#else
  return 0;
#endif
}

/* ---- Horizontal operations: the scalar definition on every path -------- */

/* ((x + y) + z) + w */
RAE_F4_INLINE float rae_f4_sum(const void* a) { const float* v = a; return ((v[0] + v[1]) + v[2]) + v[3]; }
/* min(min(min(x, y), z), w) with min(a, b) = a <= b ? a : b */
RAE_F4_INLINE float rae_f4_horizontal_min(const void* a) {
  const float* v = a; float m = v[0];
  for (int lane = 1; lane < 4; ++lane) m = m <= v[lane] ? m : v[lane];
  return m;
}
RAE_F4_INLINE float rae_f4_horizontal_max(const void* a) {
  const float* v = a; float m = v[0];
  for (int lane = 1; lane < 4; ++lane) m = m >= v[lane] ? m : v[lane];
  return m;
}
/* Box3D's b3EmbedIndexW: replace the low bitCount mantissa bits of each lane
 * with baseIndex + lane. For a positive value the index then sorts with the
 * value, and a tie falls to the lower index. */
RAE_F4_INLINE void rae_f4_embed_index_impl(float* out, const void* a, int64_t base_index, int64_t bit_count) {
  const float* v = a; uint32_t mask = (1u << (uint32_t)bit_count) - 1u;
  for (int lane = 0; lane < 4; ++lane) {
    uint32_t bits; memcpy(&bits, &v[lane], sizeof bits);
    bits = (bits & ~mask) | (uint32_t)(base_index + lane);
    memcpy(&out[lane], &bits, sizeof bits);
  }
}
/* Box3D's b3MinIndexW, scalar order: m = x; m = y < m ? y : m; ...; then the
 * low bitCount bits of the minimum lane. */
RAE_F4_INLINE int64_t rae_f4_min_lane_index(const void* a, int64_t bit_count) {
  const float* v = a; float m = v[0];
  for (int lane = 1; lane < 4; ++lane) m = v[lane] < m ? v[lane] : m;
  uint32_t bits; memcpy(&bits, &m, sizeof bits);
  return (int64_t)(bits & ((1u << (uint32_t)bit_count) - 1u));
}

/* ---- Load and store (lib/Float4.rae checks the index range) ------------ */

RAE_F4_INLINE void rae_f4_load_impl(float* out, const void* data, int64_t index) {
  memcpy(out, (const float*)data + index, 4 * sizeof(float));
}
RAE_F4_INLINE void rae_f4_store(void* data, int64_t index, const void* value) {
  memcpy((float*)data + index, value, 4 * sizeof(float));
}

/* The masked tail of a loop (docs/float4-design.md §9): only the first
 * `count` lanes (0..4, checked by lib/Float4.rae against the list's length)
 * touch memory, so nothing past the end is read or written. A memory move,
 * not arithmetic: one definition gives the same bits on every lowering. */
/* Lane by lane with constant-size copies (inlined; a variable-length memcpy
 * would be a library call), bits unchanged, NaN payloads included */
RAE_F4_INLINE void rae_f4_load_partial_impl(float* out, const void* data, int64_t index, int64_t count) {
  const float* source = (const float*)data + index;
  out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 0.0f;
  if (count >= 4) { memcpy(out, source, 4 * sizeof(float)); return; }
  if (count >= 3) memcpy(&out[2], &source[2], sizeof(float));
  if (count >= 2) memcpy(&out[1], &source[1], sizeof(float));
  if (count >= 1) memcpy(&out[0], &source[0], sizeof(float));
}
RAE_F4_INLINE void rae_f4_store_partial(void* data, int64_t index, const void* value, int64_t count) {
  float* target = (float*)data + index; const float* v = value;
  if (count >= 4) { memcpy(target, v, 4 * sizeof(float)); return; }
  if (count >= 3) memcpy(&target[2], &v[2], sizeof(float));
  if (count >= 2) memcpy(&target[1], &v[1], sizeof(float));
  if (count >= 1) memcpy(&target[0], &v[0], sizeof(float));
}
/* The lanes of `mask` that are true (any bit set), among the first `count` */
RAE_F4_INLINE void rae_f4_store_masked(void* data, int64_t index, const void* value, const void* mask, int64_t count) {
  const float* v = value; const uint32_t* m = mask; float* d = (float*)data + index;
  for (int64_t lane = 0; lane < count; ++lane) {
    if (m[lane] != 0) memcpy(&d[lane], &v[lane], sizeof(float));
  }
}

/* ---- The entry points the generated code calls -------------------------
 * `struct rae_Float4` / `struct rae_Mask4` are declared by the generated
 * code (lib/Float4.rae's types); a macro names them at the call site. */
#define RAE_F4_RESULT(call) ({ struct rae_Float4 rae_f4_result__; call; rae_f4_result__; })
#define RAE_M4_RESULT(call) ({ struct rae_Mask4 rae_m4_result__; call; rae_m4_result__; })
#define rae_f4_splat(value) RAE_F4_RESULT(rae_f4_splat_impl((float*)&rae_f4_result__, (value)))
#define rae_f4_add(a, b) RAE_F4_RESULT(rae_f4_add_impl((float*)&rae_f4_result__, (a), (b)))
#define rae_f4_sub(a, b) RAE_F4_RESULT(rae_f4_sub_impl((float*)&rae_f4_result__, (a), (b)))
#define rae_f4_mul(a, b) RAE_F4_RESULT(rae_f4_mul_impl((float*)&rae_f4_result__, (a), (b)))
#define rae_f4_div(a, b) RAE_F4_RESULT(rae_f4_div_impl((float*)&rae_f4_result__, (a), (b)))
#define rae_f4_mul_add(a, b, c) RAE_F4_RESULT(rae_f4_mul_add_impl((float*)&rae_f4_result__, (a), (b), (c)))
#define rae_f4_sqrt(a) RAE_F4_RESULT(rae_f4_sqrt_impl((float*)&rae_f4_result__, (a)))
#define rae_f4_negate(a) RAE_F4_RESULT(rae_f4_negate_impl((float*)&rae_f4_result__, (a)))
#define rae_f4_abs(a) RAE_F4_RESULT(rae_f4_abs_impl((float*)&rae_f4_result__, (a)))
#define rae_f4_min(a, b) RAE_F4_RESULT(rae_f4_min_impl((float*)&rae_f4_result__, (a), (b)))
#define rae_f4_max(a, b) RAE_F4_RESULT(rae_f4_max_impl((float*)&rae_f4_result__, (a), (b)))
#define rae_f4_clamp_symmetric(value, limit) \
  RAE_F4_RESULT(rae_f4_clamp_symmetric_impl((float*)&rae_f4_result__, (value), (limit)))
#define rae_f4_select(mask, if_true, if_false) \
  RAE_F4_RESULT(rae_f4_select_impl((float*)&rae_f4_result__, (mask), (if_true), (if_false)))
#define rae_f4_embed_index(a, base_index, bit_count) \
  RAE_F4_RESULT(rae_f4_embed_index_impl((float*)&rae_f4_result__, (a), (base_index), (bit_count)))
#define rae_f4_load(data, index) RAE_F4_RESULT(rae_f4_load_impl((float*)&rae_f4_result__, (data), (index)))
#define rae_f4_load_partial(data, index, count) \
  RAE_F4_RESULT(rae_f4_load_partial_impl((float*)&rae_f4_result__, (data), (index), (count)))
#define rae_f4_less(a, b) RAE_M4_RESULT(rae_f4_less_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_f4_greater(a, b) RAE_M4_RESULT(rae_f4_greater_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_f4_less_or_equal(a, b) RAE_M4_RESULT(rae_f4_less_or_equal_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_f4_greater_or_equal(a, b) \
  RAE_M4_RESULT(rae_f4_greater_or_equal_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_f4_equal(a, b) RAE_M4_RESULT(rae_f4_equal_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_m4_both(a, b) RAE_M4_RESULT(rae_m4_both_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_m4_either(a, b) RAE_M4_RESULT(rae_m4_either_impl((uint32_t*)&rae_m4_result__, (a), (b)))
#define rae_m4_invert(a) RAE_M4_RESULT(rae_m4_invert_impl((uint32_t*)&rae_m4_result__, (a)))

#endif
