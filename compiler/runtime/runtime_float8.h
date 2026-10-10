/* runtime_float8.h — the lowering of lib/Float8.rae (docs/float4-design.md §10).
 *
 * Float8 is eight Float lanes (`struct rae_Float8`, 32 bytes) and Mask8 eight
 * 32-bit lane masks. Same API, same rules and the same scalar definitions as
 * Float4 (runtime_float4.h), in two lowerings:
 *
 *   - AVX2 on x86-64 when the build targets it (`rae` adds -mavx2 on a CPU
 *     that has it: Rae builds from source for its machine, so the choice is
 *     made at compile time, with no runtime dispatch): one 256-bit register;
 *   - everywhere else two Float4 operations on the halves, Float4's own
 *     NEON / SSE2 / wasm SIMD128 / scalar functions. NEON and wasm SIMD128 are
 *     128 bits wide, so two registers are their native width for eight lanes.
 *
 * -DRAE_FLOAT4_SCALAR (`rae run --float4-scalar`) forces the scalar path for
 * both types. Bit-exactness holds as for Float4: IEEE single per lane, no
 * fused multiply-add, min/max/abs as compare-and-select, select/both/either/
 * invert bitwise, comparisons ordered (false on a NaN), and the horizontal
 * operations in the scalar definition's order on every path. */
#ifndef RAE_RUNTIME_FLOAT8_H
#define RAE_RUNTIME_FLOAT8_H

#include "runtime_float4.h"

#if defined(__AVX2__) && !defined(RAE_FLOAT4_SCALAR)
#define RAE_F8_PATH_AVX2 1
#include <immintrin.h>
#endif

#define RAE_F8_INLINE static inline __attribute__((always_inline, unused))

#if defined(RAE_F8_PATH_AVX2)

#define RAE_F8_LOAD(p) _mm256_loadu_ps((const float*)(p))
#define RAE_F8_STORE(p, v) _mm256_storeu_ps((float*)(p), (v))
#define RAE_M8_LOAD(p) _mm256_loadu_si256((const __m256i*)(p))
#define RAE_M8_STORE(p, m) _mm256_storeu_si256((__m256i*)(p), (m))
#define RAE_F8_CMP(a, b, predicate) _mm256_castps_si256(_mm256_cmp_ps((a), (b), (predicate)))
/* mask ? t : f, bit by bit (not blendv, which reads only the sign bit) */
RAE_F8_INLINE __m256 rae_f8_select_v(__m256i m, __m256 t, __m256 f) {
  __m256 mask = _mm256_castsi256_ps(m);
  return _mm256_or_ps(_mm256_and_ps(mask, t), _mm256_andnot_ps(mask, f));
}
#define RAE_F8_NEG(a) _mm256_xor_ps((a), _mm256_set1_ps(-0.0f))

