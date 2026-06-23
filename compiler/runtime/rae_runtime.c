#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(__APPLE__) || defined(__linux__) || defined(__GLIBC__)
#include <execinfo.h>
#define RAE_HAVE_BACKTRACE 1
#endif

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <malloc/malloc.h>
#endif

#if defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#endif

void rae_flush_stdout(void) {
  fflush(stdout);
}

/* Crash handler — runs on SIGSEGV / SIGBUS / SIGFPE / SIGABRT and prints a
 * C-level backtrace to stderr before letting the signal kill the process.
 * The backtrace points at compiler-emitted symbols, not Rae source lines,
 * but it's a much better starting point than the kernel's silent kill.
 * Re-raising the signal lets the OS still record the crash + dump core. */
static void rae_crash_handler(int sig) {
  const char* name = "signal";
  switch (sig) {
    case SIGSEGV: name = "SIGSEGV (invalid memory access)"; break;
    case SIGBUS:  name = "SIGBUS (bus error / misaligned access)"; break;
    case SIGFPE:  name = "SIGFPE (arithmetic error)"; break;
    case SIGILL:  name = "SIGILL (illegal instruction)"; break;
    case SIGABRT: name = "SIGABRT (abort)"; break;
  }
  /* Async-signal-safe path: write(2) only. */
  const char* prefix = "\n[rae crash] caught ";
  write(STDERR_FILENO, prefix, strlen(prefix));
  write(STDERR_FILENO, name, strlen(name));
  const char* suffix =
    "\n[rae crash] C-level backtrace (Rae source lines are not in here;\n"
    "[rae crash] cross-reference symbols via `atos` or `addr2line` on the\n"
    "[rae crash] generated binary if you need the call site):\n";
  write(STDERR_FILENO, suffix, strlen(suffix));
#ifdef RAE_HAVE_BACKTRACE
  void* frames[64];
  int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
#else
  const char* msg = "[rae crash] (backtrace() not available on this platform)\n";
  write(STDERR_FILENO, msg, strlen(msg));
#endif
  /* Exit with the conventional signal exit code so a parent process /
   * supervisor sees a non-zero status and can restart. On macOS,
   * `raise(sig)` after restoring SIG_DFL doesn't reliably terminate for
   * synchronously-delivered SIGSEGV/SIGBUS, so we just `_exit` here. */
  _exit(128 + sig);
}

