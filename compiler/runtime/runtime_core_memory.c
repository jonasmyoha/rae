/* Core process diagnostics, crash handling, stdout flushing, and memory statistics. Permanent C kernel/debug boundary.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/task_info.h>
#include <malloc/malloc.h>
#include <CoreFoundation/CoreFoundation.h>
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
 * Re-raising the signal lets the OS still record the crash + dump core.
 * (WASM has no POSIX signals; the host runtime reports traps itself.) */
#ifndef __wasm__
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
#endif /* !__wasm__ (crash handler) */

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

int64_t rae_ext_rae_mem_stats_buf_outstanding(void) {
  if (!g_mem_stats_enabled) return 0;
  return g_mem_buf_alloc_n - g_mem_buf_free_n;
}

int64_t rae_ext_rae_mem_stats_buf_outstanding_bytes(void) {
  if (!g_mem_stats_enabled) return 0;
  return g_mem_buf_alloc_b - g_mem_buf_free_b;
}

