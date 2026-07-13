/* Random numbers, raw buffers, libc math wrappers, C JSON extraction bridge, and crypto stubs. Buffers are permanent kernel; JSON/crypto stubs are migration/compatibility bridges.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

// Thread-local: each OS thread (main + spawned workers) gets its own RNG
// stream, so concurrent workers (e.g. a multithreaded raytracer sampling
// random scatter directions) don't race on a shared seed and skew each
// other's distributions. Each worker should seed() itself (e.g. by its band
// index) to decorrelate streams — without that, every thread starts from the
// same default and produces identical sequences. Transparent for
// single-threaded programs (the main thread's instance == the old global).
static __thread uint64_t g_rae_random_state = 0x123456789ABCDEF0ULL;

void rae_ext_rae_seed(int64_t seed) {
  g_rae_random_state = (uint64_t)seed;
}

static uint32_t rae_next_u32(void) {
  g_rae_random_state = g_rae_random_state * 6364136223846793005ULL + 1;
  return (uint32_t)(g_rae_random_state >> 32);
}

double rae_ext_rae_random(void) {
  return (double)rae_next_u32() / (double)4294967295.0;
}

int64_t rae_ext_rae_random_int(int64_t min, int64_t max) {
  if (min >= max) return min;
  uint64_t range = (uint64_t)(max - min + 1);
  return min + (int64_t)(rae_next_u32() % range);
}

double rae_ext_rae_int_to_float(int64_t v) { return (double)v; }
int64_t rae_ext_rae_float_to_int(double v) { return (int64_t)v; }
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
  if (!rae_buf_record_lookup(buf, &count, &recorded_elem)) {
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

#define RAE_BR_REGISTER(ptr, count, elem_size) rae_buf_record_set((ptr), (count), (elem_size))
#define RAE_BR_UNREGISTER(ptr)                 rae_buf_record_clear((ptr))
#define RAE_BR_CHECK(fn, buf, index, elem_size) rae_buf_check((fn), (buf), (index), (elem_size))
#define RAE_BR_CHECK_ANY(fn, buf, index)        rae_buf_check((fn), (buf), (index), (int64_t)sizeof(RaeAny))

#else  /* !RAE_DEBUG_BOUNDS */

#define RAE_BR_REGISTER(ptr, count, elem_size) ((void)0)
#define RAE_BR_UNREGISTER(ptr)                 ((void)0)
#define RAE_BR_CHECK(fn, buf, index, elem_size) ((void)0)
#define RAE_BR_CHECK_ANY(fn, buf, index)        ((void)0)

#endif

void* rae_ext_rae_buf_alloc(int64_t count, int64_t elem_size) {
  if (count <= 0) return NULL;
  void* p = calloc((size_t)count, (size_t)elem_size);
  if (p) { g_mem_buf_alloc_n++; g_mem_buf_alloc_b += count * elem_size; }
  RAE_BR_REGISTER(p, count, elem_size);
  return p;
}

void rae_ext_rae_buf_free(void* buf) {
  if (buf) {
    g_mem_buf_free_n++; g_mem_buf_free_b += rae_malloc_size_safe(buf);
    RAE_BR_UNREGISTER(buf);
    free(buf);
  }
}

void* rae_ext_rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size) {
  if (new_count <= 0) {
    if (buf) {
      g_mem_buf_free_n++; g_mem_buf_free_b += rae_malloc_size_safe(buf);
      RAE_BR_UNREGISTER(buf);
      free(buf);
    }
    return NULL;
  }
  /* realloc accounting: model as free(old) + alloc(new). The
   * outstanding count stays balanced (one free, one alloc per call),
   * which lets a leak-class hunt distinguish "buffers we forgot to
   * free" from "buffers we keep resizing". */
  int64_t old_bytes = buf ? rae_malloc_size_safe(buf) : 0;
  RAE_BR_UNREGISTER(buf);
  void* p = realloc(buf, (size_t)new_count * (size_t)elem_size);
  if (buf) { g_mem_buf_free_n++; g_mem_buf_free_b += old_bytes; }
  if (p)   { g_mem_buf_alloc_n++; g_mem_buf_alloc_b += new_count * elem_size; }
  g_mem_buf_resize_n++;
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

double rae_ext_math_sin(double x) { return sin(x); }
double rae_ext_math_cos(double x) { return cos(x); }
double rae_ext_math_tan(double x) { return tan(x); }
double rae_ext_math_asin(double x) { return asin(x); }
double rae_ext_math_acos(double x) { return acos(x); }
double rae_ext_math_atan(double x) { return atan(x); }
double rae_ext_math_atan2(double y, double x) { return atan2(y, x); }
double rae_ext_math_sqrt(double x) { return sqrt(x); }
double rae_ext_math_pow(double base, double exp) { return pow(base, exp); }
double rae_ext_math_exp(double x) { return exp(x); }
double rae_ext_math_math_log(double x) { return log(x); }
double rae_ext_math_floor(double x) { return floor(x); }
double rae_ext_math_ceil(double x) { return ceil(x); }
double rae_ext_math_round(double x) { return round(x); }

/* JSON helpers for C backend */
static const char* rae_json_find_key(const char* json, int64_t json_len, const char* key) {
    size_t klen = strlen(key);
    for (int64_t i = 0; i < json_len - (int64_t)klen - 2; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, klen) == 0 && json[i + 1 + klen] == '"') {
            int64_t j = i + 1 + (int64_t)klen + 1;
            while (j < json_len && (json[j] == ':' || json[j] == ' ')) j++;
            return json + j;
        }
    }
    return NULL;
}

int64_t rae_json_extract_int(rae_String json, const char* key) {
    const char* v = rae_json_find_key((const char*)json.data, json.len, key);
    if (!v) return 0;
    return strtoll(v, NULL, 10);
}

double rae_json_extract_float(rae_String json, const char* key) {
    const char* v = rae_json_find_key((const char*)json.data, json.len, key);
    if (!v) return 0.0;
    return strtod(v, NULL);
}

rae_Bool rae_json_extract_bool(rae_String json, const char* key) {
    const char* v = rae_json_find_key((const char*)json.data, json.len, key);
    if (!v) return 0;
    return (v[0] == 't') ? 1 : 0;
}

rae_String rae_json_extract_string(rae_String json, const char* key) {
    const char* v = rae_json_find_key((const char*)json.data, json.len, key);
    if (!v || *v != '"') return (rae_String){NULL, 0, 0, 0};
    v++;
    const char* end = strchr(v, '"');
    if (!end) return (rae_String){NULL, 0, 0, 0};
    int64_t len = (int64_t)(end - v);
    uint8_t* copy = (uint8_t*)malloc((size_t)len + 1);
    if (copy) {
      memcpy(copy, v, (size_t)len); copy[len] = 0;
      rae_mem_str_tag(copy, len + 1, RAE_SITE_JSON_EXTRACT);
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