__attribute__((constructor))
static void rae_install_crash_handler(void) {
  /* Skip if user explicitly disables (e.g. when running under a debugger
   * that wants the raw signal). */
  if (getenv("RAE_NO_CRASH_HANDLER")) return;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = rae_crash_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER | SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS,  &sa, NULL);
  sigaction(SIGFPE,  &sa, NULL);
  sigaction(SIGILL,  &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

/* ---- Allocation stats (opt-in via RAE_MEM_STATS=1) ----
 *
 * Tracks String body and List/Map buf allocations *per call site* so
 * a stress run can pinpoint which helper produces the strings that
 * never get freed. Per-site counters are gated on g_mem_stats_enabled
 * so disabled runs pay zero overhead. When enabled, a side hash
 * table maps ptr → site so the three free paths (str_free, str_interp
 * owned-input cleanup, pool_flush) can attribute back to the alloc
 * site. Output goes to stderr at process exit (atexit) and on demand
 * via rae_ext_rae_mem_stats_dump().
 */

typedef enum {
  RAE_SITE_FROM_CSTR = 0,
  RAE_SITE_FROM_BUF,
  RAE_SITE_COPY,
  RAE_SITE_INTERP,
  RAE_SITE_CONCAT,
  RAE_SITE_SUB,
  RAE_SITE_INT_TO_STR,
  RAE_SITE_FLOAT_TO_STR,
  RAE_SITE_BOOL_TO_STR,
  RAE_SITE_CHAR_TO_STR,
  RAE_SITE_STR_STRING,    /* rae_ext_rae_str_string deep-copy */
  RAE_SITE_FORMAT_TS,
  RAE_SITE_FORMAT_DATE,
  RAE_SITE_READ_LINE,
  RAE_SITE_READ_FILE,
  RAE_SITE_JSON_GET_STR,
  RAE_SITE_JSON_GET_OBJ,
  RAE_SITE_JSON_EXTRACT,
  RAE_SITE_UNKNOWN,       /* ptr seen at free time that wasn't tracked */
  RAE_SITE__COUNT
} RaeMemSite;

static const char* const g_rae_site_names[RAE_SITE__COUNT] = {
  "from_cstr", "from_buf", "copy", "interp", "concat", "sub",
  "int_to_str", "float_to_str", "bool_to_str", "char_to_str",
  "str_string", "format_ts", "format_date", "read_line", "read_file",
  "json_get_str", "json_get_obj", "json_extract", "unknown"
};

static int64_t g_mem_site_alloc_n[RAE_SITE__COUNT];
static int64_t g_mem_site_alloc_b[RAE_SITE__COUNT];
static int64_t g_mem_site_free_n[RAE_SITE__COUNT];
static int64_t g_mem_site_free_b[RAE_SITE__COUNT];

static int64_t g_mem_buf_alloc_n;
static int64_t g_mem_buf_alloc_b;
static int64_t g_mem_buf_free_n;
static int64_t g_mem_buf_free_b;
static int64_t g_mem_buf_resize_n;

static int64_t g_mem_pool_register_n;
static int64_t g_mem_pool_remove_n;
static int64_t g_mem_pool_flush_calls;
static int64_t g_mem_pool_flush_freed;

static int g_mem_stats_enabled = 0;

/* Side hash table: ptr → site. Allocated only when mem-stats is on.
 * 4M slots sized for ~2M outstanding allocations (peak observed in
 * the 20K mobile-UI stress is ~1.2M). Two parallel arrays keep the
 * key array dense (better cache behaviour for the probe scan). */
#define RAE_MEM_HASH_BITS 22
#define RAE_MEM_HASH_N    (1u << RAE_MEM_HASH_BITS)
#define RAE_MEM_HASH_MASK (RAE_MEM_HASH_N - 1)

static void**   g_mem_hash_keys;   /* NULL = empty slot */
static uint8_t* g_mem_hash_sites;  /* parallel array, valid when key != NULL */
static int64_t  g_mem_hash_size;   /* current occupancy */
static int64_t  g_mem_hash_full_drops; /* allocs we couldn't tag because table was full */

static int64_t rae_malloc_size_safe(void* p) {
  if (!p) return 0;
#if defined(__APPLE__)
  return (int64_t)malloc_size(p);
#elif defined(__linux__) || defined(__GLIBC__)
  return (int64_t)malloc_usable_size(p);
#else
  return 0;
#endif
}

static inline uint32_t rae_mem_hash_ix(void* ptr) {
  uintptr_t x = (uintptr_t)ptr;
  /* Mix high and low bits — malloc returns 16-byte aligned blocks on
   * macOS, so the low 4 bits are usually 0; shifting + xor avoids
   * clustering on the bottom of the table. */
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  return (uint32_t)(x & RAE_MEM_HASH_MASK);
}

/* Insert ptr → site. If table is full, the lookup at free time will
 * miss and the free is attributed to RAE_SITE_UNKNOWN; we track the
 * drop count so we can warn about under-attribution. */
static void rae_mem_hash_insert(void* ptr, uint8_t site) {
  if (!ptr || !g_mem_hash_keys) return;
  /* Cap load factor at ~75% to keep linear probing snappy. */
  if (g_mem_hash_size * 4 >= (int64_t)RAE_MEM_HASH_N * 3) {
    g_mem_hash_full_drops++;
    return;
  }
  uint32_t ix = rae_mem_hash_ix(ptr);
  for (uint32_t i = 0; i < RAE_MEM_HASH_N; i++) {
    uint32_t k = (ix + i) & RAE_MEM_HASH_MASK;
    if (g_mem_hash_keys[k] == NULL) {
      g_mem_hash_keys[k] = ptr;
      g_mem_hash_sites[k] = site;
      g_mem_hash_size++;
      return;
    }
    if (g_mem_hash_keys[k] == ptr) {
      /* Reinsert (e.g. realloc returned same ptr) — overwrite site. */
      g_mem_hash_sites[k] = site;
      return;
    }
  }
  g_mem_hash_full_drops++;
}

/* Lookup + remove. Returns the site that was recorded, or
 * RAE_SITE_UNKNOWN if the ptr wasn't tracked. Linear probing requires
 * the standard tombstone dance: on delete, walk forward and re-insert
 * any contiguous keys whose natural slot might lie before the gap. */
static uint8_t rae_mem_hash_remove(void* ptr) {
  if (!ptr || !g_mem_hash_keys) return RAE_SITE_UNKNOWN;
  uint32_t ix = rae_mem_hash_ix(ptr);
  for (uint32_t i = 0; i < RAE_MEM_HASH_N; i++) {
    uint32_t k = (ix + i) & RAE_MEM_HASH_MASK;
    if (g_mem_hash_keys[k] == NULL) return RAE_SITE_UNKNOWN;
    if (g_mem_hash_keys[k] == ptr) {
      uint8_t site = g_mem_hash_sites[k];
      g_mem_hash_keys[k] = NULL;
      g_mem_hash_size--;
      /* Compact the cluster so later lookups don't get stuck on the
       * gap. Walk forward; for each contiguous key whose natural
       * index is <= our hole, move it into the hole. */
      uint32_t hole = k;
      for (uint32_t j = 1; j < RAE_MEM_HASH_N; j++) {
        uint32_t kk = (hole + j) & RAE_MEM_HASH_MASK;
        if (g_mem_hash_keys[kk] == NULL) break;
        uint32_t nat = rae_mem_hash_ix(g_mem_hash_keys[kk]);
        /* Does the natural slot lie in the range (hole, kk] going
         * forward? If not, moving the entry into the hole is safe. */
        uint32_t dist_hole = (kk - hole) & RAE_MEM_HASH_MASK;
        uint32_t dist_nat  = (kk - nat) & RAE_MEM_HASH_MASK;
        if (dist_nat >= dist_hole) {
          g_mem_hash_keys[hole]  = g_mem_hash_keys[kk];
          g_mem_hash_sites[hole] = g_mem_hash_sites[kk];
          g_mem_hash_keys[kk]    = NULL;
          hole = kk;
        }
      }
      return site;
    }
  }
  return RAE_SITE_UNKNOWN;
}

static inline void rae_mem_str_tag(void* ptr, int64_t bytes, uint8_t site) {
  if (!g_mem_stats_enabled) return;
  g_mem_site_alloc_n[site]++;
  g_mem_site_alloc_b[site] += bytes;
  rae_mem_hash_insert(ptr, site);
}

static inline void rae_mem_str_untag(void* ptr, int64_t bytes_hint) {
  if (!g_mem_stats_enabled) return;
  uint8_t site = rae_mem_hash_remove(ptr);
  g_mem_site_free_n[site]++;
  /* Prefer the explicit byte count (capacity from the rae_String);
   * fall back to malloc_size for the pool_flush path that only has
   * a ptr. */
  int64_t bytes = bytes_hint > 0 ? bytes_hint : rae_malloc_size_safe(ptr);
  g_mem_site_free_b[site] += bytes;
}

static void rae_mem_stats_print(void) {
  if (!g_mem_stats_enabled) return;
  int64_t tot_an = 0, tot_ab = 0, tot_fn = 0, tot_fb = 0;
  for (int i = 0; i < RAE_SITE__COUNT; i++) {
    tot_an += g_mem_site_alloc_n[i];
    tot_ab += g_mem_site_alloc_b[i];
    tot_fn += g_mem_site_free_n[i];
    tot_fb += g_mem_site_free_b[i];
  }
  fprintf(stderr, "\n[rae mem-stats] cumulative since process start:\n");
  for (int i = 0; i < RAE_SITE__COUNT; i++) {
    if (g_mem_site_alloc_n[i] == 0 && g_mem_site_free_n[i] == 0) continue;
    int64_t out_n = g_mem_site_alloc_n[i] - g_mem_site_free_n[i];
    int64_t out_b = g_mem_site_alloc_b[i] - g_mem_site_free_b[i];
    fprintf(stderr, "  [mem:string:%-13s] alloc=%lld free=%lld outstanding=%lld bytes=%lld (alloc_b=%lld free_b=%lld)\n",
      g_rae_site_names[i],
      (long long)g_mem_site_alloc_n[i], (long long)g_mem_site_free_n[i],
      (long long)out_n, (long long)out_b,
      (long long)g_mem_site_alloc_b[i], (long long)g_mem_site_free_b[i]);
  }
  fprintf(stderr, "  [mem:string:TOTAL          ] alloc=%lld free=%lld outstanding=%lld bytes=%lld\n",
    (long long)tot_an, (long long)tot_fn,
    (long long)(tot_an - tot_fn), (long long)(tot_ab - tot_fb));
  fprintf(stderr, "  [mem:buf               ] alloc=%lld (%lld B) free=%lld (%lld B) outstanding=%lld (%lld B) resize=%lld\n",
    (long long)g_mem_buf_alloc_n, (long long)g_mem_buf_alloc_b,
    (long long)g_mem_buf_free_n,  (long long)g_mem_buf_free_b,
    (long long)(g_mem_buf_alloc_n - g_mem_buf_free_n),
    (long long)(g_mem_buf_alloc_b - g_mem_buf_free_b),
    (long long)g_mem_buf_resize_n);
  fprintf(stderr, "  [mem:pool              ] register=%lld remove=%lld flush_calls=%lld flush_freed=%lld\n",
    (long long)g_mem_pool_register_n, (long long)g_mem_pool_remove_n,
    (long long)g_mem_pool_flush_calls, (long long)g_mem_pool_flush_freed);
  if (g_mem_hash_full_drops) {
    fprintf(stderr, "  [mem:hash              ] WARNING: %lld allocations dropped (table full) — per-site free counts under-report by this much\n",
      (long long)g_mem_hash_full_drops);
  }
  fflush(stderr);
}

__attribute__((constructor))
static void rae_install_mem_stats(void) {
  const char* v = getenv("RAE_MEM_STATS");
  if (v && v[0] && v[0] != '0') {
    g_mem_stats_enabled = 1;
    g_mem_hash_keys  = (void**)  calloc(RAE_MEM_HASH_N, sizeof(void*));
    g_mem_hash_sites = (uint8_t*)calloc(RAE_MEM_HASH_N, sizeof(uint8_t));
    if (!g_mem_hash_keys || !g_mem_hash_sites) {
      fprintf(stderr, "[rae mem-stats] WARNING: failed to allocate %lu-slot hash table; per-site free attribution disabled\n",
        (unsigned long)RAE_MEM_HASH_N);
      free(g_mem_hash_keys);
      free(g_mem_hash_sites);
      g_mem_hash_keys = NULL;
      g_mem_hash_sites = NULL;
    }
    atexit(rae_mem_stats_print);
  }
}

/* Exposed so Rae code can sample the counters mid-run (e.g. the
 * 98_mobile_ui stress loop can print stats at iter 5k / 50k / 100k
 * and compute outstanding-allocations slope per window). No-op when
 * mem-stats is disabled. */
void rae_ext_rae_mem_stats_dump(void) {
  rae_mem_stats_print();
}

/* Returns the total outstanding String allocation count (alloc -
 * free) across all sites. Lets leak-regression tests measure exact
 * heap state instead of indirect RSS, which is noisy when the
 * mem-stats side-hash table is itself allocated. Returns 0 when
 * mem-stats is disabled. */
int64_t rae_ext_rae_mem_stats_outstanding(void) {
  if (!g_mem_stats_enabled) return 0;
  int64_t total = 0;
  for (int i = 0; i < RAE_SITE__COUNT; i++) {
    if (i == RAE_SITE_UNKNOWN) continue;
    total += g_mem_site_alloc_n[i] - g_mem_site_free_n[i];
  }
  return total;
}

/* Internal helpers: allocate a String body with a known call-site
 * tag. The toString helpers (str_i64/str_f64/str_bool/str_char) use
 * these so their allocations don't all get bucketed under "from_buf"
 * or "from_cstr". */
static rae_String rae_str_from_buf_impl(const uint8_t* data, int64_t len, uint8_t site) {
  if (!data || len < 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* buf = malloc(len + 1);
  if (buf) {
    memcpy(buf, data, len);
    buf[len] = '\0';
    rae_mem_str_tag(buf, len + 1, site);
    /* Register with the temp pool so the statement-end / function-end
     * flush sweeps results that the caller doesn't take ownership of.
     * Matches the str_interp contract — pool_take detaches the entry
     * when a binding captures the result, so this never double-frees. */
    rae_string_pool_register(buf);
  }
  return (rae_String){buf, len, len + 1, 1};
}

static rae_String rae_str_from_cstr_impl(const char* s, uint8_t site) {
  if (!s) return (rae_String){NULL, 0, 0, 0};
  int64_t len = (int64_t)strlen(s);
  uint8_t* data = malloc(len + 1);
  if (data) {
    memcpy(data, s, len);
    data[len] = '\0';
    rae_mem_str_tag(data, len + 1, site);
    rae_string_pool_register(data);
  }
  return (rae_String){data, len, len + 1, 1};
}

rae_String rae_ext_rae_str_from_cstr(const void* s) {
  return rae_str_from_cstr_impl((const char*)s, RAE_SITE_FROM_CSTR);
}

rae_String rae_ext_rae_str_from_buf(const uint8_t* data, int64_t len) {
  return rae_str_from_buf_impl(data, len, RAE_SITE_FROM_BUF);
}

void* rae_ext_rae_str_to_cstr(rae_String s) {
  // We ensure rae_String is always NUL-terminated for convenience,
  // but we should still handle the case where it might not be if we ever change that.
  return (void*)s.data;
}

// Free heap memory only when this String owns it. Static literals
// and borrowed views are passed in with is_owned=0 and are no-ops.
// Safe to call on a "moved-from" String (data=NULL, is_owned=0).
// Also unregisters from the temp pool — without this a subsequent
// pool flush in the same statement would double-free a heap we
// already returned to the allocator.
void rae_ext_rae_str_free(rae_String s) {
  if (s.is_owned && s.data) {
    rae_string_pool_remove(s.data);
    rae_mem_str_untag(s.data, s.capacity > 0 ? s.capacity : s.len + 1);
    free(s.data);
  }
}

// Deep-copy. Always returns an owned (heap) String, even when the
// source is borrowed/literal. Use this anywhere Rae's `=` semantics
// require a value copy of a String.
rae_String rae_string_copy(rae_String src) {
  if (!src.data || src.len <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* buf = malloc((size_t)src.len + 1);
  if (!buf) return (rae_String){NULL, 0, 0, 0};
  memcpy(buf, src.data, (size_t)src.len);
  buf[src.len] = '\0';
  rae_mem_str_tag(buf, src.len + 1, RAE_SITE_COPY);
  return (rae_String){buf, src.len, src.len + 1, 1};
}

// ---- String temp pool ----
//
// Statement-scope cleanup of heap allocations from compiler-emitted
// concat/interpolation chains. See header for the contract.
//
// Single-threaded today; if/when Rae grows real threading, this
// becomes per-thread state. The C11 _Thread_local keyword exists
// but we avoid it here to keep the runtime portable to compilers
// that don't ship it cleanly (the Live VM target also uses this
// file).
#define RAE_STRING_POOL_MAX 4096
static void* g_rae_string_pool[RAE_STRING_POOL_MAX];
static int g_rae_string_pool_count = 0;

void rae_string_pool_register(void* ptr) {
  if (!ptr) return;
  if (g_rae_string_pool_count >= RAE_STRING_POOL_MAX) return;
  g_mem_pool_register_n++;
  g_rae_string_pool[g_rae_string_pool_count++] = ptr;
}

int rae_string_pool_mark(void) {
  return g_rae_string_pool_count;
}

void rae_string_pool_flush(int saved) {
  if (saved < 0) saved = 0;
  if (saved > g_rae_string_pool_count) return;  // nothing to flush
  g_mem_pool_flush_calls++;
  for (int i = g_rae_string_pool_count - 1; i >= saved; i--) {
    if (g_rae_string_pool[i]) {
      /* The pool only knows the ptr; the hash lookup recovers the
       * site and rae_malloc_size_safe gives us the byte count. */
      rae_mem_str_untag(g_rae_string_pool[i], 0);
      g_mem_pool_flush_freed++;
      free(g_rae_string_pool[i]);
    }
    g_rae_string_pool[i] = NULL;
  }
  g_rae_string_pool_count = saved;
}

void rae_string_pool_remove(void* ptr) {
  if (!ptr) return;
  g_mem_pool_remove_n++;
  // Linear scan from the top — the entry we want to detach is
  // almost always the most recently registered one (a binding
  // taking the result of the just-emitted interpolation).
  for (int i = g_rae_string_pool_count - 1; i >= 0; i--) {
    if (g_rae_string_pool[i] == ptr) {
      // Compact: swap with last, drop count by 1.
      g_rae_string_pool[i] = g_rae_string_pool[g_rae_string_pool_count - 1];
      g_rae_string_pool_count--;
      return;
    }
  }
}

// Returns 1 if ptr is currently in the statement-scope String temp
// pool, 0 otherwise. Used by rae_string_move_or_copy to decide
// whether a Phase 2 struct-field init can MOVE the source heap or
// must COPY it. A source whose heap is NOT in the pool was either
// pool_take'd by the caller (owning-temp arg detached) or is a
// borrowed/literal-backed value — either way, moving is safe and
// avoids one allocation. A source in the pool will be flushed by
// the caller's surrounding pool_flush, so the callee must deep-
// copy to give its struct field a private heap.
int rae_string_pool_contains(void* ptr) {
  if (!ptr) return 0;
  for (int i = g_rae_string_pool_count - 1; i >= 0; i--) {
    if (g_rae_string_pool[i] == ptr) return 1;
  }
  return 0;
}

rae_String rae_ext_rae_str_interp(int n, ...) {
  if (n <= 0) return (rae_String){NULL, 0, 0, 0};
  va_list args;
  // Collect parts.
  enum { MAX_PARTS = 64 };
  if (n > MAX_PARTS) n = MAX_PARTS;
  rae_String parts[MAX_PARTS];
  int64_t total = 0;
  va_start(args, n);
  for (int i = 0; i < n; i++) {
    parts[i] = va_arg(args, rae_String);
    total += parts[i].len;
  }
  va_end(args);

  uint8_t* buf = (total > 0) ? malloc((size_t)total + 1) : NULL;
  if (buf) {
    int64_t pos = 0;
    for (int i = 0; i < n; i++) {
      if (parts[i].data && parts[i].len > 0) {
        memcpy(buf + pos, parts[i].data, (size_t)parts[i].len);
        pos += parts[i].len;
      }
    }
    buf[total] = '\0';
    rae_mem_str_tag(buf, total + 1, RAE_SITE_INTERP);
  }

  // Free any owned input — these are compiler-generated temporaries
  // (e.g. rae_ext_rae_str_i64 results). Borrowed inputs (literals,
  // rae_string_borrow-wrapped identifiers) are is_owned=0 and a
  // no-op here. The pool_remove keeps the temp-pool in sync so a
  // later flush doesn't double-free a heap we already returned to
  // the allocator.
  for (int i = 0; i < n; i++) {
    if (parts[i].is_owned && parts[i].data) {
      rae_string_pool_remove(parts[i].data);
      rae_mem_str_untag(parts[i].data, parts[i].capacity > 0 ? parts[i].capacity : parts[i].len + 1);
      free(parts[i].data);
    }
  }

  rae_String result = {buf, total, total + 1, 1};
  rae_string_pool_register(buf);
  return result;
}

static int64_t g_tick_counter = 0;

int64_t rae_ext_nextTick(void) {
  return ++g_tick_counter;
}

int64_t rae_ext_nowMs(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

int64_t rae_ext_nowNs(void) {
#ifdef __APPLE__
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) mach_timebase_info(&timebase);
  return (int64_t)((mach_absolute_time() * timebase.numer) / timebase.denom);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

void rae_ext_rae_sleep(int64_t ms) {
  if (ms > 0) {
    usleep((useconds_t)ms * 1000);
  }
}

rae_String rae_ext_formatTimestamp(int64_t epoch_ms) {
  time_t secs = (time_t)(epoch_ms / 1000);
  struct tm tm_buf;
  struct tm* tm_p = gmtime_r(&secs, &tm_buf);
  if (!tm_p) return (rae_String){NULL, 0, 0, 0};
  char buf[32];
  int n = (int)strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_p);
  if (n <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* data = malloc((size_t)n + 1);
  if (!data) return (rae_String){NULL, 0, 0, 0};
  memcpy(data, buf, (size_t)n);
  data[n] = '\0';
  rae_mem_str_tag(data, n + 1, RAE_SITE_FORMAT_TS);
  return (rae_String){data, (int64_t)n, (int64_t)n + 1, 1};
}

rae_String rae_ext_formatDate(int64_t epoch_ms) {
  time_t secs = (time_t)(epoch_ms / 1000);
  struct tm tm_buf;
  struct tm* tm_p = gmtime_r(&secs, &tm_buf);
  if (!tm_p) return (rae_String){NULL, 0, 0, 0};
  char buf[16];
  int n = (int)strftime(buf, sizeof(buf), "%Y-%m-%d", tm_p);
  if (n <= 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* data = malloc((size_t)n + 1);
  if (!data) return (rae_String){NULL, 0, 0, 0};
  memcpy(data, buf, (size_t)n);
  data[n] = '\0';
  rae_mem_str_tag(data, n + 1, RAE_SITE_FORMAT_DATE);
  return (rae_String){data, (int64_t)n, (int64_t)n + 1, 1};
}

void rae_spawn(void* (*func)(void*), void* data) {
#ifdef _WIN32
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, data, 0, NULL);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, func, data) == 0) {
        pthread_detach(thread);
    }
#endif
}

RaeAny rae_ext_json_get(const char* json, const char* field) {
    if (!json || !field) return (RaeAny){RAE_TYPE_NONE, false, false, {0}};
    
    // Tiny naive JSON parser: look for "field": value
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", field);
    const char* key_pos = strstr(json, search);
    if (!key_pos) return (RaeAny){RAE_TYPE_NONE, false, false, {0}};
    
    const char* colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return (RaeAny){RAE_TYPE_NONE, false, false, {0}};
    
    const char* val_start = colon + 1;
    while (*val_start && (*val_start == ' ' || *val_start == '\t' || *val_start == '\n' || *val_start == '\r')) {
        val_start++;
    }
    
    if (*val_start == '\"') {
        // String value
        val_start++;
        const char* val_end = strchr(val_start, '\"');
        if (!val_end) return (RaeAny){RAE_TYPE_NONE, false, false, {0}};
        size_t len = val_end - val_start;
        uint8_t* res = malloc(len + 1);
        memcpy(res, val_start, len);
        res[len] = '\0';
        rae_mem_str_tag(res, (int64_t)len + 1, RAE_SITE_JSON_GET_STR);
        return (RaeAny){RAE_TYPE_STRING, false, false, {.s = {res, (int64_t)len, (int64_t)len + 1, 1}}};
    } else if (*val_start == 't') {
        return (RaeAny){RAE_TYPE_BOOL, false, false, {.b = 1}};
    } else if (*val_start == 'f') {
        return (RaeAny){RAE_TYPE_BOOL, false, false, {.b = 0}};
    } else if (*val_start == 'n') {
        return (RaeAny){RAE_TYPE_NONE, false, false, {0}};
    } else if (*val_start == '-' || (*val_start >= '0' && *val_start <= '9')) {
        // Number
        char* end;
        double f = strtod(val_start, &end);
        if (strchr(val_start, '.') && strchr(val_start, '.') < end) {
            return (RaeAny){RAE_TYPE_FLOAT64, false, false, {.f = f}};
        } else {
            return (RaeAny){RAE_TYPE_INT64, false, false, {.i = (int64_t)f}};
        }
    } else if (*val_start == '{') {
        // Nested object (simplified: just return the raw string part)
        int depth = 1;
        const char* p = val_start + 1;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        size_t len = p - val_start;
        uint8_t* res = malloc(len + 1);
        memcpy(res, val_start, len);
        res[len] = '\0';
        rae_mem_str_tag(res, (int64_t)len + 1, RAE_SITE_JSON_GET_OBJ);
        return (RaeAny){RAE_TYPE_STRING, false, false, {.s = {res, (int64_t)len, (int64_t)len + 1, 1}}}; // We return objects as strings for now
    }
    
    return (RaeAny){RAE_TYPE_NONE, false, false, {0}};
}

void rae_ext_rae_log_any(RaeAny value) {
  if (value.is_view) printf("view ");
  else if (value.is_mod) printf("mod ");
  rae_ext_rae_log_stream_any(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_any(RaeAny value) {
  if (value.type == RAE_TYPE_ANY) {
      // If it's a reference to an Any, dereference and recurse
      RaeAny inner = *(RaeAny*)value.as.ptr;
      if (value.is_view) inner.is_view = true;
      if (value.is_mod) inner.is_mod = true;
      rae_ext_rae_log_stream_any(inner);
      return;
  }

  bool is_ref = value.is_view || value.is_mod;

  switch (value.type) {
    case RAE_TYPE_INT64: {
        int64_t v = is_ref ? *(int64_t*)value.as.ptr : value.as.i;
        printf("%lld", (long long)v); 
        break;
    }
    case RAE_TYPE_INT32: {
        int32_t v = is_ref ? *(int32_t*)value.as.ptr : (int32_t)value.as.i;
        printf("%d", v); 
        break;
    }
    case RAE_TYPE_UINT64: {
        uint64_t v = is_ref ? *(uint64_t*)value.as.ptr : (uint64_t)value.as.i;
        printf("%llu", (unsigned long long)v); 
        break;
    }
    case RAE_TYPE_FLOAT64: {
        double v = is_ref ? *(double*)value.as.ptr : value.as.f;
        printf("%g", v); 
        break;
    }
    case RAE_TYPE_FLOAT32: {
        float v = is_ref ? *(float*)value.as.ptr : (float)value.as.f;
        printf("%g", (double)v); 
        break;
    }
    case RAE_TYPE_BOOL: {
        int8_t v = is_ref ? *(int8_t*)value.as.ptr : value.as.b;
        printf("%s", v ? "true" : "false"); 
        break;
    }
    case RAE_TYPE_STRING: {
        rae_String v = is_ref ? *(rae_String*)value.as.ptr : value.as.s;
        if (v.data) fwrite(v.data, 1, v.len, stdout);
        else printf("(null)"); 
        break;
    }
    case RAE_TYPE_CHAR: {
        uint32_t v = is_ref ? *(uint32_t*)value.as.ptr : (uint32_t)value.as.i;
        rae_ext_rae_log_stream_char(v); 
        break;
    }
    case RAE_TYPE_ID: printf("Id(%lld)", (long long)value.as.i); break;
    case RAE_TYPE_KEY: {
        rae_String v = is_ref ? *(rae_String*)value.as.ptr : value.as.s;
        printf("Key(\"");
        if (v.data) fwrite(v.data, 1, v.len, stdout);
        printf("\")");
        break;
    }
    case RAE_TYPE_UINT32: {
        uint32_t v = is_ref ? *(uint32_t*)value.as.ptr : (uint32_t)value.as.i;
        printf("%u", v); 
        break;
    }
    case RAE_TYPE_LIST: printf("[...]"); break;
    case RAE_TYPE_BUFFER: printf("%p", value.as.ptr); break;
    case RAE_TYPE_NONE: printf("none"); break;
    case RAE_TYPE_ANY: printf("Any(...)"); break;
  }
}

void rae_ext_rae_log_list_fields(RaeAny* items, int64_t length, int64_t capacity) {
  rae_ext_rae_log_stream_list_fields(items, length, capacity);
  printf("\n");
  fflush(stdout);
}

void rae_ext_rae_log_stream_list_fields(RaeAny* items, int64_t length, int64_t capacity) {
  printf("{ #(");
  for (int64_t i = 0; i < capacity; i++) {
    if (i > 0) printf(", ");
    if (i < length) {
      rae_ext_rae_log_stream_any(items[i]);
    } else {
      printf("none");
    }
  }
  printf("), %lld, %lld }", (long long)length, (long long)capacity);
}

void rae_ext_rae_log_stream_list_typed(void* data, int64_t length, int64_t capacity, int elem_kind) {
  printf("{ #(");
  for (int64_t i = 0; i < capacity; i++) {
    if (i > 0) printf(", ");
    if (i >= length) { printf("none"); continue; }
    switch (elem_kind) {
      case RAE_LIST_ELEM_ANY:    rae_ext_rae_log_stream_any(((RaeAny*)data)[i]); break;
      case RAE_LIST_ELEM_INT64:  printf("%lld", (long long)((int64_t*)data)[i]); break;
      case RAE_LIST_ELEM_FLOAT64:printf("%g", ((double*)data)[i]); break;
      case RAE_LIST_ELEM_BOOL:   printf("%s", ((bool*)data)[i] ? "true" : "false"); break;
      case RAE_LIST_ELEM_CHAR32: {
        uint32_t cp = ((uint32_t*)data)[i];
        // Encode as UTF-8 for printable codepoints, else fall back to U+XXXX.
        if (cp < 0x80) { putchar((int)cp); }
        else if (cp < 0x800) { putchar(0xC0 | (cp >> 6)); putchar(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { putchar(0xE0 | (cp >> 12)); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
        else { putchar(0xF0 | (cp >> 18)); putchar(0x80 | ((cp >> 12) & 0x3F)); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
        break;
      }
      case RAE_LIST_ELEM_STRING: {
        rae_String s = ((rae_String*)data)[i];
        printf("\"%.*s\"", (int)s.len, (const char*)s.data);
        break;
      }
      default: printf("?"); break;
    }
  }
  printf("), %lld, %lld }", (long long)length, (long long)capacity);
}

void rae_ext_rae_log_list_typed(void* data, int64_t length, int64_t capacity, int elem_kind) {
  rae_ext_rae_log_stream_list_typed(data, length, capacity, elem_kind);
  printf("\n");
  fflush(stdout);
}

void rae_ext_rae_log_cstr(const char* text) {
  if (!text) {
    fputs("(null)\n", stdout);
    rae_flush_stdout();
    return;
  }
  fputs(text, stdout);
  fputc('\n', stdout);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_cstr(const char* text) {
  if (!text) {
    return;
  }
  fputs(text, stdout);
  rae_flush_stdout();
}

void rae_ext_rae_log_string(rae_String value) {
  rae_ext_rae_log_stream_string(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_string(rae_String value) {
  if (value.data) {
    fwrite(value.data, 1, value.len, stdout);
  }
  rae_flush_stdout();
}

void rae_ext_rae_log_i64(int64_t value) {
  printf("%lld\n", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_i64(int64_t value) {
  printf("%lld", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_bool(int8_t value) {
  printf("%s\n", value ? "true" : "false");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_bool(int8_t value) {
  printf("%s", value ? "true" : "false");
  rae_flush_stdout();
}

void rae_ext_rae_log_char(uint32_t value) {
  rae_ext_rae_log_stream_char(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_char(uint32_t value) {
  if (value < 0x80) {
    printf("%c", (char)value);
  } else if (value < 0x800) {
    printf("%c%c", (char)(0xC0 | (value >> 6)), (char)(0x80 | (value & 0x3F)));
  } else if (value < 0x10000) {
    printf("%c%c%c", (char)(0xE0 | (value >> 12)), (char)(0x80 | ((value >> 6) & 0x3F)), (char)(0x80 | (value & 0x3F)));
  } else {
    printf("%c%c%c%c", (char)(0xF0 | (value >> 18)), (char)(0x80 | ((value >> 12) & 0x3F)), (char)(0x80 | ((value >> 6) & 0x3F)), (char)(0x80 | (value & 0x3F)));
  }
  rae_flush_stdout();
}

void rae_ext_rae_log_id(int64_t value) {
  printf("%lld\n", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_id(int64_t value) {
  printf("%lld", (long long)value);
  rae_flush_stdout();
}

void rae_ext_rae_log_key(rae_String value) {
  rae_ext_rae_log_stream_string(value);
  printf("\n");
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_key(rae_String value) {
  rae_ext_rae_log_stream_string(value);
}

void rae_ext_rae_log_float(double value) {
  printf("%g\n", value);
  rae_flush_stdout();
}

void rae_ext_rae_log_stream_float(double value) {
  printf("%g", value);
  rae_flush_stdout();
}

rae_String rae_ext_rae_str_concat(rae_String a, rae_String b) {
  int64_t len_a = a.len;
  int64_t len_b = b.len;
  uint8_t* result_data = malloc(len_a + len_b + 1);
  if (result_data) {
    if (a.data) memcpy(result_data, a.data, len_a);
    if (b.data) memcpy(result_data + len_a, b.data, len_b);
    result_data[len_a + len_b] = '\0';
    rae_mem_str_tag(result_data, len_a + len_b + 1, RAE_SITE_CONCAT);
    /* Register with the temp pool, matching the str_interp contract.
     * Callers that bind the result (let / assign / ret) detach via
     * rae_string_pool_take; transient uses (foo(a + b) at expr-stmt
     * scope) get swept by the surrounding mark/flush. Without this
     * line, every concat result whose owner doesn't explicitly call
     * str_free leaks until process exit. */
    rae_string_pool_register(result_data);
  }
  return (rae_String){result_data, len_a + len_b, len_a + len_b + 1, 1};
}

rae_String rae_ext_rae_str_concat_cstr(rae_String a, rae_String b) {
  return rae_ext_rae_str_concat(a, b);
}

int64_t rae_ext_rae_str_len(rae_String s) {
  return s.len;
}

int64_t rae_ext_rae_str_compare(rae_String a, rae_String b) {
  if (a.len < b.len) {
    int res = memcmp(a.data, b.data, a.len);
    return res == 0 ? -1 : res;
  } else if (a.len > b.len) {
    int res = memcmp(a.data, b.data, b.len);
    return res == 0 ? 1 : res;
  } else {
    return memcmp(a.data, b.data, a.len);
  }
}

rae_Bool rae_ext_rae_str_eq(rae_String a, rae_String b) {
  if (a.len != b.len) return false;
  if (a.len == 0) return true;
  return memcmp(a.data, b.data, a.len) == 0;
}

int64_t rae_ext_rae_str_hash(rae_String s) {
  if (!s.data) return 0;
  // FNV-1a hash
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (int64_t i = 0; i < s.len; i++) {
    hash ^= (uint64_t)s.data[i];
    hash *= 0x100000001b3ULL;
  }
  return (int64_t)hash;
}

rae_String rae_ext_rae_str_sub(rae_String s, int64_t start, int64_t len) {
  if (!s.data) return (rae_String){NULL, 0, 0, 0};
  if (start < 0) start = 0;
  if (start >= s.len) return (rae_String){NULL, 0, 0, 0};
  if (start + len > s.len) len = s.len - start;
  if (len <= 0) return (rae_String){NULL, 0, 0, 0};

  uint8_t* result_data = malloc((size_t)len + 1);
  if (result_data) {
    memcpy(result_data, s.data + start, (size_t)len);
    result_data[len] = '\0';
    rae_mem_str_tag(result_data, len + 1, RAE_SITE_SUB);
  }
  return (rae_String){result_data, len, len + 1, 1};
}

rae_Bool rae_ext_rae_str_contains(rae_String s, rae_String sub) {
  if (!s.data || !sub.data) return false;
  if (sub.len == 0) return true;
  if (sub.len > s.len) return false;
  // Naive search because we don't necessarily have NUL termination at the right place if it's a subslice
  // But we DO ensure NUL termination in our helpers.
  return strstr((const char*)s.data, (const char*)sub.data) != NULL;
}

rae_Bool rae_ext_rae_str_starts_with(rae_String s, rae_String prefix) {
  if (prefix.len > s.len) return false;
  if (prefix.len == 0) return true;
  return memcmp(s.data, prefix.data, prefix.len) == 0;
}

rae_Bool rae_ext_rae_str_ends_with(rae_String s, rae_String suffix) {
  if (suffix.len > s.len) return false;
  if (suffix.len == 0) return true;
  return memcmp(s.data + s.len - suffix.len, suffix.data, suffix.len) == 0;
}

int64_t rae_ext_rae_str_index_of(rae_String s, rae_String sub) {
  if (!s.data || !sub.data) return -1;
  if (sub.len == 0) return 0;
  const char* p = strstr((const char*)s.data, (const char*)sub.data);
  if (!p) return -1;
  return (int64_t)(p - (const char*)s.data);
}

rae_String rae_ext_rae_str_to_lower(rae_String s) {
  if (!s.data || s.len == 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* out = malloc(s.len + 1);
  if (!out) return (rae_String){NULL, 0, 0, 0};
  for (int64_t i = 0; i < s.len; i++) {
    uint8_t c = s.data[i];
    if (c >= 'A' && c <= 'Z') {
      out[i] = c + ('a' - 'A');
    } else {
      out[i] = c;
    }
  }
  out[s.len] = '\0';
  rae_mem_str_tag(out, s.len + 1, RAE_SITE_CONCAT);
  rae_string_pool_register(out);
  return (rae_String){out, s.len, s.len + 1, 1};
}

rae_String rae_ext_rae_str_trim(rae_String s) {
  if (!s.data || s.len == 0) return (rae_String){NULL, 0, 0, 0};
  int64_t start = 0;
  while (start < s.len && (s.data[start] == ' ' || s.data[start] == '\t' || s.data[start] == '\n' || s.data[start] == '\r')) start++;
  if (start == s.len) return (rae_String){NULL, 0, 0, 0};
  int64_t end = s.len - 1;
  while (end > start && (s.data[end] == ' ' || s.data[end] == '\t' || s.data[end] == '\n' || s.data[end] == '\r')) end--;
  return rae_ext_rae_str_sub(s, start, end - start + 1);
}

uint32_t rae_ext_rae_str_at(rae_String s, int64_t index) {
  if (!s.data || index < 0 || index >= s.len) return 0;
  uint8_t c = s.data[index];
  if (c < 0x80) return (uint32_t)c;
  if ((c & 0xE0) == 0xC0) {
    if (index + 1 >= s.len) return (uint32_t)c;
    return (uint32_t)(((c & 0x1F) << 6) | (s.data[index+1] & 0x3F));
  }
  if ((c & 0xF0) == 0xE0) {
    if (index + 2 >= s.len) return (uint32_t)c;
    return (uint32_t)(((c & 0x0F) << 12) | ((s.data[index+1] & 0x3F) << 6) | (s.data[index+2] & 0x3F));
  }
  if ((c & 0xF8) == 0xF0) {
    if (index + 3 >= s.len) return (uint32_t)c;
    return (uint32_t)(((c & 0x07) << 18) | ((s.data[index+1] & 0x3F) << 12) | ((s.data[index+2] & 0x3F) << 6) | (s.data[index+3] & 0x3F));
  }
  return (uint32_t)c;
}

double rae_ext_rae_str_to_f64(rae_String s) {
  if (!s.data) return 0.0;
  return atof((const char*)s.data);
}

int64_t rae_ext_rae_str_to_i64(rae_String s) {
  if (!s.data) return 0;
  return (int64_t)atoll((const char*)s.data);
}

rae_String rae_ext_rae_io_read_line(void) {
  char* buffer = NULL;
  size_t len = 0;
  if (getline(&buffer, &len, stdin) == -1) {
    free(buffer);
    return (rae_String){NULL, 0, 0, 0};
  }
  // Remove newline
  size_t blen = strlen(buffer);
  if (blen > 0 && buffer[blen-1] == '\n') {
      buffer[blen-1] = '\0';
      blen--;
  }
  if (blen > 0 && buffer[blen-1] == '\r') {
      buffer[blen-1] = '\0';
      blen--;
  }
  rae_mem_str_tag(buffer, (int64_t)len, RAE_SITE_READ_LINE);
  return (rae_String){(uint8_t*)buffer, (int64_t)blen, (int64_t)len, 1};
}

rae_Char rae_ext_rae_io_read_char(void) {
  // TODO: Proper UTF-8 read from stdin
  return (uint32_t)getchar();
}

void rae_ext_rae_sys_exit(int64_t code) {
  exit((int)code);
}

rae_String rae_ext_rae_sys_get_env(rae_String name) {
  if (!name.data) return (rae_String){NULL, 0, 0, 0};
  const char* val = getenv((const char*)name.data);
  return rae_ext_rae_str_from_cstr((void*)val);
}

rae_String rae_ext_rae_sys_read_file(rae_String path) {
  if (!path.data) return (rae_String){NULL, 0, 0, 0};
  FILE* f = fopen((const char*)path.data, "rb");
  if (!f) return (rae_String){NULL, 0, 0, 0};
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buffer = malloc((size_t)len + 1);
  if (buffer) {
    fread(buffer, 1, (size_t)len, f);
    buffer[len] = '\0';
    rae_mem_str_tag(buffer, (int64_t)len + 1, RAE_SITE_READ_FILE);
  }
  fclose(f);
  return (rae_String){buffer, (int64_t)len, (int64_t)len + 1, 1};
}

rae_Bool rae_ext_rae_sys_write_file(rae_String path, rae_String content) {
  if (!path.data || !content.data) return false;
  FILE* f = fopen((const char*)path.data, "wb");
  if (!f) return false;
  size_t written = fwrite(content.data, 1, (size_t)content.len, f);
  fclose(f);
  return written == (size_t)content.len;
}

rae_Bool rae_ext_rae_sys_rename(rae_String oldPath, rae_String newPath) {
    if (!oldPath.data || !newPath.data) return false;
    return rename((const char*)oldPath.data, (const char*)newPath.data) == 0;
}

rae_Bool rae_ext_rae_sys_delete(rae_String path) {
    if (!path.data) return false;
    return remove((const char*)path.data) == 0;
}

rae_Bool rae_ext_rae_sys_make_dir(rae_String path) {
    if (!path.data) return false;
    // mkdir(2) returns -1 with EEXIST when the dir already exists; we
    // treat that as success so callers can use this idempotently.
    if (mkdir((const char*)path.data, 0755) == 0) return true;
    return errno == EEXIST;
}

#include <sys/file.h> // For flock

rae_Bool rae_ext_rae_sys_exists(rae_String path) {
    if (!path.data) return false;
    return access((const char*)path.data, F_OK) == 0;
}

rae_Bool rae_ext_rae_sys_lock_file(rae_String path) {
    if (!path.data) return false;
    int fd = open((const char*)path.data, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return false;
    }
    // Note: We are leaking the FD here for simplicity in this prototype.
    // In a real implementation, we'd need a way to track and close it.
    return true;
}

rae_Bool rae_ext_rae_sys_unlock_file(rae_String path) {
    if (!path.data) return false;
    int fd = open((const char*)path.data, O_RDWR);
    if (fd < 0) return false;
    flock(fd, LOCK_UN);
    close(fd);
    return true;
}

int64_t rae_ext_rae_sys_rss_kb(void) {
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return (int64_t)(info.resident_size / 1024);
    }
    return -1;
#elif defined(__linux__)
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    int64_t rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            rss = atoll(line + 6);
            break;
        }
    }
    fclose(f);
    return rss;
#else
    return -1;
#endif
}

/* Non-recursive directory scan. Returns a newline-separated list of
 * entry names (excluding "." and ".."), or the empty string when the
 * directory can't be opened. Caller-side `lib/fs.rae::listDir` splits
 * this back into `List(File)`. Names are returned in the order the
 * OS yields them — POSIX makes no guarantee, and APFS in particular
 * returns entries in insertion order rather than sorted. */
rae_String rae_ext_rae_sys_list_dir(rae_String folder) {
  if (!folder.data) return (rae_String){NULL, 0, 0, 0};
  DIR* dir = opendir((const char*)folder.data);
  if (!dir) return (rae_String){NULL, 0, 0, 0};

  /* Grow a heap buffer as we append entries with '\n' separators.
   * Keeps the function single-pass (no readdir count then re-read). */
  size_t cap = 256;
  size_t len = 0;
  uint8_t* buf = malloc(cap);
  if (!buf) { closedir(dir); return (rae_String){NULL, 0, 0, 0}; }

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    const char* name = entry->d_name;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue; /* skip . and .. */
    }
    size_t nameLen = strlen(name);
    size_t need = len + nameLen + 1; /* +1 for separator newline */
    if (need > cap) {
      while (need > cap) cap *= 2;
      uint8_t* grown = realloc(buf, cap);
      if (!grown) { free(buf); closedir(dir); return (rae_String){NULL, 0, 0, 0}; }
      buf = grown;
    }
    if (len > 0) {
      buf[len++] = '\n';
    }
    memcpy(buf + len, name, nameLen);
    len += nameLen;
  }
  closedir(dir);

  if (len == 0) { free(buf); return (rae_String){NULL, 0, 0, 0}; }

  /* Null-terminate and register with the string pool so the caller's
   * Rae-side `let raw: String = ...` gets the same lifetime semantics
   * as any other owning String. */
  uint8_t* finalBuf = realloc(buf, len + 1);
  if (!finalBuf) finalBuf = buf;
  finalBuf[len] = '\0';
  rae_mem_str_tag(finalBuf, len + 1, RAE_SITE_CONCAT);
  rae_string_pool_register(finalBuf);
  return (rae_String){finalBuf, (int64_t)len, (int64_t)(len + 1), 1};
}

double rae_ext_rae_sys_file_mtime(rae_String path) {
    if (!path.data) return 0.0;
    struct stat st;
    if (stat((const char*)path.data, &st) != 0) return 0.0;
#if defined(__APPLE__)
    return (double)st.st_mtimespec.tv_sec + (double)st.st_mtimespec.tv_nsec / 1.0e9;
#elif defined(__linux__)
    return (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1.0e9;
#else
    return (double)st.st_mtime;
#endif
}

rae_String rae_ext_rae_str_i64(int64_t v) {
  char buffer[32];
  int len = snprintf(buffer, 32, "%lld", (long long)v);
  return rae_str_from_buf_impl((uint8_t*)buffer, len, RAE_SITE_INT_TO_STR);
}

rae_String rae_ext_rae_str_i64_ptr(const int64_t* v) {
  return rae_ext_rae_str_i64(*v);
}

rae_String rae_ext_rae_str_f64(double v) {
  char buffer[32];
  int len = snprintf(buffer, 32, "%g", v);
  return rae_str_from_buf_impl((uint8_t*)buffer, len, RAE_SITE_FLOAT_TO_STR);
}

rae_String rae_ext_rae_str_f64_ptr(const double* v) {
  return rae_ext_rae_str_f64(*v);
}

rae_String rae_ext_rae_str_bool(rae_Bool v) {
  return rae_str_from_cstr_impl(v ? "true" : "false", RAE_SITE_BOOL_TO_STR);
}

rae_String rae_ext_rae_str_bool_ptr(const rae_Bool* v) {
  return rae_ext_rae_str_bool(*v);
}

rae_String rae_ext_rae_str_char(uint32_t v) {
  uint8_t buffer[5];
  int len = 0;
  if (v < 0x80) {
    buffer[0] = (uint8_t)v;
    len = 1;
  } else if (v < 0x800) {
    buffer[0] = (uint8_t)(0xC0 | (v >> 6));
    buffer[1] = (uint8_t)(0x80 | (v & 0x3F));
    len = 2;
  } else if (v < 0x10000) {
    buffer[0] = (uint8_t)(0xE0 | (v >> 12));
    buffer[1] = (uint8_t)(0x80 | ((v >> 6) & 0x3F));
    buffer[2] = (uint8_t)(0x80 | (v & 0x3F));
    len = 3;
  } else {
    buffer[0] = (uint8_t)(0xF0 | (v >> 18));
    buffer[1] = (uint8_t)(0x80 | ((v >> 12) & 0x3F));
    buffer[2] = (uint8_t)(0x80 | ((v >> 6) & 0x3F));
    buffer[3] = (uint8_t)(0x80 | (v & 0x3F));
    len = 4;
  }
  buffer[len] = '\0';
  return rae_str_from_buf_impl(buffer, len, RAE_SITE_CHAR_TO_STR);
}

rae_String rae_ext_rae_str_char_ptr(const uint32_t* v) {
    return rae_ext_rae_str_char(*v);
}

rae_String rae_ext_rae_str_string(rae_String s) {
    return rae_str_from_buf_impl(s.data, s.len, RAE_SITE_STR_STRING);
}

rae_String rae_ext_rae_str_any(RaeAny v) {
    // Format a boxed value as a Rae string. Handles `none` and primitive types
    // by delegating to the typed formatters; defaults to empty for structs and
    // other heap types (those should be formatted via toString instead).
    switch (v.type) {
        case RAE_TYPE_INT64:   return rae_ext_rae_str_i64(v.as.i);
        case RAE_TYPE_INT32:   return rae_ext_rae_str_i64(v.as.i);
        case RAE_TYPE_UINT64:  return rae_ext_rae_str_i64(v.as.i);
        case RAE_TYPE_FLOAT64: return rae_ext_rae_str_f64(v.as.f);
        case RAE_TYPE_FLOAT32: return rae_ext_rae_str_f64(v.as.f);
        case RAE_TYPE_BOOL:    return rae_ext_rae_str_bool(v.as.b);
        // Extracting a String from a RaeAny is a read, not an
        // ownership transfer — the RaeAny boxed a copy of someone
        // else's String (e.g. a list element). Always return a
        // borrowed view (is_owned=0) so a consumer like
        // rae_ext_rae_str_interp doesn't free the storage that
        // still belongs to the original owner.
        case RAE_TYPE_STRING:  return rae_string_borrow(v.as.s);
        case RAE_TYPE_CHAR:    return rae_ext_rae_str_char((uint32_t)v.as.i);
        case RAE_TYPE_NONE:    return (rae_String){(uint8_t*)"none", 4, 0, 0};
        default:               return (rae_String){(uint8_t*)"", 0, 0, 0};
    }
}

rae_String rae_ext_rae_str_string_ptr(const rae_String* s) {
    return rae_ext_rae_str_from_buf(s->data, s->len);
}

rae_String rae_ext_rae_str_cstr(const char* s) {
  return rae_ext_rae_str_from_cstr((void*)s);
}

rae_String rae_ext_rae_str_cstr_ptr(const char** s) {
  return rae_ext_rae_str_from_cstr((void*)*s);
}

static uint64_t g_rae_random_state = 0x123456789ABCDEF0ULL;

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
  if (!buf) return (RaeAny){0};
  RAE_BR_CHECK_ANY("rae_buf_get_any", buf, index);
  return ((RaeAny*)buf)[index];
}

double rae_ext_rae_math_sin(double x) { return sin(x); }
double rae_ext_rae_math_cos(double x) { return cos(x); }
double rae_ext_rae_math_tan(double x) { return tan(x); }
double rae_ext_rae_math_asin(double x) { return asin(x); }
double rae_ext_rae_math_acos(double x) { return acos(x); }
double rae_ext_rae_math_atan(double x) { return atan(x); }
double rae_ext_rae_math_atan2(double y, double x) { return atan2(y, x); }
double rae_ext_rae_math_sqrt(double x) { return sqrt(x); }
double rae_ext_rae_math_pow(double base, double exp) { return pow(base, exp); }
double rae_ext_rae_math_exp(double x) { return exp(x); }
double rae_ext_rae_math_log(double x) { return log(x); }
double rae_ext_rae_math_floor(double x) { return floor(x); }
double rae_ext_rae_math_ceil(double x) { return ceil(x); }
double rae_ext_rae_math_round(double x) { return round(x); }

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

#if defined(__APPLE__)
/* App Nap opt-out. macOS throttles event delivery to processes
 * with low idle CPU once App Nap engages — observed in the field
 * via `sample <pid>` showing RunningBoardServices'
 * `rbs_acquire_appnap_assertion` on the main run loop. Throttled
 * apps still wake on bursts (mouse drag, scroll) but coalesce
 * single discrete events for seconds, exactly the "stops accepting
 * input" pattern users hit.
 *
 * Fix: call `[[NSProcessInfo processInfo] beginActivityWithOptions:
 * reason:]` with `NSActivityUserInitiated` (0x00FFFFFFULL). The
 * returned activity object must be retained for the process
 * lifetime; we stash it in a static and never release.
 *
 * Implementation goes through `objc_msgSend` directly so we don't
 * need an Objective-C compilation unit — the runtime is already
 * linked via the Cocoa framework (raylib apps pull it in) and via
 * Foundation otherwise. Lives OUTSIDE the `RAE_HAS_RAYLIB` block
 * because it's process-level, not raylib-specific; the symbol has
 * to be defined in both the rae driver build and compiled-target
 * builds so `vm_raylib.c`'s native wrapper can link. */
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>
static void* g_appnap_activity = NULL;
void rae_ext_disableAppNap(void) {
  if (g_appnap_activity != NULL) return;
  Class npClass = objc_getClass("NSProcessInfo");
  if (!npClass) {
    fprintf(stderr, "[app-nap] NSProcessInfo class not found\n");
    return;
  }
  SEL piSel = sel_registerName("processInfo");
  SEL beginSel = sel_registerName("beginActivityWithOptions:reason:");
  void* processInfo = ((void* (*)(void*, SEL))objc_msgSend)((void*)npClass, piSel);
  if (!processInfo) {
    fprintf(stderr, "[app-nap] processInfo nil\n");
    return;
  }
  uint64_t options = 0x00FFFFFFULL;
  CFStringRef reason = CFSTR("Rae interactive UI loop");
  void* activity = ((void* (*)(void*, SEL, uint64_t, void*))objc_msgSend)(
    processInfo, beginSel, options, (void*)reason);
  if (activity) {
    SEL retainSel = sel_registerName("retain");
    ((void* (*)(void*, SEL))objc_msgSend)(activity, retainSel);
    g_appnap_activity = activity;
    fprintf(stderr, "[app-nap] disabled (activity=%p)\n", activity);
  } else {
    fprintf(stderr, "[app-nap] beginActivity returned nil\n");
  }
  fflush(stderr);
}

/* Bring THIS process's window to the foreground:
 * `[[NSApplication sharedApplication] activateIgnoringOtherApps:YES]`.
 *
 * Used after controlling Spotify (which can pull Spotify forward) so
 * focus returns to our own window and clicks keep landing. We activate
 * OURSELVES rather than "whatever was frontmost before the play": the
 * latter (via System Events `set frontmost`) reliably hands focus to
 * the launching app — SUMU when run from the dev tools — which drops us
 * out of the foreground and macOS then demotes us to a background
 * process (no Dock entry, unfocusable, dead close button). Activating
 * self has no such failure mode. Pure objc_msgSend; no ObjC unit. */
void rae_ext_activateSelf(void) {
  Class appCls = objc_getClass("NSApplication");
  if (!appCls) return;
  SEL sharedSel = sel_registerName("sharedApplication");
  void* app = ((void* (*)(Class, SEL))objc_msgSend)(appCls, sharedSel);
  if (!app) return;
  SEL actSel = sel_registerName("activateIgnoringOtherApps:");
  ((void (*)(void*, SEL, signed char))objc_msgSend)(app, actSel, (signed char)1);
}
#else
void rae_ext_disableAppNap(void) {
  /* No-op outside macOS — App Nap is a macOS-specific power
   * management feature. */
}
void rae_ext_activateSelf(void) {
  /* No-op outside macOS. */
}
#endif

#ifdef RAE_HAS_RAYLIB
/* Raylib wrappers for C backend */
#include <raylib.h>
#include <rlgl.h>
#if defined(__APPLE__)
/* Apple deprecated the entire OpenGL framework in 10.14 in favour
 * of Metal, but the symbols still link and work. raylib itself goes
 * through OpenGL → Metal under the hood here. Silence the noise. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

/* Phase 0 of the color-management plan (docs/color-management-plan.md).
 *
 * Raylib hands the OS a raw OpenGL framebuffer and walks away —
 * macOS WindowServer then interprets those bytes through whatever
 * the host display's profile is (Display P3 on every modern Mac).
 * sRGB-encoded PNG content displayed as P3 looks visibly over-
 * saturated, especially on stylised game art with bright primaries.
 *
 * The fix until the Metal backend lands is to tell macOS the
 * window's framebuffer is sRGB. WindowServer then does the right
 * sRGB → P3 conversion for free. Drives the existing Cocoa colour-
 * management path that Preview, Safari and Chrome already use.
 *
 * Pure C, no Objective-C compiler step: routes through the objc
 * runtime's C API (`objc_msgSend` etc.). Apple-only — non-Apple
 * builds compile this away. */
#ifdef __APPLE__
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
static void rae_macos_set_window_srgb(void) {
    Class app_cls = objc_getClass("NSApplication");
    if (!app_cls) return;
    SEL shared_sel = sel_registerName("sharedApplication");
    id app = ((id (*)(Class, SEL))objc_msgSend)(app_cls, shared_sel);
    if (!app) return;
    SEL windows_sel = sel_registerName("windows");
    id windows = ((id (*)(id, SEL))objc_msgSend)(app, windows_sel);
    if (!windows) return;
    Class space_cls = objc_getClass("NSColorSpace");
    if (!space_cls) return;
    SEL srgb_sel = sel_registerName("sRGBColorSpace");
    id srgb = ((id (*)(Class, SEL))objc_msgSend)(space_cls, srgb_sel);
    if (!srgb) return;
    SEL count_sel = sel_registerName("count");
    SEL at_idx_sel = sel_registerName("objectAtIndex:");
    SEL set_space_sel = sel_registerName("setColorSpace:");
    unsigned long n = ((unsigned long (*)(id, SEL))objc_msgSend)(windows, count_sel);
    for (unsigned long i = 0; i < n; i++) {
        id w = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, at_idx_sel, i);
        if (w) ((void (*)(id, SEL, id))objc_msgSend)(w, set_space_sel, srgb);
    }
}
#else
static void rae_macos_set_window_srgb(void) { /* no-op on non-Apple */ }
#endif

void rae_ext_initWindow(int64_t width, int64_t height, rae_String title) {
    InitWindow((int)width, (int)height, (const char*)title.data);
    rae_macos_set_window_srgb();
}

void rae_ext_setConfigFlags(int64_t flags) {
    SetConfigFlags((unsigned int)flags);
}

void rae_ext_drawCubeWires(Vector3 pos, double width, double height, double length, Color color) {
    DrawCubeWires(pos, (float)width, (float)height, (float)length, color);
}

void rae_ext_drawSphere(Vector3 centerPos, double radius, Color color) {
    DrawSphere(centerPos, (float)radius, color);
}

void rae_ext_drawCircle(double x, double y, double radius, Color color) {
    DrawCircle((int)x, (int)y, (float)radius, color);
}

void rae_ext_drawCircleGradient(int64_t x, int64_t y, double radius, Color color1, Color color2) {
    DrawCircleGradient((int)x, (int)y, (float)radius, color1, color2);
}

void rae_ext_drawRectangleGradientV(int64_t x, int64_t y, int64_t width, int64_t height, Color color1, Color color2) {
    DrawRectangleGradientV((int)x, (int)y, (int)width, (int)height, color1, color2);
}

void rae_ext_drawRectangleGradientH(int64_t x, int64_t y, int64_t width, int64_t height, Color color1, Color color2) {
    DrawRectangleGradientH((int)x, (int)y, (int)width, (int)height, color1, color2);
}

double rae_ext_getTime(void) {
    return GetTime();
}

Color rae_ext_colorFromHSV(double hue, double saturation, double value) {
    return ColorFromHSV((float)hue, (float)saturation, (float)value);
}

void rae_ext_takeScreenshot(rae_String fileName) {
    TakeScreenshot((const char*)fileName.data);
}

void rae_ext_drawCylinder(Vector3 position, double radiusTop, double radiusBottom, double height, int64_t slices, Color color) {
    DrawCylinder(position, (float)radiusTop, (float)radiusBottom, (float)height, (int)slices, color);
}

void rae_ext_drawGrid(int64_t slices, double spacing) {
    DrawGrid((int)slices, (float)spacing);
}

void rae_ext_beginMode3D(Camera3D camera) {
    BeginMode3D(camera);
}

void rae_ext_endMode3D(void) {
    EndMode3D();
}

void rae_ext_beginMode2D(Camera2D camera) {
    BeginMode2D(camera);
}

void rae_ext_endMode2D(void) {
    EndMode2D();
}

void rae_ext_drawRectangle(double x, double y, double width, double height, Color color) {
    DrawRectangle((int)x, (int)y, (int)width, (int)height, color);
}

void rae_ext_drawRectangleLines(double x, double y, double width, double height, Color color) {
    DrawRectangleLines((int)x, (int)y, (int)width, (int)height, color);
}

void rae_ext_drawRectangleRounded(double x, double y, double width, double height, double roundness, int64_t segments, Color color) {
    Rectangle rec = {(float)x, (float)y, (float)width, (float)height};
    DrawRectangleRounded(rec, (float)roundness, (int)segments, color);
}

void rae_ext_drawCube(Vector3 pos, double width, double height, double length, Color color) {
    DrawCube(pos, (float)width, (float)height, (float)length, color);
}

void rae_ext_drawText(rae_String text, double x, double y, double fontSize, Color color) {
    DrawText((const char*)text.data, (int)x, (int)y, (int)fontSize, color);
}

rae_Bool rae_ext_windowShouldClose(void) { return WindowShouldClose(); }
void rae_ext_closeWindow(void) { CloseWindow(); }
void rae_ext_setTargetFPS(int64_t fps) { SetTargetFPS((int)fps); }

/* GLFW wait-events bindings. raylib bundles GLFW statically inside
 * libraylib.a; the dynamic libraylib.dylib does NOT export these
 * symbols. ALL build paths must link libraylib.a directly:
 *   - compiler/Makefile (the rae driver binary)
 *   - compiler/src/main.c (the gcc command rae emits when running
 *     a Compiled-target program)
 *   - compiler/tools/run_examples.sh
 *   - examples/98_mobile_ui/snapshot.sh
 *   - rae-devtools-web/src/server/config.ts (the IDE-style runner)
 *
 * If you're adding a new launcher: link `/opt/homebrew/lib/libraylib.a`
 * directly, NOT `-lraylib`. The dynamic library is missing the GLFW
 * symbols we need for the event-driven UI loop.
 *
 * No GLFW header is on the include path (Homebrew's raylib formula
 * does not ship glfw3.h); declare the prototypes locally. All three
 * should be called only after initWindow() has run — GLFW must be
 * initialised by then. */
extern void glfwWaitEventsTimeout(double timeout);
extern void glfwWaitEvents(void);
extern void glfwPostEmptyEvent(void);

/* Window-close callback: by default raylib's own callback sets
 * CORE.Window.shouldClose=TRUE when the user clicks the red X. We
 * override it with this waker so the wait-events main loop wakes up
 * immediately on close. The override needs to do BOTH jobs raylib's
 * default does PLUS post an empty event:
 *   1. glfwSetWindowShouldClose so raylib's WindowShouldClose() picks
 *      it up on the next call.
 *   2. glfwPostEmptyEvent so glfwWaitEventsTimeout returns now instead
 *      of waiting out the rest of its timeout. */
typedef struct GLFWwindow GLFWwindow;
typedef void (*GLFWwindowclosefun)(GLFWwindow*);
typedef void (*GLFWmousebuttonfun)(GLFWwindow*, int, int, int);
extern GLFWwindow* glfwGetCurrentContext(void);
extern void glfwSetWindowCloseCallback(GLFWwindow* w, GLFWwindowclosefun cb);
extern void glfwSetWindowShouldClose(GLFWwindow* w, int value);
extern GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow* w, GLFWmousebuttonfun cb);
#define RAE_GLFW_PRESS 1
#define RAE_GLFW_RELEASE 0
#define RAE_GLFW_MOUSE_BUTTON_LEFT 0

static void rae_glfw_close_waker(GLFWwindow* w) {
  /* macOS quirk: clicking the red-X invokes this delegate-driven
   * callback (we see it fire in stderr), but `glfwPostEmptyEvent`
   * from inside the delegate does NOT reliably wake the
   * `glfwWaitEventsTimeout` blocked in `nextEventMatchingMask` — and
   * the wait's own timeout doesn't fire either while the close path
   * is pending. So the window appeared "stuck open" until the user
   * clicked again somewhere in the app to generate an event the
   * wait would actually return from.
   *
   * Fix: exit the process directly from the callback. The OS
   * reclaims the GL context, GPU textures, and any other resources;
   * we lose the explicit `closeWindow` / texture-unload calls but
   * those are best-effort hygiene anyway. The diagnostic print is
   * kept so future investigations into a different macOS path don't
   * have to be re-instrumented. */
  fprintf(stderr, "[close-waker] fired (w=%p) — exiting\n", (void*)w);
  fflush(stderr);
  glfwSetWindowShouldClose(w, 1);
  // `_exit`, not `exit`: skip atexit handlers. raylib / GLFW
  // cleanup (CloseWindow, glfwTerminate) can hang waiting on the
  // GL context / GPU sync, and any user-registered atexit hook is
  // run on a thread that may already hold locks acquired in the
  // delegate-driven close path. The OS reclaims the GL context,
  // textures, fds, and process memory regardless. Without this,
  // the app process can stay alive after the window is gone,
  // which in turn keeps the rae watch supervisor waiting and
  // makes the devtools Stop button look broken.
  _exit(0);
}

/* macOS click-drop workaround. raylib's IsMouseButtonPressed/Released
 * are computed by comparing the previous poll's button state to the
 * current poll's — if a press+release pair arrives between two polls
 * (easy on macOS, where mouse-button events don't reliably wake
 * glfwWaitEventsTimeout), the post-poll state is up→up with no edge
 * and BOTH flags return false. The click vanishes.
 *
 * Fix: chain our own GLFW mouse-button callback below raylib's so we
 * see every transition per-event rather than per-poll. Counters are
 * single-threaded (GLFW callbacks fire on the main thread, same as
 * the rest of the loop), so plain int suffices — no atomics. */
static int g_mouse_press_pending = 0;
static int g_mouse_release_pending = 0;
static GLFWmousebuttonfun g_prev_mouse_button_cb = NULL;
static int g_mouse_button_hook_installed = 0;

static void rae_glfw_mouse_button_chain_cb(GLFWwindow* w, int button, int action, int mods) {
  if (g_prev_mouse_button_cb) g_prev_mouse_button_cb(w, button, action, mods);
  if (button == RAE_GLFW_MOUSE_BUTTON_LEFT) {
    if (action == RAE_GLFW_PRESS) g_mouse_press_pending = 1;
    else if (action == RAE_GLFW_RELEASE) g_mouse_release_pending = 1;
  }
}

void rae_ext_installMouseButtonHook(void) {
  if (g_mouse_button_hook_installed) return;
  GLFWwindow* w = glfwGetCurrentContext();
  if (!w) {
    fprintf(stderr, "[mouse-hook] FAILED: no current GLFW context\n");
    fflush(stderr);
    return;
  }
  g_prev_mouse_button_cb = glfwSetMouseButtonCallback(w, rae_glfw_mouse_button_chain_cb);
  g_mouse_button_hook_installed = 1;
  fprintf(stderr, "[mouse-hook] installed (prev cb=%p)\n", (void*)g_prev_mouse_button_cb);
  fflush(stderr);
}

rae_Bool rae_ext_mouseHookDrainPressed(void) {
  int v = g_mouse_press_pending;
  g_mouse_press_pending = 0;
  return v != 0;
}

rae_Bool rae_ext_mouseHookDrainReleased(void) {
  int v = g_mouse_release_pending;
  g_mouse_release_pending = 0;
  return v != 0;
}

/* True once installMouseButtonHook has successfully chained the GLFW
 * callback. Lets the input layer use the hook as the sole authoritative
 * edge source and skip raylib's poll-to-poll edges entirely, while
 * still falling back to polling for hosts that never install it. */
rae_Bool rae_ext_mouseHookActive(void) {
  return g_mouse_button_hook_installed != 0;
}

void rae_ext_waitEventsTimeout(double seconds) { glfwWaitEventsTimeout(seconds); }
void rae_ext_waitEvents(void) { glfwWaitEvents(); }
void rae_ext_postEmptyEvent(void) { glfwPostEmptyEvent(); }
void rae_ext_installWindowCloseWaker(void) {
  GLFWwindow* w = glfwGetCurrentContext();
  if (w) {
    glfwSetWindowCloseCallback(w, rae_glfw_close_waker);
    fprintf(stderr, "[close-waker] installed for window %p\n", (void*)w);
  } else {
    fprintf(stderr, "[close-waker] FAILED: no current GLFW context\n");
  }
  fflush(stderr);
}

void rae_ext_beginDrawing(void) { BeginDrawing(); }
void rae_ext_endDrawing(void) { EndDrawing(); }
void rae_ext_clearBackground(Color color) { ClearBackground(color); }
rae_Bool rae_ext_isKeyDown(int64_t key) { return IsKeyDown((int)key); }
rae_Bool rae_ext_isKeyPressed(int64_t key) { return IsKeyPressed((int)key); }
int64_t rae_ext_getMouseX(void) { return (int64_t)GetMouseX(); }
int64_t rae_ext_getMouseY(void) { return (int64_t)GetMouseY(); }
double rae_ext_getMouseWheelMove(void) { return (double)GetMouseWheelMove(); }
rae_Bool rae_ext_isMouseButtonDown(int64_t button) { return IsMouseButtonDown((int)button); }
rae_Bool rae_ext_isMouseButtonPressed(int64_t button) { return IsMouseButtonPressed((int)button); }
rae_Bool rae_ext_isMouseButtonReleased(int64_t button) { return IsMouseButtonReleased((int)button); }
void rae_ext_setMouseScale(double scaleX, double scaleY) { SetMouseScale((float)scaleX, (float)scaleY); }
int64_t rae_ext_getScreenWidth(void) { return (int64_t)GetScreenWidth(); }
int64_t rae_ext_getScreenHeight(void) { return (int64_t)GetScreenHeight(); }
rae_Bool rae_ext_isWindowResized(void) { return IsWindowResized(); }
int64_t rae_ext_getRenderWidth(void) { return (int64_t)GetRenderWidth(); }
int64_t rae_ext_getRenderHeight(void) { return (int64_t)GetRenderHeight(); }
int64_t rae_ext_getCurrentMonitor(void) { return (int64_t)GetCurrentMonitor(); }
int64_t rae_ext_getMonitorWidth(int64_t monitor) { return (int64_t)GetMonitorWidth((int)monitor); }
int64_t rae_ext_getMonitorHeight(int64_t monitor) { return (int64_t)GetMonitorHeight((int)monitor); }
int64_t rae_ext_getMonitorRefreshRate(int64_t monitor) { return (int64_t)GetMonitorRefreshRate((int)monitor); }
void rae_ext_setWindowSize(int64_t width, int64_t height) { SetWindowSize((int)width, (int)height); }
void rae_ext_setWindowPosition(int64_t x, int64_t y) { SetWindowPosition((int)x, (int)y); }
int64_t rae_ext_getWindowPositionX(void) { Vector2 p = GetWindowPosition(); return (int64_t)p.x; }
int64_t rae_ext_getWindowPositionY(void) { Vector2 p = GetWindowPosition(); return (int64_t)p.y; }
Texture rae_ext_loadTexture(rae_String fileName) { return LoadTexture((const char*)fileName.data); }

/* Set the GPU sampling filter for a texture id. `filter` matches
 * raylib's TextureFilter enum (0=POINT, 1=BILINEAR, 2=TRILINEAR, ...).
 * MSDF/MTSDF atlases REQUIRE bilinear (1): the distance-field decode
 * relies on the GPU interpolating distance values between texels;
 * point sampling produces jagged, stair-stepped glyph edges. The
 * filter is GL state keyed by texture id, so passing the Texture by
 * value still affects the live texture. */
void rae_ext_setTextureFilter(Texture texture, int64_t filter) {
  SetTextureFilter(texture, (int)filter);
}

Texture rae_ext_loadCircleCroppedTexture(rae_String fileName) {
  /* Load `fileName`, force RGBA, and zero the alpha channel of every
   * pixel outside an inscribed circle. Used by the mobile UI for
   * "profile picture"-style round avatars without needing a
   * fragment-shader pipeline. The smaller of width/height bounds
   * the circle so rectangular sources still produce a centered
   * circular crop. */
  Image img = LoadImage((const char*)fileName.data);
  if (img.data == NULL) {
    return (Texture){0};
  }
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  float cx = (float)img.width / 2.0f;
  float cy = (float)img.height / 2.0f;
  float r = (img.width < img.height ? (float)img.width : (float)img.height) / 2.0f;
  float r2 = r * r;
  unsigned char* data = (unsigned char*)img.data;
  for (int y = 0; y < img.height; y++) {
    for (int x = 0; x < img.width; x++) {
      float dx = (float)x + 0.5f - cx;
      float dy = (float)y + 0.5f - cy;
      float d2 = dx * dx + dy * dy;
      if (d2 > r2) {
        data[(y * img.width + x) * 4 + 3] = 0;
      }
    }
  }
  Texture tex = LoadTextureFromImage(img);
  UnloadImage(img);
  return tex;
}
Texture rae_ext_loadRoundedCroppedTexture(rae_String fileName, double radius) {
  /* Load `fileName`, force RGBA, downscale to a thumbnail-friendly
   * size, then zero the alpha channel of every pixel outside the
   * rounded-rectangle of the given corner radius. Pre-baking the
   * alpha mask lets the renderer use plain `DrawTexture` with no
   * clipping logic.
   *
   * Downscale matters: `radius` is intended as DISPLAY pixels (a
   * scene file saying `"radius": 8` means "8 px at display size").
   * Source covers are typically ~600x600 — baking only 8 pixels of
   * curve on that source gives ~0.7 display pixels when the thumb
   * renders at 56x56, indistinguishable from a sharp rectangle.
   * Resizing to a 128-side image first makes the same 8-pixel curve
   * map to ~3.5 display pixels, which reads as proper rounding. */
  Image img = LoadImage((const char*)fileName.data);
  if (img.data == NULL) {
    return (Texture){0};
  }
  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  /* Downscale longest side to `thumbMax` while preserving aspect.
   * 64 is small enough that even a small `radius` reads as proper
   * rounding when the thumb renders at typical UI sizes (40-80 px),
   * but big enough that the cover detail stays recognisable. */
  const int thumbMax = 64;
  int origMaxSide = (img.width > img.height) ? img.width : img.height;
  if (origMaxSide > thumbMax) {
    float k = (float)thumbMax / (float)origMaxSide;
    int newW = (int)((float)img.width * k);
    int newH = (int)((float)img.height * k);
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;
    ImageResize(&img, newW, newH);
  }
  float r = (float)radius;
  if (r < 0.0f) r = 0.0f;
  float maxR = (img.width < img.height ? (float)img.width : (float)img.height) / 2.0f;
  if (r > maxR) r = maxR;
  float r2 = r * r;
  float w1 = (float)img.width - 1.0f;
  float h1 = (float)img.height - 1.0f;
  unsigned char* data = (unsigned char*)img.data;
  for (int y = 0; y < img.height; y++) {
    for (int x = 0; x < img.width; x++) {
      /* Distance from this pixel to the nearest corner-center; if
       * inside the radius, alpha stays; if outside AND we're inside
       * the corner box, alpha is zeroed. */
      float fx = (float)x;
      float fy = (float)y;
      int inCorner = 0;
      float cx = 0.0f, cy = 0.0f;
      if (fx < r && fy < r) { inCorner = 1; cx = r; cy = r; }
      else if (fx > w1 - r && fy < r) { inCorner = 1; cx = w1 - r; cy = r; }
      else if (fx < r && fy > h1 - r) { inCorner = 1; cx = r; cy = h1 - r; }
      else if (fx > w1 - r && fy > h1 - r) { inCorner = 1; cx = w1 - r; cy = h1 - r; }
      if (inCorner) {
        float dx = fx - cx;
        float dy = fy - cy;
        if (dx * dx + dy * dy > r2) {
          data[(y * img.width + x) * 4 + 3] = 0;
        }
      }
    }
  }
  Texture tex = LoadTextureFromImage(img);
  UnloadImage(img);
  return tex;
}
void rae_ext_unloadTexture(Texture texture) { UnloadTexture(texture); }

/* Silence raylib's INFO/DEBUG logs (TEXTURE / SHADER / GL / ...).
 * Per-frame texture churn — e.g. the dock's frosted-glass blur
 * updating its cached texture every frame — otherwise floods the
 * terminal with "TEXTURE: [ID N] Texture loaded successfully" lines.
 * Pass `LOG_WARNING` (4) at boot to keep only warnings and errors. */
void rae_ext_raylibSetLogLevel(int64_t level) {
  SetTraceLogLevel((int)level);
}

/* Rounded textured rect via a fragment shader. One global Shader,
 * lazy-loaded on first use, used to mask any sprite with a non-zero
 * corner radius at draw time. Pass the on-screen sprite size + the
 * radius (both in pixels) before each draw; the shader computes a
 * signed-distance value to a rounded-rect boundary and antialiases
 * the alpha at the curve. */
static Shader g_rae_rounded_shader = {0};
static int g_rae_rounded_loc_size = -1;
static int g_rae_rounded_loc_radius = -1;
static int g_rae_rounded_loaded = 0;

static void rae_rounded_shader_ensure(void) {
  if (g_rae_rounded_loaded) return;
  const char* fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec2 uSize;\n"
    "uniform float uRadius;\n"
    "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec4 texel = texture(texture0, fragTexCoord);\n"
    "  vec4 c = texel * colDiffuse * fragColor;\n"
    "  vec2 p = (fragTexCoord - vec2(0.5)) * uSize;\n"
    "  float d = sdRoundRect(p, uSize * 0.5, uRadius);\n"
    "  float aa = clamp(0.5 - d, 0.0, 1.0);\n"
    "  c.a *= aa;\n"
    "  finalColor = c;\n"
    "}\n";
  g_rae_rounded_shader = LoadShaderFromMemory(NULL, fs);
  g_rae_rounded_loc_size = GetShaderLocation(g_rae_rounded_shader, "uSize");
  g_rae_rounded_loc_radius = GetShaderLocation(g_rae_rounded_shader, "uRadius");
  g_rae_rounded_loaded = 1;
}

void rae_ext_roundedSpriteBegin(double width, double height, double radius) {
  rae_rounded_shader_ensure();
  float size[2] = { (float)width, (float)height };
  float r = (float)radius;
  SetShaderValue(g_rae_rounded_shader, g_rae_rounded_loc_size, size, SHADER_UNIFORM_VEC2);
  SetShaderValue(g_rae_rounded_shader, g_rae_rounded_loc_radius, &r, SHADER_UNIFORM_FLOAT);
  BeginShaderMode(g_rae_rounded_shader);
}

void rae_ext_roundedSpriteEnd(void) {
  EndShaderMode();
}

/* ---- MTSDF text shader -----------------------------------------------
 *
 * Multi-channel + true-distance signed-distance-field text. The atlas
 * is generated offline by Chlumsky's `msdf-atlas-gen` (-type mtsdf).
 * RGB carries the MSDF median-trick channels (clean reconstruction of
 * sharp corners) and A carries the straight Euclidean distance — used
 * for the outline + shadow bands so they don't develop the false
 * intersections that pure RGB median has inside concave glyph features.
 *
 * `uPxRange` is the atlas's authored distanceRange (4 by default in
 * gen-msdf.sh) scaled to on-screen pixels — caller passes
 * `pxRange * onScreenSize / atlasFontSize`. It defines the anti-alias
 * band width: 1 means hard pixels, larger = softer edges.
 *
 * Outline: bands the silhouette by an additional `uOutlineWidth` screen
 * pixels around the body, painted in `uOutlineColor`.
 *
 * Shadow: re-samples the atlas at fragTexCoord - uShadowOffset (so a
 * positive uShadowOffset is "shadow falls down-right"). `uShadowSoftness`
 * widens the falloff in pixels — 1 = hard, 4-8 = nicely blurred. Shadow
 * paints UNDER the outline + body.
 *
 * The shader composites in this order: shadow → outline → body. Each
 * layer multiplies its coverage against the per-vertex `fragColor` (so
 * the entity's Active/Opacity chain still fades the whole glyph). */
static Shader g_rae_mtsdf_shader = {0};
static int g_rae_mtsdf_loaded = 0;
static int g_rae_mtsdf_loc_pxrange = -1;
static int g_rae_mtsdf_loc_textColor = -1;
static int g_rae_mtsdf_loc_outlineColor = -1;
static int g_rae_mtsdf_loc_outlineWidth = -1;
static int g_rae_mtsdf_loc_shadowColor = -1;
static int g_rae_mtsdf_loc_shadowOffset = -1;
static int g_rae_mtsdf_loc_shadowSoftness = -1;

static void rae_mtsdf_shader_ensure(void) {
  if (g_rae_mtsdf_loaded) return;
  const char* fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform float uPxRange;\n"
    "uniform vec4  uTextColor;\n"
    "uniform vec4  uOutlineColor;\n"
    "uniform float uOutlineWidth;\n"
    "uniform vec4  uShadowColor;\n"
    "uniform vec2  uShadowOffset;\n"
    "uniform float uShadowSoftness;\n"
    "float median3(float r, float g, float b) {\n"
    "  return max(min(r,g), min(max(r,g), b));\n"
    "}\n"
    "float msdfDistPx(vec4 sample4) {\n"
    "  return uPxRange * (median3(sample4.r, sample4.g, sample4.b) - 0.5);\n"
    "}\n"
    /* Composite one straight-alpha layer (color `srgb`, coverage-scaled
     * alpha `sa`) over a PREMULTIPLIED accumulator. Working in
     * premultiplied space is what kills the edge halo: the previous
     * shader multiplied glyph RGB by coverage and then the alpha-blend
     * stage multiplied by coverage again (coverage^2), darkening
     * anti-aliased edges — invisible on dark UI, an obvious grey
     * "outline" on light backgrounds. */
    "vec4 overPremul(vec4 dst, vec3 srgb, float sa) {\n"
    "  vec3 sp = srgb * sa;\n"
    "  return vec4(sp + dst.rgb * (1.0 - sa), sa + dst.a * (1.0 - sa));\n"
    "}\n"
    "void main() {\n"
    "  vec4 atlasSample = texture(texture0, fragTexCoord);\n"
    "  float bodyDist = msdfDistPx(atlasSample);\n"
    "  float bodyCov  = clamp(bodyDist + 0.5, 0.0, 1.0);\n"
    "  float outlineCov = 0.0;\n"
    "  if (uOutlineWidth > 0.0 && uOutlineColor.a > 0.0) {\n"
    "    float outlineTotal = clamp((bodyDist + uOutlineWidth) + 0.5, 0.0, 1.0);\n"
    "    outlineCov = clamp(outlineTotal - bodyCov, 0.0, 1.0);\n"
    "  }\n"
    "  float shadowCov = 0.0;\n"
    "  if (uShadowColor.a > 0.0) {\n"
    "    vec4 sSample = texture(texture0, fragTexCoord - uShadowOffset);\n"
    "    float sDist = msdfDistPx(sSample);\n"
    "    float soft = max(uShadowSoftness, 1.0);\n"
    "    shadowCov = clamp(sDist / soft + 0.5, 0.0, 1.0);\n"
    "  }\n"
    /* Bottom-to-top: shadow, then outline ring, then body. */
    "  vec4 acc = vec4(0.0);\n"
    "  acc = overPremul(acc, uShadowColor.rgb,  uShadowColor.a  * shadowCov);\n"
    "  acc = overPremul(acc, uOutlineColor.rgb, uOutlineColor.a * outlineCov);\n"
    "  acc = overPremul(acc, uTextColor.rgb,    uTextColor.a    * bodyCov);\n"
    /* Entity tint/fade (fragColor * colDiffuse) scales the whole
     * premultiplied result so alpha and colour stay consistent. */
    "  vec4 tint = fragColor * colDiffuse;\n"
    "  acc.rgb *= tint.rgb;\n"
    "  acc *= tint.a;\n"
    "  finalColor = acc;\n"
    "}\n";
  g_rae_mtsdf_shader = LoadShaderFromMemory(NULL, fs);
  g_rae_mtsdf_loc_pxrange        = GetShaderLocation(g_rae_mtsdf_shader, "uPxRange");
  g_rae_mtsdf_loc_textColor      = GetShaderLocation(g_rae_mtsdf_shader, "uTextColor");
  g_rae_mtsdf_loc_outlineColor   = GetShaderLocation(g_rae_mtsdf_shader, "uOutlineColor");
  g_rae_mtsdf_loc_outlineWidth   = GetShaderLocation(g_rae_mtsdf_shader, "uOutlineWidth");
  g_rae_mtsdf_loc_shadowColor    = GetShaderLocation(g_rae_mtsdf_shader, "uShadowColor");
  g_rae_mtsdf_loc_shadowOffset   = GetShaderLocation(g_rae_mtsdf_shader, "uShadowOffset");
  g_rae_mtsdf_loc_shadowSoftness = GetShaderLocation(g_rae_mtsdf_shader, "uShadowSoftness");
  g_rae_mtsdf_loaded = 1;
}

/* Caller passes: pxRange (in screen px — i.e. atlas pxrange * screen-
 * size / atlas-font-size), the text/outline/shadow colors as 0-255
 * RGBA, the outline width in screen px, shadow offset in texCoord
 * units (atlas-uv space, NOT screen-px — Rae side knows the atlas
 * dimensions and converts), and shadow softness in screen px. */
void rae_ext_msdfBegin(double pxRange,
                       Color textColor, Color outlineColor, double outlineWidth,
                       Color shadowColor, double shadowOffX, double shadowOffY,
                       double shadowSoftness) {
  rae_mtsdf_shader_ensure();
  float px = (float)pxRange;
  float tc[4] = { textColor.r/255.0f, textColor.g/255.0f, textColor.b/255.0f, textColor.a/255.0f };
  float oc[4] = { outlineColor.r/255.0f, outlineColor.g/255.0f, outlineColor.b/255.0f, outlineColor.a/255.0f };
  float ow = (float)outlineWidth;
  float sc[4] = { shadowColor.r/255.0f, shadowColor.g/255.0f, shadowColor.b/255.0f, shadowColor.a/255.0f };
  float so[2] = { (float)shadowOffX, (float)shadowOffY };
  float ss = (float)shadowSoftness;
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_pxrange,        &px, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_textColor,      tc,  SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_outlineColor,   oc,  SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_outlineWidth,   &ow, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_shadowColor,    sc,  SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_shadowOffset,   so,  SHADER_UNIFORM_VEC2);
  SetShaderValue(g_rae_mtsdf_shader, g_rae_mtsdf_loc_shadowSoftness, &ss, SHADER_UNIFORM_FLOAT);
  /* The fragment shader emits PREMULTIPLIED-alpha colour, so it must be
   * composited with a premultiplied blend (GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
   * Using the default straight-alpha blend here would re-introduce the
   * coverage^2 edge darkening this shader exists to avoid. */
  BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
  BeginShaderMode(g_rae_mtsdf_shader);
}

void rae_ext_msdfEnd(void) {
  EndShaderMode();
  EndBlendMode();
}

void rae_ext_drawTexturePro(Texture texture, Rectangle source, Rectangle dest, Vector2 origin, double rotation, Color tint) {
  DrawTexturePro(texture, source, dest, origin, (float)rotation, tint);
}

/* Gradient rounded-rect: shader fills the alpha-masked rounded box
 * with a linear gradient from `from` to `to` along the given angle
 * (degrees — 0=L→R, 90=T→B). Pair with `CornerRadius` in scenes
 * via the `GradientFill` ECS component. Single atomic call;
 * lazy-loads its shader on first use. */
static Shader g_rae_gradient_shader = {0};
static int g_rae_grad_loc_size = -1;
static int g_rae_grad_loc_radius = -1;
static int g_rae_grad_loc_from = -1;
static int g_rae_grad_loc_to = -1;
static int g_rae_grad_loc_angle = -1;
static int g_rae_gradient_loaded = 0;

static void rae_gradient_shader_ensure(void) {
  if (g_rae_gradient_loaded) return;
  const char* fs =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform vec2 uSize;\n"
    "uniform float uRadius;\n"
    "uniform vec4 uFrom;\n"
    "uniform vec4 uTo;\n"
    "uniform float uAngleRad;\n"
    "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
    "  vec2 q = abs(p) - b + vec2(r);\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;\n"
    "}\n"
    "void main() {\n"
    "  vec2 dir = vec2(cos(uAngleRad), sin(uAngleRad));\n"
    "  vec2 p = fragTexCoord - vec2(0.5);\n"
    "  float t = clamp(dot(p, dir) + 0.5, 0.0, 1.0);\n"
    "  vec4 c = mix(uFrom, uTo, t);\n"
    "  vec2 pp = (fragTexCoord - vec2(0.5)) * uSize;\n"
    "  float d = sdRoundRect(pp, uSize * 0.5, uRadius);\n"
    "  float aa = clamp(0.5 - d, 0.0, 1.0);\n"
    "  c.a *= aa * fragColor.a;\n"
    "  finalColor = c;\n"
    "}\n";
  g_rae_gradient_shader = LoadShaderFromMemory(NULL, fs);
  g_rae_grad_loc_size = GetShaderLocation(g_rae_gradient_shader, "uSize");
  g_rae_grad_loc_radius = GetShaderLocation(g_rae_gradient_shader, "uRadius");
  g_rae_grad_loc_from = GetShaderLocation(g_rae_gradient_shader, "uFrom");
  g_rae_grad_loc_to = GetShaderLocation(g_rae_gradient_shader, "uTo");
  g_rae_grad_loc_angle = GetShaderLocation(g_rae_gradient_shader, "uAngleRad");
  g_rae_gradient_loaded = 1;
}

void rae_ext_drawGradientRect(
    double x, double y, double w, double h, double radius,
    int64_t r1, int64_t g1, int64_t b1, int64_t a1,
    int64_t r2, int64_t g2, int64_t b2, int64_t a2,
    double angleDeg
) {
  rae_gradient_shader_ensure();
  float size[2] = { (float)w, (float)h };
  float fr = (float)radius;
  float from[4] = { (float)r1 / 255.0f, (float)g1 / 255.0f, (float)b1 / 255.0f, (float)a1 / 255.0f };
  float to[4]   = { (float)r2 / 255.0f, (float)g2 / 255.0f, (float)b2 / 255.0f, (float)a2 / 255.0f };
  float angleRad = (float)(angleDeg * 3.14159265358979 / 180.0);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_size, size, SHADER_UNIFORM_VEC2);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_radius, &fr, SHADER_UNIFORM_FLOAT);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_from, from, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_to, to, SHADER_UNIFORM_VEC4);
  SetShaderValue(g_rae_gradient_shader, g_rae_grad_loc_angle, &angleRad, SHADER_UNIFORM_FLOAT);
  BeginShaderMode(g_rae_gradient_shader);
  /* Emit a textured quad with explicit 0..1 UVs. `DrawRectangleRec`
   * samples raylib's tiny `shapes` white-pixel texture so fragTexCoord
   * is effectively constant — the shader's gradient + SDF corner mask
   * both need fragTexCoord to span 0..1 across the rect. Hand-rolling
   * the quad guarantees it. */
  Texture2D white = (Texture2D){ rlGetTextureIdDefault(), 1, 1, 1, 7 };
  rlSetTexture(white.id);
  rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, 255);
    rlNormal3f(0.0f, 0.0f, 1.0f);
    rlTexCoord2f(0.0f, 0.0f); rlVertex2f((float)x,            (float)y);
    rlTexCoord2f(0.0f, 1.0f); rlVertex2f((float)x,            (float)(y + h));
    rlTexCoord2f(1.0f, 1.0f); rlVertex2f((float)(x + w),      (float)(y + h));
    rlTexCoord2f(1.0f, 0.0f); rlVertex2f((float)(x + w),      (float)y);
  rlEnd();
  rlSetTexture(0);
  EndShaderMode();
}
Texture rae_ext_captureAndBlurRegion(double x, double y, double w, double h, int64_t blurSize) {
  /* Frosted-glass for a sub-region of the screen — used by the
   * mobile UI's bottom dock so only the bar area is blurred (the
   * "vibrancy" effect), not the whole window like the modal-blur
   * helper below. `x/y/w/h` are in *logical* coordinates; we scale
   * by GetWindowScaleDPI() to crop the right physical-pixel rect
   * from `LoadImageFromScreen`. */
  rlDrawRenderBatchActive();
  glFinish();
  Image full = LoadImageFromScreen();
  Vector2 scale = GetWindowScaleDPI();
  Rectangle crop = {
    (float)x * scale.x,
    (float)y * scale.y,
    (float)w * scale.x,
    (float)h * scale.y
  };
  Image region = ImageFromImage(full, crop);
  UnloadImage(full);
  ImageBlurGaussian(&region, (int)blurSize);
  Texture tex = LoadTextureFromImage(region);
  UnloadImage(region);
  return tex;
}

Texture rae_ext_captureAndBlurBackdrop(int64_t blurSize) {
  /* Frosted-glass backdrop helper for modal UI: grab the back buffer,
   * run a Gaussian blur over it on the CPU, upload as a Texture, and
   * release the temporary Image. The blur radius `blurSize` is in
   * pixels — ~10 reads as soft "vibrancy" on a typical mobile-sized
   * window. Cost is dominated by ImageBlurGaussian which is O(width *
   * height * blurSize). Designed for one-shot calls on modal open.
   *
   * IMPORTANT: raylib batches draw calls in CPU buffers and only
   * flushes them to the GPU at end-of-frame (or when the batch fills
   * up). Calling `LoadImageFromScreen` while draws are still pending
   * returns stale framebuffer contents — usually just the most
   * recent ClearBackground color, which is why the captured image
   * looks like a uniform dark block. Fix: force a flush + GPU sync
   * before the read. Same workaround `examples/98_mobile_ui/snapshot.sh`
   * documents for the related `TakeScreenshot` path on macOS+Metal. */
  rlDrawRenderBatchActive();
  glFinish();
  Image img = LoadImageFromScreen();
  ImageBlurGaussian(&img, (int)blurSize);
  Texture tex = LoadTextureFromImage(img);
  UnloadImage(img);
  return tex;
}
void rae_ext_drawTexture(Texture texture, double x, double y, Color tint) { DrawTexture(texture, (int)x, (int)y, tint); }
void rae_ext_drawTextureEx(Texture texture, Vector2 pos, double rotation, double scale, Color tint) { DrawTextureEx(texture, pos, (float)rotation, (float)scale, tint); }
int64_t rae_ext_measureText(rae_String text, int64_t fontSize) { return (int64_t)MeasureText((const char*)text.data, (int)fontSize); }

/* Custom font support.
 *
 * Raylib's `Font` struct holds internal arrays/pointers, which makes passing
 * it across the Rae ↔ C boundary awkward (especially for the live VM, where
 * structs go through RaeAny). We sidestep that with a fixed-size array of
 * "font slots" addressed by `Int`. A slot starts unloaded; `loadFontInto`
 * fills it via raylib's `LoadFontEx` with a codepoint table that covers
 * basic ASCII plus the few Unicode glyphs the HUD uses (arrows, middle dot,
 * em dash). `drawTextWithFont` falls back to the default font if the slot
 * isn't loaded yet, so the program never silently shows blank text.
 */
#define RAE_FONT_SLOTS 8
static Font g_rae_fonts[RAE_FONT_SLOTS];
static int g_rae_font_loaded[RAE_FONT_SLOTS];

static const int g_rae_font_codepoints[] = {
    /* ASCII printable */
    32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
    64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
    80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
    96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
    /* Latin-1 Supplement (U+00A0..U+00FF). Covers ä ö ü ß é è ê à á â
     * ç ñ í ó ú etc. — everything in the names "Björk", "Röyksopp",
     * and the common Western European diacritics. Body fonts (Roboto)
     * have these glyphs; icon fonts don't, and bake notdef boxes that
     * never get drawn because callers don't write 0xA0-0xFF to the
     * icon slot. */
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6,         /* 0x00B7 below */
            0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
    /* HUD glyphs */
    0x00B7, /* · middle dot */
    0x2013, /* – en dash */
    0x2014, /* — em dash */
    0x2192, /* → right arrow */
    0x2190, /* ← left arrow */
    0x2191, /* ↑ up arrow */
    0x2193, /* ↓ down arrow */
    0x2026, /* … horizontal ellipsis */
    /* Material Icons Outlined codepoints used by `lib/ui/icon_codepoints.rae`.
     * Body fonts (e.g. Roboto) won't have glyphs for these — they bake as
     * `notdef` boxes, but no caller writes these codepoints to a body font
     * slot. The icon-font slot picks them up. */
    0xe145, /* add */
    0xe5c4, /* arrow_back */
    0xe5cb, /* chevron_left */
    0xe5cc, /* chevron_right */
    0xe5cd, /* close */
    0xf090, /* download */
    0xe01d, /* equalizer */
    0xe5ce, /* expand_less */
    0xe5cf, /* expand_more */
    0xe87d, /* favorite */
    0xe87e, /* favorite_border */
    0xe88a, /* home */
    0xe02e, /* library_add */
    0xe02f, /* library_books */
    0xe030, /* library_music */
    0xe5d2, /* menu */
    0xeae1, /* more_horiz */
    0xe034, /* pause */
    0xe037, /* play_arrow */
    0xe03b, /* playlist_add */
    0xe065, /* playlist_add_check */
    0xe05f, /* playlist_play */
    0xe03d, /* queue_music */
    0xe040, /* repeat */
    0xe8b6, /* search */
    0xe043, /* shuffle */
    0xe044, /* skip_next */
    0xe045, /* skip_previous */
    0xe047, /* stop */
    0xe80e, /* whatshot */
    0xe7fd, /* person */
    0xe429, /* tune */
    0xe9ba  /* logout */
};
#define RAE_FONT_CODEPOINT_COUNT ((int)(sizeof(g_rae_font_codepoints)/sizeof(g_rae_font_codepoints[0])))

void rae_ext_loadFontInto(int64_t slot, rae_String path, int64_t fontSize) {
    if (slot < 0 || slot >= RAE_FONT_SLOTS) return;
    if (g_rae_font_loaded[slot]) {
        UnloadFont(g_rae_fonts[slot]);
        g_rae_font_loaded[slot] = 0;
    }
    /* Quality strategy: bake the glyph atlas at a high resolution and let
     * the GPU bilinear-filter at draw time. Raylib's default is point
     * sampling, which is fine for retro pixel fonts but turns smooth
     * vector fonts (Roboto etc.) into a blocky mess at non-native sizes.
     * Use max(64, fontSize * 2) so a typical 18–28 px UI request gets a
     * 64–96 px atlas — sharp at the requested size, smooth when scaled. */
    int atlasSize = (int)fontSize * 2;
    if (atlasSize < 64) atlasSize = 64;
    g_rae_fonts[slot] = LoadFontEx(
        (const char*)path.data,
        atlasSize,
        (int*)g_rae_font_codepoints,
        RAE_FONT_CODEPOINT_COUNT
    );
    if (g_rae_fonts[slot].texture.id != 0) {
        SetTextureFilter(g_rae_fonts[slot].texture, TEXTURE_FILTER_BILINEAR);
        g_rae_font_loaded[slot] = 1;
    }
}

void rae_ext_unloadFontSlot(int64_t slot) {
    if (slot < 0 || slot >= RAE_FONT_SLOTS) return;
    if (g_rae_font_loaded[slot]) {
        UnloadFont(g_rae_fonts[slot]);
        g_rae_font_loaded[slot] = 0;
    }
}

rae_Bool rae_ext_isFontSlotLoaded(int64_t slot) {
    if (slot < 0 || slot >= RAE_FONT_SLOTS) return 0;
    return g_rae_font_loaded[slot] ? 1 : 0;
}

void rae_ext_drawTextWithFont(int64_t slot, rae_String text, double x, double y, double fontSize, double spacing, Color color) {
    if (slot >= 0 && slot < RAE_FONT_SLOTS && g_rae_font_loaded[slot]) {
        DrawTextEx(
            g_rae_fonts[slot],
            (const char*)text.data,
            (Vector2){(float)x, (float)y},
            (float)fontSize,
            (float)spacing,
            color
        );
    } else {
        /* Fallback: default font — keeps text on screen if the TTF is missing. */
        DrawText((const char*)text.data, (int)x, (int)y, (int)fontSize, color);
    }
}

/* Slot-aware width measurement. Companion to `drawTextWithFont` — needed
 * to center icon glyphs correctly, since the Material Icons font has
 * private-use codepoints (e.g. 0xE037 = play_arrow) that aren't in the
 * default font. Plain `MeasureText` would return the default-font's
 * notdef width instead of the actual rendered glyph width, putting the
 * icon visibly off-center inside its container.
 * Returns the rendered width in pixels at `fontSize` with `spacing`
 * between glyphs. Falls back to the default font's measurement when
 * the slot isn't loaded, matching `drawTextWithFont`'s fallback. */
int64_t rae_ext_measureTextWithFont(int64_t slot, rae_String text, double fontSize, double spacing) {
    if (slot >= 0 && slot < RAE_FONT_SLOTS && g_rae_font_loaded[slot]) {
        Vector2 sz = MeasureTextEx(
            g_rae_fonts[slot],
            (const char*)text.data,
            (float)fontSize,
            (float)spacing
        );
        return (int64_t)sz.x;
    }
    return (int64_t)MeasureText((const char*)text.data, (int)fontSize);
}
#endif

/* ============================================================
 * Spotify (macOS desktop app) bridge — see lib/sys/spotify.rae
 *
 * Drives the local Spotify desktop app via `osascript` (AppleScript).
 * No credentials, no SDK, no HTTP auth — pure local automation. The
 * runtime needs the Spotify app open and the one-time macOS
 * "Automation" permission ("allow rae to control Spotify"); first
 * run triggers the system prompt.
 *
 * Layout: one `spotifyRefresh()` call per poll runs osascript and
 * fills a 6-field static cache (state + track + artist + album +
 * track id + artwork url). Per-field getters then return fresh owned
 * String copies so the rae side can build a `SpotifyTrack` struct
 * without struct-FFI gymnastics.
 *
 * Album art: `spotifyFetchArtwork(url, outPath)` shells out to curl.
 * iTunes Search API fallback for when Spotify hands back an empty
 * artwork URL (local files, podcasts) — `itunesSearchArtworkUrl`
 * upscales the 100x100 thumb to 600x600 the same way SUMU does.
 * ============================================================ */

#if defined(__APPLE__)
#include <sys/wait.h>

static int rae_osascript_run(const char* const* lines) {
    int argc = 0;
    while (lines[argc]) argc++;
    char** argv = malloc(sizeof(char*) * (2 + 2 * (size_t)argc + 1));
    if (!argv) return -1;
    int a = 0;
    argv[a++] = (char*)"osascript";
    for (int i = 0; i < argc; i++) {
        argv[a++] = (char*)"-e";
        argv[a++] = (char*)lines[i];
    }
    argv[a] = NULL;
    pid_t pid = fork();
    if (pid < 0) { free(argv); return -1; }
    if (pid == 0) {
        execvp("/usr/bin/osascript", argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    free(argv);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static long rae_osascript_capture(const char* const* lines, char* out, size_t out_cap) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    int argc = 0;
    while (lines[argc]) argc++;
    char** argv = malloc(sizeof(char*) * (2 + 2 * (size_t)argc + 1));
    if (!argv) { close(fds[0]); close(fds[1]); return -1; }
    int a = 0;
    argv[a++] = (char*)"osascript";
    for (int i = 0; i < argc; i++) {
        argv[a++] = (char*)"-e";
        argv[a++] = (char*)lines[i];
    }
    argv[a] = NULL;
    pid_t pid = fork();
    if (pid < 0) { free(argv); close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        execvp("/usr/bin/osascript", argv);
        _exit(127);
    }
    close(fds[1]);
    size_t off = 0;
    while (off + 1 < out_cap) {
        ssize_t n = read(fds[0], out + off, out_cap - 1 - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    out[off] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    free(argv);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    while (off > 0 && (out[off - 1] == '\n' || out[off - 1] == '\r')) {
        out[--off] = '\0';
    }
    return (long)off;
}

typedef struct {
    char* state;
    char* trackId;
    char* trackName;
    char* artistName;
    char* albumName;
    char* artworkUrl;
    double positionSec;     /* player position in seconds, 0 when stopped */
    double durationSec;     /* track duration in seconds, 0 when unknown */
} RaeSpotifyCache;
static RaeSpotifyCache g_spotify_cache = {0};

static void rae_spotify_cache_set_field(char** slot, const char* src, size_t len) {
    free(*slot);
    *slot = malloc(len + 1);
    if (!*slot) return;
    memcpy(*slot, src, len);
    (*slot)[len] = '\0';
}

static rae_String rae_cstr_to_owned_rae_string(const char* s) {
    if (!s) return (rae_String){NULL, 0, 0, 0};
    size_t n = strlen(s);
    uint8_t* buf = malloc(n + 1);
    if (!buf) return (rae_String){NULL, 0, 0, 0};
    memcpy(buf, s, n);
    buf[n] = '\0';
    rae_mem_str_tag(buf, (int64_t)n + 1, RAE_SITE_FROM_CSTR);
    return (rae_String){buf, (int64_t)n, (int64_t)n + 1, 1};
}

void rae_ext_spotifyLaunch(void) {
    fprintf(stderr, "[spotify-c] launch\n");
    static const char* lines[] = {
        "tell application \"Spotify\" to if it is not running then launch",
        NULL
    };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] launch failed (osascript rc=%d)\n", rc);
}

void rae_ext_spotifyPlay(void) {
    fprintf(stderr, "[spotify-c] play\n");
    static const char* lines[] = { "tell application \"Spotify\" to play", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play failed (osascript rc=%d) — Spotify not running or Automation permission denied?\n", rc);
}

void rae_ext_spotifyPause(void) {
    fprintf(stderr, "[spotify-c] pause\n");
    static const char* lines[] = { "tell application \"Spotify\" to pause", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] pause failed (osascript rc=%d)\n", rc);
}

void rae_ext_spotifyNext(void) {
    fprintf(stderr, "[spotify-c] next\n");
    static const char* lines[] = { "tell application \"Spotify\" to next track", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] next failed (osascript rc=%d)\n", rc);
}

void rae_ext_spotifyPrevious(void) {
    fprintf(stderr, "[spotify-c] previous\n");
    static const char* lines[] = { "tell application \"Spotify\" to previous track", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] previous failed (osascript rc=%d)\n", rc);
}

/* Play a specific Spotify URI directly. Accepts spotify:track:<id>,
 * spotify:album:<id>, spotify:playlist:<id>, etc. Used when the local
 * album.json carries an explicit Spotify URI. */
void rae_ext_spotifyPlayUri(rae_String uri) {
    if (!uri.data || uri.len == 0) return;
    fprintf(stderr, "[spotify-c] play uri=%.*s\n", (int)uri.len, (const char*)uri.data);
    char* uri_c = malloc((size_t)uri.len + 1);
    if (!uri_c) return;
    memcpy(uri_c, uri.data, (size_t)uri.len); uri_c[uri.len] = '\0';
    char script[1024];
    snprintf(script, sizeof(script), "tell application \"Spotify\" to play track \"%s\"", uri_c);
    free(uri_c);
    const char* lines[] = { script, NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play uri failed (osascript rc=%d)\n", rc);
}

/* Search-and-play: feed the query string to Spotify's search URI scheme,
 * wait briefly for the search panel to populate, then issue `play`. This
 * is the AppleScript equivalent of the user typing into the search box
 * and pressing the play button — Spotify auto-plays the top hit when
 * the search loads with focus on it.
 *
 * The query is URL-encoded inline (alphanumerics + a few safe chars
 * pass through; everything else %xx). Quotes are escaped for the
 * AppleScript string literal. */
void rae_ext_spotifyPlayQuery(rae_String query) {
    if (!query.data || query.len == 0) return;
    fprintf(stderr, "[spotify-c] play query=%.*s\n", (int)query.len, (const char*)query.data);
    /* URL-encode the query for the `spotify:search:` URI. A bare space
     * (or any reserved/unsafe char) makes Spotify's URI handler stop at
     * the first space: "Time to pretend MGMT" was being parsed as just
     * "Time". Percent-encode everything outside the RFC 3986 unreserved
     * set (A-Z a-z 0-9 - _ . ~) so the full multi-word query reaches
     * Spotify. The encoded output contains no `"` or `\`, so it is also
     * safe to drop straight into the AppleScript string literal below. */
    static const char rae_hexdig[] = "0123456789ABCDEF";
    char escaped[2048];
    size_t off = 0;
    for (size_t i = 0; i < (size_t)query.len && off + 4 < sizeof(escaped); i++) {
        unsigned char c = (unsigned char)query.data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped[off++] = (char)c;
        } else {
            escaped[off++] = '%';
            escaped[off++] = rae_hexdig[c >> 4];
            escaped[off++] = rae_hexdig[c & 0x0F];
        }
    }
    escaped[off] = '\0';
    /* One atomic AppleScript: launch Spotify if needed, then play the
     * search URI in a single `play track` call. `tell ... to play` /
     * `launch` (never `activate`) drive Spotify via background Apple
     * Events, like SUMU's play_spotify.
     *
     * Focus handling lives in C (`rae_ext_activateSelf` after the run),
     * NOT in this AppleScript. We deliberately do NOT capture-and-
     * restore "whatever was frontmost before the play" via System Events
     * `set frontmost`: that hands focus to the launching app (SUMU when
     * run from the dev tools), dropping our window out of the foreground
     * — macOS then demotes us to a background process (no Dock entry,
     * unfocusable, dead close button, launches behind SUMU). Re-asserting
     * our OWN app instead has no such failure mode. */
    char launch_line[128];
    char play_line[2160];
    snprintf(launch_line, sizeof(launch_line),
        "tell application \"Spotify\" to if it is not running then launch");
    snprintf(play_line, sizeof(play_line),
        "tell application \"Spotify\" to play track \"spotify:search:%s\"",
        escaped);
    const char* lines[] = {
        launch_line,
        play_line,
        NULL
    };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play query failed (osascript rc=%d)\n", rc);
    /* `tell ... to play` is a background Apple Event, but `launch` (and
     * some Spotify configs) can pull Spotify to the front. Re-assert our
     * own window so the user keeps clicking the rae UI instead of having
     * focus stuck on Spotify. Activating SELF (not "the previous app")
     * avoids handing focus to the launcher (SUMU) — see rae_ext_activateSelf. */
    rae_ext_activateSelf();
}

void rae_ext_spotifyRefresh(void) {
    static const char* lines[] = {
        "tell application \"Spotify\"",
        "  set playerState to player state as text",
        "  if playerState is \"stopped\" then",
        "    return playerState & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"\" & \"||\" & \"0\" & \"||\" & \"0\"",
        "  end if",
        "  set trackId to \"\"",
        "  set trackName to \"\"",
        "  set artistName to \"\"",
        "  set albumName to \"\"",
        "  set artworkUrl to \"\"",
        "  set posSec to 0",
        "  set durMs to 0",
        "  try",
        "    set trackId to id of current track",
        "  end try",
        "  try",
        "    set trackName to name of current track",
        "  end try",
        "  try",
        "    set artistName to artist of current track",
        "  end try",
        "  try",
        "    set albumName to album of current track",
        "  end try",
        "  try",
        "    set artworkUrl to artwork url of current track",
        "  end try",
        "  try",
        "    set posSec to player position",
        "  end try",
        "  try",
        "    set durMs to duration of current track",
        "  end try",
        "  return playerState & \"||\" & trackId & \"||\" & trackName & \"||\" & artistName & \"||\" & albumName & \"||\" & artworkUrl & \"||\" & (posSec as text) & \"||\" & (durMs as text)",
        "end tell",
        NULL
    };
    char buf[4096];
    long n = rae_osascript_capture(lines, buf, sizeof(buf));
    char** slots[] = {
        &g_spotify_cache.state,
        &g_spotify_cache.trackId,
        &g_spotify_cache.trackName,
        &g_spotify_cache.artistName,
        &g_spotify_cache.albumName,
        &g_spotify_cache.artworkUrl,
    };
    /* Reset everything before re-filling so a failure leaves a known-empty cache. */
    for (int i = 0; i < 6; i++) {
        rae_spotify_cache_set_field(slots[i], "", 0);
    }
    g_spotify_cache.positionSec = 0.0;
    g_spotify_cache.durationSec = 0.0;
    if (n < 0) return;
    char* p = buf;
    /* Parse 6 string fields. */
    for (int i = 0; i < 6; i++) {
        char* sep = strstr(p, "||");
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        rae_spotify_cache_set_field(slots[i], p, len);
        if (!sep) break;
        p = sep + 2;
    }
    /* Parse the trailing position + duration (numeric). The strstr walk
     * above leaves p pointing past the last "||" of the strings if every
     * separator was found. Re-walk from the buffer start to be safe. */
    {
        char* q = buf;
        for (int i = 0; i < 6; i++) {
            char* sep = strstr(q, "||");
            if (!sep) { q = NULL; break; }
            q = sep + 2;
        }
        if (q) {
            char* sep = strstr(q, "||");
            if (sep) {
                *sep = '\0';
                g_spotify_cache.positionSec = atof(q);
                char* d = sep + 2;
                g_spotify_cache.durationSec = atof(d) / 1000.0;
            }
        }
    }
}

rae_String rae_ext_spotifyState(void)       { return rae_cstr_to_owned_rae_string(g_spotify_cache.state); }
rae_String rae_ext_spotifyTrackId(void)     { return rae_cstr_to_owned_rae_string(g_spotify_cache.trackId); }
rae_String rae_ext_spotifyTrackName(void)   { return rae_cstr_to_owned_rae_string(g_spotify_cache.trackName); }
rae_String rae_ext_spotifyArtistName(void)  { return rae_cstr_to_owned_rae_string(g_spotify_cache.artistName); }
rae_String rae_ext_spotifyAlbumName(void)   { return rae_cstr_to_owned_rae_string(g_spotify_cache.albumName); }
rae_String rae_ext_spotifyArtworkUrl(void)  { return rae_cstr_to_owned_rae_string(g_spotify_cache.artworkUrl); }
double rae_ext_spotifyPosition(void)        { return g_spotify_cache.positionSec; }
double rae_ext_spotifyDuration(void)        { return g_spotify_cache.durationSec; }

rae_Bool rae_ext_spotifyFetchArtwork(rae_String url, rae_String outPath) {
    if (!url.data || url.len == 0 || !outPath.data || outPath.len == 0) return false;
    char* url_c = malloc((size_t)url.len + 1);
    char* out_c = malloc((size_t)outPath.len + 1);
    if (!url_c || !out_c) { free(url_c); free(out_c); return false; }
    memcpy(url_c, url.data, (size_t)url.len); url_c[url.len] = '\0';
    memcpy(out_c, outPath.data, (size_t)outPath.len); out_c[outPath.len] = '\0';
    pid_t pid = fork();
    if (pid < 0) { free(url_c); free(out_c); return false; }
    if (pid == 0) {
        char* argv[] = { (char*)"curl", (char*)"-sLf", url_c, (char*)"-o", out_c, NULL };
        execvp("/usr/bin/curl", argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    free(url_c); free(out_c);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void rae_url_encode_append(char* out, size_t* off, size_t cap, const char* s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n && *off + 3 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        if (safe) {
            out[(*off)++] = (char)c;
        } else {
            out[(*off)++] = '%';
            out[(*off)++] = hex[c >> 4];
            out[(*off)++] = hex[c & 0xF];
        }
    }
}

rae_String rae_ext_itunesSearchArtworkUrl(rae_String term) {
    if (!term.data || term.len == 0) return (rae_String){NULL, 0, 0, 0};
    char url[1024];
    size_t off = 0;
    const char* prefix = "https://itunes.apple.com/search?term=";
    size_t plen = strlen(prefix);
    if (plen > sizeof(url)) return (rae_String){NULL, 0, 0, 0};
    memcpy(url, prefix, plen);
    off = plen;
    rae_url_encode_append(url, &off, sizeof(url), (const char*)term.data, (size_t)term.len);
    const char* suffix = "&entity=song&limit=1";
    size_t slen = strlen(suffix);
    if (off + slen + 1 > sizeof(url)) return (rae_String){NULL, 0, 0, 0};
    memcpy(url + off, suffix, slen);
    off += slen;
    url[off] = '\0';
    int fds[2];
    if (pipe(fds) < 0) return (rae_String){NULL, 0, 0, 0};
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return (rae_String){NULL, 0, 0, 0}; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        char* argv[] = { (char*)"curl", (char*)"-sLf", url, NULL };
        execvp("/usr/bin/curl", argv);
        _exit(127);
    }
    close(fds[1]);
    char body[32 * 1024];
    size_t bo = 0;
    while (bo + 1 < sizeof(body)) {
        ssize_t n = read(fds[0], body + bo, sizeof(body) - 1 - bo);
        if (n <= 0) break;
        bo += (size_t)n;
    }
    body[bo] = '\0';
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return (rae_String){NULL, 0, 0, 0};
    const char* key = "\"artworkUrl100\":\"";
    char* k = strstr(body, key);
    if (!k) return (rae_String){NULL, 0, 0, 0};
    k += strlen(key);
    char* end = strchr(k, '"');
    if (!end) return (rae_String){NULL, 0, 0, 0};
    size_t len = (size_t)(end - k);
    char* art = malloc(len + 1);
    if (!art) return (rae_String){NULL, 0, 0, 0};
    memcpy(art, k, len);
    art[len] = '\0';
    char* hit = strstr(art, "100x100");
    if (hit) memcpy(hit, "600x600", 7);
    rae_mem_str_tag((uint8_t*)art, (int64_t)len + 1, RAE_SITE_FROM_CSTR);
    return (rae_String){(uint8_t*)art, (int64_t)len, (int64_t)len + 1, 1};
}

#else  /* !__APPLE__ — Spotify bridge is macOS-only. Stubs return empty/false. */

void rae_ext_spotifyLaunch(void)   {}
void rae_ext_spotifyPlay(void)     {}
void rae_ext_spotifyPause(void)    {}
void rae_ext_spotifyNext(void)     {}
void rae_ext_spotifyPrevious(void) {}
void rae_ext_spotifyRefresh(void)  {}
rae_String rae_ext_spotifyState(void)      { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_spotifyTrackId(void)    { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_spotifyTrackName(void)  { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_spotifyArtistName(void) { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_spotifyAlbumName(void)  { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_spotifyArtworkUrl(void) { return (rae_String){NULL, 0, 0, 0}; }
double rae_ext_spotifyPosition(void)       { return 0.0; }
double rae_ext_spotifyDuration(void)       { return 0.0; }
void rae_ext_spotifyPlayUri(rae_String uri) { (void)uri; }
void rae_ext_spotifyPlayQuery(rae_String query) { (void)query; }
rae_Bool rae_ext_spotifyFetchArtwork(rae_String url, rae_String outPath) { (void)url; (void)outPath; return false; }
rae_String rae_ext_itunesSearchArtworkUrl(rae_String term) { (void)term; return (rae_String){NULL, 0, 0, 0}; }

#endif  /* __APPLE__ */
