/* Raw buffers, libc math wrappers, the toJson buffer as a String, and crypto stubs. Buffers are permanent kernel; crypto stubs are a compatibility bridge.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

float rae_ext_rae_int_to_float(int64_t v){ return (double)v; }
int64_t rae_ext_rae_float_to_int(float v){ return (int64_t)v; }
/* #973: the raw IEEE-754 bits of a 32-bit float as a u32 (in an int64). Lets
 * GpuArgs.pushF32 append an f32's exact bytes without a per-call 1-element
 * List(Float) + buf_copy — that scratch alloc was ~500k Buffer allocs / 18 s
 * in 114 (the dominant idle malloc traffic). */

/* Bit intrinsics over the 64-bit two's-complement representation of an Int.
 * leadingZeros/trailingZeros of 0 are defined as 64 (the builtins are UB on 0). */
/* popcount / leading_zeros / trailing_zeros are static inline in rae_runtime.h. */
/* Debug-only bounds checking for rae_buf_get/set. Compiled in when the
 * binary is built with `-DRAE_DEBUG_BOUNDS`. Tracks (ptr -> count, elem_size)
 * in a small open-addressed hash; on every get/set the entry is looked up
 * and the index range-checked. On miss (buffer not allocated via this
 * runtime, e.g. raylib-owned), the check is skipped. */
#ifdef RAE_DEBUG_BOUNDS

#define RAE_BR_CAP 4096   /* must be power of two */
typedef struct {
  void*   ptr;
  int64_t count;
  int64_t elem_size;
} RaeBufRecord;
static RaeBufRecord g_buf_records[RAE_BR_CAP];
static int g_buf_records_inited = 0;
/* Buffers are allocated and freed on any thread: one lock for the table. */
static pthread_mutex_t g_buf_records_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t rae_buf_slot(void* ptr) {
  uintptr_t k = (uintptr_t)ptr;
  k ^= k >> 16;
  k *= 0x9E3779B97F4A7C15ULL;
  k ^= k >> 32;
  return (size_t)(k & (RAE_BR_CAP - 1));
}

static void rae_buf_record_set(void* ptr, int64_t count, int64_t elem_size) {
  if (!ptr) return;
  if (!g_buf_records_inited) {
    memset(g_buf_records, 0, sizeof(g_buf_records));
    g_buf_records_inited = 1;
  }
  size_t s = rae_buf_slot(ptr);
  for (size_t i = 0; i < RAE_BR_CAP; i++) {
    size_t idx = (s + i) & (RAE_BR_CAP - 1);
    if (g_buf_records[idx].ptr == NULL || g_buf_records[idx].ptr == ptr) {
      g_buf_records[idx].ptr = ptr;
      g_buf_records[idx].count = count;
      g_buf_records[idx].elem_size = elem_size;
      return;
    }
  }
  /* Table full — silently drop. Caller loses the bounds check for this
   * one allocation; everything else still works. */
}

static void rae_buf_record_clear(void* ptr) {
  if (!ptr || !g_buf_records_inited) return;
  size_t s = rae_buf_slot(ptr);
  for (size_t i = 0; i < RAE_BR_CAP; i++) {
    size_t idx = (s + i) & (RAE_BR_CAP - 1);
    if (g_buf_records[idx].ptr == ptr) {
      g_buf_records[idx].ptr = NULL;
      g_buf_records[idx].count = 0;
      g_buf_records[idx].elem_size = 0;
      return;
    }
    if (g_buf_records[idx].ptr == NULL) return;
  }
}

static int rae_buf_record_lookup(void* ptr, int64_t* count_out, int64_t* elem_size_out) {
  if (!ptr || !g_buf_records_inited) return 0;
  size_t s = rae_buf_slot(ptr);
  for (size_t i = 0; i < RAE_BR_CAP; i++) {
    size_t idx = (s + i) & (RAE_BR_CAP - 1);
    if (g_buf_records[idx].ptr == ptr) {
      *count_out = g_buf_records[idx].count;
      *elem_size_out = g_buf_records[idx].elem_size;
      return 1;
    }
    if (g_buf_records[idx].ptr == NULL) return 0;
  }
  return 0;
}