RAE_F8_INLINE void rae_f8_splat_impl(float* out, float value) { RAE_F8_STORE(out, _mm256_set1_ps(value)); }
RAE_F8_INLINE void rae_f8_add_impl(float* out, const void* a, const void* b) {
  RAE_F8_STORE(out, _mm256_add_ps(RAE_F8_LOAD(a), RAE_F8_LOAD(b)));
}
RAE_F8_INLINE void rae_f8_sub_impl(float* out, const void* a, const void* b) {
  RAE_F8_STORE(out, _mm256_sub_ps(RAE_F8_LOAD(a), RAE_F8_LOAD(b)));
}
RAE_F8_INLINE void rae_f8_mul_impl(float* out, const void* a, const void* b) {
  RAE_F8_STORE(out, _mm256_mul_ps(RAE_F8_LOAD(a), RAE_F8_LOAD(b)));
}
RAE_F8_INLINE void rae_f8_div_impl(float* out, const void* a, const void* b) {
  RAE_F8_STORE(out, _mm256_div_ps(RAE_F8_LOAD(a), RAE_F8_LOAD(b)));
}
/* a * b + c: two roundings, never fused */
RAE_F8_INLINE void rae_f8_mul_add_impl(float* out, const void* a, const void* b, const void* c) {
  RAE_F8_STORE(out, _mm256_add_ps(_mm256_mul_ps(RAE_F8_LOAD(a), RAE_F8_LOAD(b)), RAE_F8_LOAD(c)));
}
RAE_F8_INLINE void rae_f8_sqrt_impl(float* out, const void* a) { RAE_F8_STORE(out, _mm256_sqrt_ps(RAE_F8_LOAD(a))); }
RAE_F8_INLINE void rae_f8_negate_impl(float* out, const void* a) { RAE_F8_STORE(out, RAE_F8_NEG(RAE_F8_LOAD(a))); }
RAE_F8_INLINE void rae_f8_abs_impl(float* out, const void* a) {
  __m256 x = RAE_F8_LOAD(a);
  RAE_F8_STORE(out, rae_f8_select_v(RAE_F8_CMP(x, _mm256_setzero_ps(), _CMP_LT_OQ), RAE_F8_NEG(x), x));
}
RAE_F8_INLINE void rae_f8_min_impl(float* out, const void* a, const void* b) {
  __m256 x = RAE_F8_LOAD(a), y = RAE_F8_LOAD(b);
  RAE_F8_STORE(out, rae_f8_select_v(RAE_F8_CMP(x, y, _CMP_LE_OQ), x, y));
}
RAE_F8_INLINE void rae_f8_max_impl(float* out, const void* a, const void* b) {
  __m256 x = RAE_F8_LOAD(a), y = RAE_F8_LOAD(b);
  RAE_F8_STORE(out, rae_f8_select_v(RAE_F8_CMP(x, y, _CMP_GE_OQ), x, y));
}
RAE_F8_INLINE void rae_f8_clamp_symmetric_impl(float* out, const void* value, const void* limit) {
  __m256 x = RAE_F8_LOAD(value), l = RAE_F8_LOAD(limit);
  __m256 r = rae_f8_select_v(RAE_F8_CMP(x, l, _CMP_LE_OQ), x, l);
  __m256 n = RAE_F8_NEG(l);
  RAE_F8_STORE(out, rae_f8_select_v(RAE_F8_CMP(r, n, _CMP_LE_OQ), n, r));
}
RAE_F8_INLINE void rae_f8_less_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, RAE_F8_CMP(RAE_F8_LOAD(a), RAE_F8_LOAD(b), _CMP_LT_OQ));
}
RAE_F8_INLINE void rae_f8_greater_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, RAE_F8_CMP(RAE_F8_LOAD(a), RAE_F8_LOAD(b), _CMP_GT_OQ));
}
RAE_F8_INLINE void rae_f8_less_or_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, RAE_F8_CMP(RAE_F8_LOAD(a), RAE_F8_LOAD(b), _CMP_LE_OQ));
}
RAE_F8_INLINE void rae_f8_greater_or_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, RAE_F8_CMP(RAE_F8_LOAD(a), RAE_F8_LOAD(b), _CMP_GE_OQ));
}
RAE_F8_INLINE void rae_f8_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, RAE_F8_CMP(RAE_F8_LOAD(a), RAE_F8_LOAD(b), _CMP_EQ_OQ));
}
RAE_F8_INLINE void rae_f8_select_impl(float* out, const void* mask, const void* if_true, const void* if_false) {
  RAE_F8_STORE(out, rae_f8_select_v(RAE_M8_LOAD(mask), RAE_F8_LOAD(if_true), RAE_F8_LOAD(if_false)));
}
RAE_F8_INLINE void rae_m8_both_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, _mm256_and_si256(RAE_M8_LOAD(a), RAE_M8_LOAD(b)));
}
RAE_F8_INLINE void rae_m8_either_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_STORE(out, _mm256_or_si256(RAE_M8_LOAD(a), RAE_M8_LOAD(b)));
}
RAE_F8_INLINE void rae_m8_invert_impl(uint32_t* out, const void* a) {
  RAE_M8_STORE(out, _mm256_xor_si256(RAE_M8_LOAD(a), _mm256_set1_epi32(-1)));
}
/* A lane is true when any bit is set: compare each 32-bit lane with zero */
RAE_F8_INLINE bool rae_m8_any_lane(const void* mask) {
  return _mm256_movemask_epi8(_mm256_cmpeq_epi32(RAE_M8_LOAD(mask), _mm256_setzero_si256())) != -1;
}
RAE_F8_INLINE bool rae_m8_all_lanes(const void* mask) {
  return _mm256_movemask_epi8(_mm256_cmpeq_epi32(RAE_M8_LOAD(mask), _mm256_setzero_si256())) == 0;
}