static void rae_buf_check(const char* fn, void* buf, int64_t index, int64_t elem_size) {
  int64_t count = 0;
  int64_t recorded_elem = 0;
  RAE_LOCK(&g_buf_records_lock);
  int found = rae_buf_record_lookup(buf, &count, &recorded_elem);
  RAE_UNLOCK(&g_buf_records_lock);
  if (!found) {
    /* Not from rae_ext_rae_buf_alloc — skip. */
    return;
  }
  if (elem_size != recorded_elem) {
    fprintf(stderr,
      "\n[rae debug] %s(buf=%p, index=%lld) elem_size=%lld but allocated elem_size=%lld\n",
      fn, buf, (long long)index, (long long)elem_size, (long long)recorded_elem);
    fflush(stderr);
    abort();
  }
  if (index < 0 || index >= count) {
    fprintf(stderr,
      "\n[rae debug] %s(buf=%p) out-of-bounds: index=%lld, length=%lld (elem_size=%lld)\n",
      fn, buf, (long long)index, (long long)count, (long long)elem_size);
    fflush(stderr);
    abort();
  }
}

#define RAE_BR_REGISTER(ptr, count, elem_size) do { RAE_LOCK(&g_buf_records_lock); rae_buf_record_set((ptr), (count), (elem_size)); RAE_UNLOCK(&g_buf_records_lock); } while (0)
#define RAE_BR_UNREGISTER(ptr)                 do { RAE_LOCK(&g_buf_records_lock); rae_buf_record_clear((ptr)); RAE_UNLOCK(&g_buf_records_lock); } while (0)
#define RAE_BR_CHECK(fn, buf, index, elem_size) rae_buf_check((fn), (buf), (index), (elem_size))
#define RAE_BR_CHECK_ANY(fn, buf, index)        rae_buf_check((fn), (buf), (index), (int64_t)sizeof(RaeAny))

#else  /* !RAE_DEBUG_BOUNDS */

#define RAE_BR_REGISTER(ptr, count, elem_size) ((void)0)
#define RAE_BR_UNREGISTER(ptr)                 ((void)0)
#define RAE_BR_CHECK(fn, buf, index, elem_size) ((void)0)
#define RAE_BR_CHECK_ANY(fn, buf, index)        ((void)0)

#endif

/* Buffer bytes are measured like String bytes (rae_mem_block_bytes), and
 * only while RAE_MEM_STATS is on: the COUNTS stay always-live (they back
 * rae_ext_rae_mem_alloc_total), but asking the allocator for a block's size
 * costs a call per alloc and free, which only the stats need. */
static inline int64_t rae_mem_buf_bytes(void* ptr, int64_t hint) {
  return g_mem_stats_enabled ? rae_mem_block_bytes(ptr, hint) : 0;
}

void* rae_ext_rae_buf_alloc(int64_t count, int64_t elem_size) {
  if (count <= 0) return NULL;
  void* p = calloc((size_t)count, (size_t)elem_size);
  if (p) { RAE_STAT_ADD(RAE_MC_BUF_ALLOC_N, 1); RAE_STAT_ADD(RAE_MC_BUF_ALLOC_B, rae_mem_buf_bytes(p, count * elem_size)); }
  RAE_BR_REGISTER(p, count, elem_size);
  return p;
}

void rae_ext_rae_buf_free(void* buf) {
  if (buf) {
    RAE_STAT_ADD(RAE_MC_BUF_FREE_N, 1); RAE_STAT_ADD(RAE_MC_BUF_FREE_B, rae_mem_buf_bytes(buf, 0));
    RAE_BR_UNREGISTER(buf);
    free(buf);
  }
}

void* rae_ext_rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size) {
  if (new_count <= 0) {
    if (buf) {
      RAE_STAT_ADD(RAE_MC_BUF_FREE_N, 1); RAE_STAT_ADD(RAE_MC_BUF_FREE_B, rae_mem_buf_bytes(buf, 0));
      RAE_BR_UNREGISTER(buf);
      free(buf);
    }
    return NULL;
  }
  /* realloc accounting: model as free(old) + alloc(new). The
   * outstanding count stays balanced (one free, one alloc per call),
   * which lets a leak-class hunt distinguish "buffers we forgot to
   * free" from "buffers we keep resizing". */
  int64_t old_bytes = buf ? rae_mem_buf_bytes(buf, 0) : 0;
  RAE_BR_UNREGISTER(buf);
  void* p = realloc(buf, (size_t)new_count * (size_t)elem_size);
  if (buf) { RAE_STAT_ADD(RAE_MC_BUF_FREE_N, 1); RAE_STAT_ADD(RAE_MC_BUF_FREE_B, old_bytes); }
  if (p)   { RAE_STAT_ADD(RAE_MC_BUF_ALLOC_N, 1); RAE_STAT_ADD(RAE_MC_BUF_ALLOC_B, rae_mem_buf_bytes(p, new_count * elem_size)); }
  RAE_STAT_ADD(RAE_MC_BUF_RESIZE_N, 1);
  RAE_BR_REGISTER(p, new_count, elem_size);
  return p;
}