#else /* two Float4 operations on the halves: Float4's lowering, its bits */

#define RAE_F8_HALVES1(name, out, a) \
  do { name((out), (a)); name((out) + 4, (const float*)(a) + 4); } while (0)
#define RAE_F8_HALVES2(name, out, a, b) \
  do { name((out), (a), (b)); name((out) + 4, (const float*)(a) + 4, (const float*)(b) + 4); } while (0)
#define RAE_F8_HALVES3(name, out, a, b, c) \
  do { name((out), (a), (b), (c)); \
       name((out) + 4, (const float*)(a) + 4, (const float*)(b) + 4, (const float*)(c) + 4); } while (0)

RAE_F8_INLINE void rae_f8_splat_impl(float* out, float value) {
  rae_f4_splat_impl(out, value); rae_f4_splat_impl(out + 4, value);
}
RAE_F8_INLINE void rae_f8_add_impl(float* out, const void* a, const void* b) { RAE_F8_HALVES2(rae_f4_add_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_sub_impl(float* out, const void* a, const void* b) { RAE_F8_HALVES2(rae_f4_sub_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_mul_impl(float* out, const void* a, const void* b) { RAE_F8_HALVES2(rae_f4_mul_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_div_impl(float* out, const void* a, const void* b) { RAE_F8_HALVES2(rae_f4_div_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_mul_add_impl(float* out, const void* a, const void* b, const void* c) {
  RAE_F8_HALVES3(rae_f4_mul_add_impl, out, a, b, c);
}
RAE_F8_INLINE void rae_f8_sqrt_impl(float* out, const void* a) { RAE_F8_HALVES1(rae_f4_sqrt_impl, out, a); }
RAE_F8_INLINE void rae_f8_negate_impl(float* out, const void* a) { RAE_F8_HALVES1(rae_f4_negate_impl, out, a); }
RAE_F8_INLINE void rae_f8_abs_impl(float* out, const void* a) { RAE_F8_HALVES1(rae_f4_abs_impl, out, a); }
RAE_F8_INLINE void rae_f8_min_impl(float* out, const void* a, const void* b) { RAE_F8_HALVES2(rae_f4_min_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_max_impl(float* out, const void* a, const void* b) { RAE_F8_HALVES2(rae_f4_max_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_clamp_symmetric_impl(float* out, const void* value, const void* limit) {
  RAE_F8_HALVES2(rae_f4_clamp_symmetric_impl, out, value, limit);
}
#define RAE_M8_HALVES2(name, out, a, b) \
  do { name((out), (a), (b)); name((out) + 4, (const float*)(a) + 4, (const float*)(b) + 4); } while (0)
RAE_F8_INLINE void rae_f8_less_impl(uint32_t* out, const void* a, const void* b) { RAE_M8_HALVES2(rae_f4_less_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_greater_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_HALVES2(rae_f4_greater_impl, out, a, b);
}
RAE_F8_INLINE void rae_f8_less_or_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_HALVES2(rae_f4_less_or_equal_impl, out, a, b);
}
RAE_F8_INLINE void rae_f8_greater_or_equal_impl(uint32_t* out, const void* a, const void* b) {
  RAE_M8_HALVES2(rae_f4_greater_or_equal_impl, out, a, b);
}
RAE_F8_INLINE void rae_f8_equal_impl(uint32_t* out, const void* a, const void* b) { RAE_M8_HALVES2(rae_f4_equal_impl, out, a, b); }
RAE_F8_INLINE void rae_f8_select_impl(float* out, const void* mask, const void* if_true, const void* if_false) {
  rae_f4_select_impl(out, mask, if_true, if_false);
  rae_f4_select_impl(out + 4, (const uint32_t*)mask + 4, (const float*)if_true + 4, (const float*)if_false + 4);
}
RAE_F8_INLINE void rae_m8_both_impl(uint32_t* out, const void* a, const void* b) {
  rae_m4_both_impl(out, a, b); rae_m4_both_impl(out + 4, (const uint32_t*)a + 4, (const uint32_t*)b + 4);
}
RAE_F8_INLINE void rae_m8_either_impl(uint32_t* out, const void* a, const void* b) {
  rae_m4_either_impl(out, a, b); rae_m4_either_impl(out + 4, (const uint32_t*)a + 4, (const uint32_t*)b + 4);
}
RAE_F8_INLINE void rae_m8_invert_impl(uint32_t* out, const void* a) {
  rae_m4_invert_impl(out, a); rae_m4_invert_impl(out + 4, (const uint32_t*)a + 4);
}
RAE_F8_INLINE bool rae_m8_any_lane(const void* mask) {
  return rae_m4_any_lane(mask) || rae_m4_any_lane((const uint32_t*)mask + 4);
}
RAE_F8_INLINE bool rae_m8_all_lanes(const void* mask) {
  return rae_m4_all_lanes(mask) && rae_m4_all_lanes((const uint32_t*)mask + 4);
}

#endif

/* Which lowering: Float4's code (0 scalar, 1 NEON, 2 SSE2, 3 wasm), or 4 AVX2 */
RAE_F8_INLINE int64_t rae_f8_lowering(void) {
#if defined(RAE_F8_PATH_AVX2)
  return 4;
#else
  return rae_f4_lowering();
#endif
}

/* ---- Horizontal operations: the scalar definition on every path -------- */

/* ((((((x0 + x1) + x2) + x3) + x4) + x5) + x6) + x7 */
RAE_F8_INLINE float rae_f8_sum(const void* a) {
  const float* v = a; float s = v[0];
  for (int lane = 1; lane < 8; ++lane) s = s + v[lane];
  return s;
}
RAE_F8_INLINE float rae_f8_horizontal_min(const void* a) {
  const float* v = a; float m = v[0];
  for (int lane = 1; lane < 8; ++lane) m = m <= v[lane] ? m : v[lane];
  return m;
}
RAE_F8_INLINE float rae_f8_horizontal_max(const void* a) {
  const float* v = a; float m = v[0];
  for (int lane = 1; lane < 8; ++lane) m = m >= v[lane] ? m : v[lane];
  return m;
}

/* ---- Load, store and the masked tail (lib/Float8.rae checks the range) -- */

RAE_F8_INLINE void rae_f8_load_impl(float* out, const void* data, int64_t index) {
  memcpy(out, (const float*)data + index, 8 * sizeof(float));
}
RAE_F8_INLINE void rae_f8_store(void* data, int64_t index, const void* value) {
  memcpy((float*)data + index, value, 8 * sizeof(float));
}
/* The first `count` lanes (0..8) touch memory: constant-size copies */
RAE_F8_INLINE void rae_f8_load_partial_impl(float* out, const void* data, int64_t index, int64_t count) {
  const float* source = (const float*)data + index;
  for (int lane = 0; lane < 8; ++lane) out[lane] = 0.0f;
  if (count >= 8) { memcpy(out, source, 8 * sizeof(float)); return; }
  for (int lane = 0; lane < 7; ++lane) {
    if (lane < count) memcpy(&out[lane], &source[lane], sizeof(float));
  }
}
RAE_F8_INLINE void rae_f8_store_partial(void* data, int64_t index, const void* value, int64_t count) {
  float* target = (float*)data + index; const float* v = value;
  if (count >= 8) { memcpy(target, v, 8 * sizeof(float)); return; }
  for (int lane = 0; lane < 7; ++lane) {
    if (lane < count) memcpy(&target[lane], &v[lane], sizeof(float));
  }
}
RAE_F8_INLINE void rae_f8_store_masked(void* data, int64_t index, const void* value, const void* mask, int64_t count) {
  const float* v = value; const uint32_t* m = mask; float* d = (float*)data + index;
  for (int64_t lane = 0; lane < count; ++lane) {
    if (m[lane] != 0) memcpy(&d[lane], &v[lane], sizeof(float));
  }
}

/* ---- The entry points the generated code calls ------------------------- */
#define RAE_F8_RESULT(call) ({ struct rae_Float8 rae_f8_result__; call; rae_f8_result__; })
#define RAE_M8_RESULT(call) ({ struct rae_Mask8 rae_m8_result__; call; rae_m8_result__; })
#define rae_f8_splat(value) RAE_F8_RESULT(rae_f8_splat_impl((float*)&rae_f8_result__, (value)))
#define rae_f8_add(a, b) RAE_F8_RESULT(rae_f8_add_impl((float*)&rae_f8_result__, (a), (b)))
#define rae_f8_sub(a, b) RAE_F8_RESULT(rae_f8_sub_impl((float*)&rae_f8_result__, (a), (b)))
#define rae_f8_mul(a, b) RAE_F8_RESULT(rae_f8_mul_impl((float*)&rae_f8_result__, (a), (b)))
#define rae_f8_div(a, b) RAE_F8_RESULT(rae_f8_div_impl((float*)&rae_f8_result__, (a), (b)))
#define rae_f8_mul_add(a, b, c) RAE_F8_RESULT(rae_f8_mul_add_impl((float*)&rae_f8_result__, (a), (b), (c)))
#define rae_f8_sqrt(a) RAE_F8_RESULT(rae_f8_sqrt_impl((float*)&rae_f8_result__, (a)))
#define rae_f8_negate(a) RAE_F8_RESULT(rae_f8_negate_impl((float*)&rae_f8_result__, (a)))
#define rae_f8_abs(a) RAE_F8_RESULT(rae_f8_abs_impl((float*)&rae_f8_result__, (a)))
#define rae_f8_min(a, b) RAE_F8_RESULT(rae_f8_min_impl((float*)&rae_f8_result__, (a), (b)))
#define rae_f8_max(a, b) RAE_F8_RESULT(rae_f8_max_impl((float*)&rae_f8_result__, (a), (b)))
#define rae_f8_clamp_symmetric(value, limit) \
  RAE_F8_RESULT(rae_f8_clamp_symmetric_impl((float*)&rae_f8_result__, (value), (limit)))
#define rae_f8_select(mask, if_true, if_false) \
  RAE_F8_RESULT(rae_f8_select_impl((float*)&rae_f8_result__, (mask), (if_true), (if_false)))
#define rae_f8_load(data, index) RAE_F8_RESULT(rae_f8_load_impl((float*)&rae_f8_result__, (data), (index)))
#define rae_f8_load_partial(data, index, count) \
  RAE_F8_RESULT(rae_f8_load_partial_impl((float*)&rae_f8_result__, (data), (index), (count)))
#define rae_f8_less(a, b) RAE_M8_RESULT(rae_f8_less_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_f8_greater(a, b) RAE_M8_RESULT(rae_f8_greater_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_f8_less_or_equal(a, b) RAE_M8_RESULT(rae_f8_less_or_equal_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_f8_greater_or_equal(a, b) \
  RAE_M8_RESULT(rae_f8_greater_or_equal_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_f8_equal(a, b) RAE_M8_RESULT(rae_f8_equal_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_m8_both(a, b) RAE_M8_RESULT(rae_m8_both_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_m8_either(a, b) RAE_M8_RESULT(rae_m8_either_impl((uint32_t*)&rae_m8_result__, (a), (b)))
#define rae_m8_invert(a) RAE_M8_RESULT(rae_m8_invert_impl((uint32_t*)&rae_m8_result__, (a)))

#endif