void rae_ext_rae_buf_copy(void* src, int64_t src_off, void* dst, int64_t dst_off, int64_t len, int64_t elem_size) {
  if (!src || !dst || len <= 0) return;
  memmove((char*)dst + dst_off * elem_size, (char*)src + src_off * elem_size, (size_t)len * (size_t)elem_size);
}

void rae_ext_rae_buf_set(void* buf, int64_t index, int64_t elem_size, const void* value) {
  if (!buf || !value) return;
  RAE_BR_CHECK("rae_buf_set", buf, index, elem_size);
  memcpy((char*)buf + (index * elem_size), value, (size_t)elem_size);
}

void rae_ext_rae_buf_get(void* buf, int64_t index, int64_t elem_size, void* out_val) {
  if (!buf || !out_val) return;
  RAE_BR_CHECK("rae_buf_get", buf, index, elem_size);
  memcpy(out_val, (char*)buf + (index * elem_size), (size_t)elem_size);
}

void rae_ext_rae_buf_set_any(void* buf, int64_t index, RaeAny value) {
  if (!buf) return;
  RAE_BR_CHECK_ANY("rae_buf_set_any", buf, index);
  ((RaeAny*)buf)[index] = value;
}

RaeAny rae_ext_rae_buf_get_any(void* buf, int64_t index) {
  if (!buf) return rae_any_none();
  RAE_BR_CHECK_ANY("rae_buf_get_any", buf, index);
  return ((RaeAny*)buf)[index];
}

/* lib/Math's transcendental functions, out of line on purpose (see the
 * comment at their declarations in rae_runtime.h); the exactly specified ones
 * (sqrt, floor, ceil, round, remainder, the bit casts) are static inline there. */
float rae_ext_Math_sin(float x) { return sinf(x); }
float rae_ext_Math_cos(float x) { return cosf(x); }
float rae_ext_Math_tan(float x) { return tanf(x); }
float rae_ext_Math_asin(float x) { return asinf(x); }
float rae_ext_Math_acos(float x) { return acosf(x); }
float rae_ext_Math_atan(float x) { return atanf(x); }
float rae_ext_Math_atan2(float y, float x) { return atan2f(y, x); }
float rae_ext_Math_pow(float base, float exp) { return powf(base, exp); }
float rae_ext_Math_exp(float x) { return expf(x); }
float rae_ext_Math_math_log(float x) { return logf(x); }

/* The toJson buffer as an owned String (the generated toJson calls it; the
 * JSON reading is lib/core/JsonScan.rae, docs/runtime-c-audit.md row 5) */
// #761: build an owned JSON String and manage it like any other owned-String
// producer — mem-tag it AND pool-register it. The C backend emits owned method
// results (obj.toJson()) as pool temps: a surrounding `pool_flush` frees an
// inlined result, and `pool_take` hands ownership to a local binding. Skipping
// the register left the result untracked, so the let-then-view idiom
// (`let s = obj.toJson(); useView(s)`) over-freed (outstanding=-1) and the
// inlined form leaked. Reuses the JSON_EXTRACT site for stats.
rae_String rae_json_build(const char* s, int64_t len) {
    uint8_t* copy = (uint8_t*)malloc((size_t)len + 1);
    if (copy) {
        memcpy(copy, s, (size_t)len); copy[len] = 0;
        rae_mem_str_tag(copy, len + 1, RAE_SITE_JSON_EXTRACT);
        rae_string_pool_register(copy);
    }
    return (rae_String){copy, len, len + 1, 1};
}

/* Crypto stub wrappers for C backend — actual crypto requires monocypher linkage */
void rae_ext_rae_crypto_lock(RaeAny key, RaeAny nonce, RaeAny plain, int64_t plain_len, RaeAny mac, RaeAny cipher) {
    (void)key; (void)nonce; (void)plain; (void)plain_len; (void)mac; (void)cipher;
    /* Requires monocypher linkage for actual implementation */
}

int64_t rae_ext_rae_crypto_unlock(RaeAny key, RaeAny nonce, RaeAny mac, RaeAny cipher, int64_t cipher_len, RaeAny plain) {
    (void)key; (void)nonce; (void)mac; (void)cipher; (void)cipher_len; (void)plain;
    return -1; /* Requires monocypher linkage */
}

void rae_ext_rae_crypto_argon2i(rae_String password, rae_String salt, int64_t nb_blocks, int64_t nb_iterations, RaeAny hash_buf, int64_t hash_len) {
    (void)password; (void)salt; (void)nb_blocks; (void)nb_iterations; (void)hash_buf; (void)hash_len;
    /* Requires monocypher linkage; signature matches lib/sys.rae declaration. */
}
