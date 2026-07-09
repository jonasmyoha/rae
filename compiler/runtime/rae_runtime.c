#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

/* WASM (wasip1) lacks signals, threads, flock, and fork/exec. Pure-compute
 * Rae programs (the raytracer, etc.) use none of these, but the runtime is one
 * monolithic file, so the host-only features are guarded with `#ifndef __wasm__`
 * (with no-op/failure fallbacks where a symbol must remain) so it still
 * compiles for the WASM target. */
#ifndef __wasm__
#include <signal.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
/* pthread.h includes cleanly under wasi-sdk (provides pthread_t for the task
 * struct); only the pthread_* *calls* are guarded out for __wasm__ below. */
#include <pthread.h>
#endif

#if defined(__APPLE__) || defined(__linux__) || defined(__GLIBC__)
#include <execinfo.h>
#define RAE_HAVE_BACKTRACE 1
#endif

/* ----- Task(T) runtime (compiled backend) ----------------------------- */
/* A spawned task gets one RaeTask + one OS thread. The per-spawned-function
 * thunk (emitted by the C backend) runs the function and stores its result
 * into `result`; `rae_task_await` joins exactly once and hands back the
 * buffer, which the get() call site casts to T. */
RaeTask* rae_task_new(size_t result_size) {
  RaeTask* t = (RaeTask*)malloc(sizeof(RaeTask));
  t->result = result_size ? malloc(result_size) : NULL;
  t->done = 0;
  t->joined = 0;
  return t;
}

void* rae_task_await(RaeTask* t) {
  if (!t) return NULL;
  if (!t->joined) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
  }
  return t->result;
}

/* Scope-exit drop (join-on-drop): a Task local that goes out of scope is
 * joined so its worker can't outlive the scope (and isn't killed mid-run at
 * process teardown), then freed. NOTE: the result buffer's own contents are
 * not cascade-dropped here — a heap result that was never get()'d leaks its
 * payload; acceptable for now (callers normally get() the result). */
void rae_task_drop(RaeTask* t) {
  if (!t) return;
  if (!t->joined) {
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
    pthread_join(t->thread, NULL);
#endif
    t->joined = 1;
  }
  free(t->result);
  free(t);
}

/* ----- Channel(T) runtime: MPSC int64 channel (#271) ------------------ */
/* Backs lib/channel.rae's `Channel(T)`, Rae's first message-passing
 * primitive. Multi-producer, single-consumer: any thread may _send; only
 * the owning consumer thread drains via _recv (after checking _count). A
 * pthread mutex serialises every access and is hidden entirely inside the
 * runtime — ordinary Rae code never locks. `recv_count` is the monotonic
 * read-only observable of consumer progress. Fixed ring (cap chosen large
 * enough for the proof); a send to a full ring is dropped (the proof never
 * overflows). The channel is referenced from Rae by an opaque Int handle
 * (this pointer), so a `spawn`'d by-value copy of Channel(T) shares the one
 * underlying channel. The mutex calls are guarded out for single-threaded
 * wasm, matching RaeTask above. */
#define RAE_CHAN_CAP 4096
typedef struct {
  pthread_mutex_t mu;
  int64_t buf[RAE_CHAN_CAP];
  int64_t head;        /* next index to pop */
  int64_t count;       /* items currently buffered */
  int64_t recv_count;  /* total drained (read-only observable) */
} RaeChannel;

int64_t rae_ext_rae_chan_new(void) {
  RaeChannel* c = (RaeChannel*)malloc(sizeof(RaeChannel));
  c->head = 0; c->count = 0; c->recv_count = 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_init(&c->mu, NULL);
#endif
  return (int64_t)(intptr_t)c;
}

void rae_ext_rae_chan_send(int64_t ch, int64_t value) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  if (c->count < RAE_CHAN_CAP) {
    int64_t tail = (c->head + c->count) % RAE_CHAN_CAP;
    c->buf[tail] = value;
    c->count++;
  }
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
}

int64_t rae_ext_rae_chan_count(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  int64_t n = c->count;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
  return n;
}

int64_t rae_ext_rae_chan_recv(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  int64_t v = 0;
  if (c->count > 0) {
    v = c->buf[c->head];
    c->head = (c->head + 1) % RAE_CHAN_CAP;
    c->count--;
    c->recv_count++;
  }
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
  return v;
}

int64_t rae_ext_rae_chan_received(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return 0;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_lock(&c->mu);
#endif
  int64_t n = c->recv_count;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_unlock(&c->mu);
#endif
  return n;
}

void rae_ext_rae_chan_free(int64_t ch) {
  RaeChannel* c = (RaeChannel*)(intptr_t)ch;
  if (!c) return;
#if !defined(__wasm__) || defined(RAE_WASM_THREADS)
  pthread_mutex_destroy(&c->mu);
#endif
  free(c);
}

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
// Thread-local: each OS thread (main + spawned workers) gets its own
// statement-scope String temp pool. Without this, concurrent workers doing
// string interpolation/concat race on a shared pool and corrupt each other's
// temporaries. A returned String is pool_remove'd (detached) before return,
// so it survives the worker's pool flush and is safe to hand to the parent.
static __thread void* g_rae_string_pool[RAE_STRING_POOL_MAX];
static __thread int g_rae_string_pool_count = 0;

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

rae_String rae_ext_time_formatTimestamp(int64_t epoch_ms) {
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

rae_String rae_ext_time_formatDate(int64_t epoch_ms) {
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
#elif defined(__wasm__) && !defined(RAE_WASM_THREADS)
    (void)func; (void)data;  /* single-threaded wasip1: no OS threads */
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, func, data) == 0) {
        pthread_detach(thread);
    }
#endif
}

RaeAny rae_ext_json_get(const char* json, const char* field) {
    if (!json || !field) return rae_any_none();
    
    // Tiny naive JSON parser: look for "field": value
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", field);
    const char* key_pos = strstr(json, search);
    if (!key_pos) return rae_any_none();
    
    const char* colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return rae_any_none();
    
    const char* val_start = colon + 1;
    while (*val_start && (*val_start == ' ' || *val_start == '\t' || *val_start == '\n' || *val_start == '\r')) {
        val_start++;
    }
    
    if (*val_start == '\"') {
        // String value
        val_start++;
        const char* val_end = strchr(val_start, '\"');
        if (!val_end) return rae_any_none();
        size_t len = val_end - val_start;
        uint8_t* res = malloc(len + 1);
        memcpy(res, val_start, len);
        res[len] = '\0';
        rae_mem_str_tag(res, (int64_t)len + 1, RAE_SITE_JSON_GET_STR);
        return rae_any_string((rae_String){res, (int64_t)len, (int64_t)len + 1, 1});
    } else if (*val_start == 't') {
        return rae_any_bool(1);
    } else if (*val_start == 'f') {
        return rae_any_bool(0);
    } else if (*val_start == 'n') {
        return rae_any_none();
    } else if (*val_start == '-' || (*val_start >= '0' && *val_start <= '9')) {
        // Number
        char* end;
        double f = strtod(val_start, &end);
        if (strchr(val_start, '.') && strchr(val_start, '.') < end) {
            return rae_any_float(f);
        } else {
            return rae_any_int((int64_t)f);
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
        return rae_any_string((rae_String){res, (int64_t)len, (int64_t)len + 1, 1}); // We return objects as strings for now
    }
    
    return rae_any_none();
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

#ifndef __wasm__
#include <sys/file.h> // For flock
#endif

rae_Bool rae_ext_rae_sys_exists(rae_String path) {
    if (!path.data) return false;
    return access((const char*)path.data, F_OK) == 0;
}

/* File locking uses flock(), which the WASM sandbox does not provide; there is
 * no cross-process file locking in that environment, so these are no-ops. */
rae_Bool rae_ext_rae_sys_lock_file(rae_String path) {
#ifdef __wasm__
    (void)path; return false;
#else
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
#endif
}

rae_Bool rae_ext_rae_sys_unlock_file(rae_String path) {
#ifdef __wasm__
    (void)path; return false;
#else
    if (!path.data) return false;
    int fd = open((const char*)path.data, O_RDWR);
    if (fd < 0) return false;
    flock(fd, LOCK_UN);
    close(fd);
    return true;
#endif
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

Texture rae_ext_loadStreamTexture(int64_t width, int64_t height) {
    Image img = GenImageColor((int)width, (int)height, BLACK);
    Texture t = LoadTextureFromImage(img);
    UnloadImage(img);
    return t;
}

void rae_ext_updateStreamTexture(Texture texture, const int64_t* pixels, int64_t count) {
    if (!pixels || count <= 0) return;
    /* Reusable scratch for the packed-Int -> RGBA8 expansion. Display runs on
     * the main thread only, so a static buffer is fine and avoids a per-frame
     * malloc of the (large, at Full HD) pixel array. */
    static unsigned char* scratch = NULL;
    static int64_t scratch_count = 0;
    if (count > scratch_count) {
        unsigned char* grown = (unsigned char*)realloc(scratch, (size_t)count * 4);
        if (!grown) return;
        scratch = grown;
        scratch_count = count;
    }
    const int64_t* px = (const int64_t*)pixels;
    for (int64_t i = 0; i < count; i++) {
        int64_t p = px[i];
        scratch[i * 4 + 0] = (unsigned char)((p >> 16) & 0xFF);
        scratch[i * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);
        scratch[i * 4 + 2] = (unsigned char)(p & 0xFF);
        scratch[i * 4 + 3] = 255;
    }
    UpdateTexture(texture, scratch);
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
 * Rae Image API — PNG encode/decode behind a Rae-owned seam.
 *
 * Implementation today: lodepng (C, zlib-licensed, vendored as runtime/
 * lodepng.{c,h}; see lodepng.VENDOR.md). The seam is intentionally thin so the
 * backend is replaceable — the long-term intent is to own the DEFLATE codec +
 * PNG container in Rae (docs/png-and-deflate-strategy.md). Compiled into this
 * single translation unit, so every build path that compiles rae_runtime.c gets
 * it with no extra build-flag plumbing.
 * ============================================================ */
#include "lodepng.h"
#include "lodepng.c"

/* Vendored stb_image (see stb_image.VENDOR.md) — JPEG decode for
 * gpu2d artwork (#228; decision record docs/image-decoding-design.md).
 * Deliberately restricted: STBI_ONLY_JPEG keeps the attack surface to
 * one parser (PNG stays on lodepng above), STBI_NO_STDIO means stb
 * only ever parses bytes we read ourselves, and the dimension cap
 * matches the previous ImageIO limit. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC   /* raylib links its own stb_image; keep ours TU-local */
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

/* Save w*h*4 top-down RGBA8 bytes to `path` as a PNG. Returns 0 on success. */
static int rae_png_save_rgba32(const char* path, const unsigned char* rgba, int w, int h) {
    if (!path || !rgba || w <= 0 || h <= 0) return 1;
    unsigned err = lodepng_encode32_file(path, rgba, (unsigned)w, (unsigned)h);
    if (err) {
        fprintf(stderr, "[image] PNG encode failed: %s\n", lodepng_error_text(err));
        return (int)err;
    }
    return 0;
}

/* Rae Image API (lib/image.rae): encode a packed-0xRRGGBB Int framebuffer
 * (width*height entries, row-major top-down) to a PNG file. */
rae_Bool rae_ext_image_savePng(rae_String path, const int64_t* pixels, int64_t w, int64_t h) {
    if (!path.data || !pixels || w <= 0 || h <= 0) return false;
    size_t count = (size_t)w * (size_t)h;
    unsigned char* rgba = (unsigned char*)malloc(count * 4);
    if (!rgba) return false;
    for (size_t i = 0; i < count; i++) {
        int64_t p = pixels[i];
        rgba[i * 4 + 0] = (unsigned char)((p >> 16) & 0xFF);  /* R */
        rgba[i * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);   /* G */
        rgba[i * 4 + 2] = (unsigned char)(p & 0xFF);          /* B */
        rgba[i * 4 + 3] = 255;                                /* A */
    }
    int rc = rae_png_save_rgba32((const char*)path.data, rgba, (int)w, (int)h);
    free(rgba);
    if (rc == 0) fprintf(stderr, "[image] saved %s (%lldx%lld)\n",
                         (const char*)path.data, (long long)w, (long long)h);
    return rc == 0;
}

/* Rae Image API (lib/image.rae): decode a PNG file into a packed-Int pixel
 * buffer (width*height entries, row-major top-down). Each Int is 0xAARRGGBB:
 * the low 24 bits are RGB (the same domain savePng consumes — feeding the result
 * straight back to savePng round-trips RGB exactly), the top byte is the alpha
 * channel (so RGBA assets like the MTSDF atlas survive the round trip).
 *
 * Returns a freshly rae_buf_alloc'd Buffer(Int) the caller owns; writes the
 * dimensions through the `mod Int` out-params. On failure returns NULL and sets
 * *w = *h = 0 (the caller checks width > 0). */
int64_t* rae_ext_image_loadPng(rae_String path, int64_t* w_out, int64_t* h_out) {
    if (w_out) *w_out = 0;
    if (h_out) *h_out = 0;
    if (!path.data) return NULL;
    unsigned char* rgba = NULL;
    unsigned uw = 0, uh = 0;
    unsigned err = lodepng_decode32_file(&rgba, &uw, &uh, (const char*)path.data);
    if (err) {
        fprintf(stderr, "[image] PNG decode failed: %s\n", lodepng_error_text(err));
        return NULL;
    }
    size_t count = (size_t)uw * (size_t)uh;
    int64_t* pixels = (int64_t*)rae_ext_rae_buf_alloc((int64_t)count, (int64_t)sizeof(int64_t));
    if (!pixels) { free(rgba); return NULL; }
    for (size_t i = 0; i < count; i++) {
        int64_t r = rgba[i * 4 + 0];
        int64_t g = rgba[i * 4 + 1];
        int64_t b = rgba[i * 4 + 2];
        int64_t a = rgba[i * 4 + 3];
        pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;   /* 0xAARRGGBB */
    }
    free(rgba);
    if (w_out) *w_out = (int64_t)uw;
    if (h_out) *h_out = (int64_t)uh;
    fprintf(stderr, "[image] loaded %s (%ux%u)\n", (const char*)path.data, uw, uh);
    return pixels;
}

/* ---- DEFLATE/zlib oracle (lib/compress/oracle.rae) -----------------------
 * lodepng's raw DEFLATE + zlib codecs, exposed so the pure-Rae codec can be
 * validated against a reference (round-trip + interop, per
 * docs/png-and-deflate-strategy.md). Bytes cross as Buffer(Int) (each 0..255);
 * the result is a fresh rae_buf_alloc'd Buffer(Int) the caller owns, with the
 * byte count written through `outLen`. Returns NULL on failure (outLen = 0). */
static int64_t* rae_compress_run(const int64_t* data, int64_t len, int64_t* out_len,
                                 int encode, int zlib) {
    if (out_len) *out_len = 0;
    if (!data || len < 0) return NULL;
    unsigned char* in = (unsigned char*)malloc((size_t)len > 0 ? (size_t)len : 1);
    if (!in) return NULL;
    for (int64_t i = 0; i < len; i++) in[i] = (unsigned char)(data[i] & 0xFF);
    unsigned char* out = NULL; size_t outsize = 0; unsigned err;
    if (encode) {
        LodePNGCompressSettings s = lodepng_default_compress_settings;
        err = zlib ? lodepng_zlib_compress(&out, &outsize, in, (size_t)len, &s)
                   : lodepng_deflate(&out, &outsize, in, (size_t)len, &s);
    } else {
        LodePNGDecompressSettings s = lodepng_default_decompress_settings;
        err = zlib ? lodepng_zlib_decompress(&out, &outsize, in, (size_t)len, &s)
                   : lodepng_inflate(&out, &outsize, in, (size_t)len, &s);
    }
    free(in);
    if (err) { free(out); fprintf(stderr, "[compress] lodepng error: %s\n", lodepng_error_text(err)); return NULL; }
    int64_t* result = (int64_t*)rae_ext_rae_buf_alloc((int64_t)outsize, (int64_t)sizeof(int64_t));
    if (!result) { free(out); return NULL; }
    for (size_t i = 0; i < outsize; i++) result[i] = out[i];
    free(out);
    if (out_len) *out_len = (int64_t)outsize;
    return result;
}
int64_t* rae_ext_compress_oracle_deflate(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 1, 0);
}
int64_t* rae_ext_compress_oracle_inflate(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 0, 0);
}
int64_t* rae_ext_compress_oracle_zlibCompress(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 1, 1);
}
int64_t* rae_ext_compress_oracle_zlibDecompress(const int64_t* data, int64_t len, int64_t* out_len) {
    return rae_compress_run(data, len, out_len, 0, 1);
}
/* Decode a PNG (held in a Buffer(Int) of bytes) to 0xAARRGGBB pixels via
 * lodepng — the oracle for testing the pure-Rae PNG decoder. */
int64_t* rae_ext_compress_oracle_decodePng(const int64_t* data, int64_t len, int64_t* w_out, int64_t* h_out) {
    if (w_out) *w_out = 0; if (h_out) *h_out = 0;
    if (!data || len <= 0) return NULL;
    unsigned char* in = (unsigned char*)malloc((size_t)len);
    if (!in) return NULL;
    for (int64_t i = 0; i < len; i++) in[i] = (unsigned char)(data[i] & 0xFF);
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    unsigned err = lodepng_decode32(&rgba, &uw, &uh, in, (size_t)len);
    free(in);
    if (err) { free(rgba); fprintf(stderr, "[png-oracle] decode: %s\n", lodepng_error_text(err)); return NULL; }
    size_t count = (size_t)uw * (size_t)uh;
    int64_t* px = (int64_t*)rae_ext_rae_buf_alloc((int64_t)count, (int64_t)sizeof(int64_t));
    if (!px) { free(rgba); return NULL; }
    for (size_t i = 0; i < count; i++)
        px[i] = ((int64_t)rgba[i*4+3] << 24) | ((int64_t)rgba[i*4+0] << 16) | ((int64_t)rgba[i*4+1] << 8) | rgba[i*4+2];
    free(rgba);
    if (w_out) *w_out = uw; if (h_out) *h_out = uh;
    return px;
}
/* Encode 0xAARRGGBB pixels to PNG bytes via lodepng — oracle for the encoder. */
int64_t* rae_ext_compress_oracle_encodePng(const int64_t* pixels, int64_t w, int64_t h, int64_t* out_len) {
    if (out_len) *out_len = 0;
    if (!pixels || w <= 0 || h <= 0) return NULL;
    size_t count = (size_t)w * (size_t)h;
    unsigned char* rgba = (unsigned char*)malloc(count * 4);
    if (!rgba) return NULL;
    for (size_t i = 0; i < count; i++) {
        int64_t p = pixels[i];
        rgba[i*4+0] = (unsigned char)((p >> 16) & 0xFF);
        rgba[i*4+1] = (unsigned char)((p >> 8) & 0xFF);
        rgba[i*4+2] = (unsigned char)(p & 0xFF);
        rgba[i*4+3] = (unsigned char)((p >> 24) & 0xFF);
    }
    unsigned char* out = NULL; size_t outsize = 0;
    unsigned err = lodepng_encode32(&out, &outsize, rgba, (unsigned)w, (unsigned)h);
    free(rgba);
    if (err) { free(out); fprintf(stderr, "[png-oracle] encode: %s\n", lodepng_error_text(err)); return NULL; }
    int64_t* res = (int64_t*)rae_ext_rae_buf_alloc((int64_t)outsize, (int64_t)sizeof(int64_t));
    if (!res) { free(out); return NULL; }
    for (size_t i = 0; i < outsize; i++) res[i] = out[i];
    free(out);
    if (out_len) *out_len = (int64_t)outsize;
    return res;
}

/* ============================================================
 * SDL3 desktop platform layer — see lib/sdl3.rae (RAE_HAS_SDL3).
 *
 * A handle-free, single-window windowing/present backend parallel to the
 * raylib block: init creates window + renderer + a streaming texture (kept in
 * file-static globals), updatePixels expands the packed-0xRRGGBB framebuffer to
 * RGBA8 and uploads it, present draws it, shouldClose pumps events.
 * Compiled-target only — the Live VM has no SDL bindings.
 *
 * Headless verification: RAE_SDL_SCREENSHOT=<path.bmp> saves the last uploaded
 * frame on close; RAE_SDL_HEADLESS_MS=<ms> auto-closes after that wall-clock
 * budget — so an agent/CI run can render + snapshot without a human closing the
 * window.
 * ============================================================ */
#ifdef RAE_HAS_SDL3
#include <SDL3/SDL.h>

static SDL_Window*   g_sdl_win = NULL;
static SDL_Renderer* g_sdl_ren = NULL;
static SDL_Texture*  g_sdl_tex = NULL;
static int g_sdl_w = 0, g_sdl_h = 0;            /* window size */
static int g_sdl_tex_w = 0, g_sdl_tex_h = 0;    /* current texture (framebuffer) size */
static unsigned char* g_sdl_scratch = NULL;   /* RGBA8 expansion of the last frame */
static int64_t g_sdl_scratch_px = 0;
static int64_t g_sdl_start_ms = 0;
static int64_t g_sdl_headless_ms = 0;          /* >0 => auto-close after this budget */
static int64_t g_sdl_target_fps = 0;           /* >0 => cap present rate */
static int64_t g_sdl_last_present_ms = 0;
static unsigned char g_sdl_pressed[SDL_SCANCODE_COUNT]; /* went-down-this-frame edges */
static unsigned char g_sdl_keydown[SDL_SCANCODE_COUNT]; /* held state, from key down/up events */
static unsigned char g_sdl_mouse[8];                    /* held state, by SDL button index (1=L,2=M,3=R) */
static unsigned char g_sdl_mouse_pressed[8];            /* went-down-this-frame edges */
static unsigned char g_sdl_mouse_released[8];           /* went-up-this-frame edges */
static bool g_sdl_mouse_captured = false;

/* Map raylib/GLFW key codes (letters = ASCII uppercase, arrows = 262-265, plus
 * a few common keys) to SDL scancodes so ported examples keep their key ints. */
static SDL_Scancode rae_sdl_scancode(int64_t key) {
    if (key >= 'A' && key <= 'Z') return SDL_GetScancodeFromKey((SDL_Keycode)(key + 32), NULL); /* lowercase */
    if (key >= '0' && key <= '9') return SDL_GetScancodeFromKey((SDL_Keycode)key, NULL);
    if (key >= 290 && key <= 301) return (SDL_Scancode)(SDL_SCANCODE_F1 + (int)(key - 290)); /* raylib F1..F12 */
    switch (key) {
        case 32:  return SDL_SCANCODE_SPACE;
        case 256: return SDL_SCANCODE_ESCAPE;
        case 257: return SDL_SCANCODE_RETURN;
        case 262: return SDL_SCANCODE_RIGHT;
        case 263: return SDL_SCANCODE_LEFT;
        case 264: return SDL_SCANCODE_DOWN;
        case 265: return SDL_SCANCODE_UP;
        case 340: return SDL_SCANCODE_LSHIFT;
        default:  return SDL_SCANCODE_UNKNOWN;
    }
}

void rae_ext_sdl3_initWindow(int64_t width, int64_t height, rae_String title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return;
    }
    g_sdl_w = (int)width; g_sdl_h = (int)height;
    const char* t = title.data ? (const char*)title.data : "Rae (SDL3)";
    if (!SDL_CreateWindowAndRenderer(t, (int)width, (int)height, SDL_WINDOW_RESIZABLE, &g_sdl_win, &g_sdl_ren)) {
        fprintf(stderr, "[sdl] window/renderer failed: %s\n", SDL_GetError());
        return;
    }
    /* Texture is created lazily by sdlUpdatePixels (its size can differ from the
     * window and can change at runtime — e.g. a preview/final quality toggle). */
    g_sdl_start_ms = rae_ext_nowMs();
    g_sdl_last_present_ms = g_sdl_start_ms;
    const char* hm = getenv("RAE_SDL_HEADLESS_MS");
    if (hm) g_sdl_headless_ms = (int64_t)atoll(hm);
}

void rae_ext_sdl3_setTargetFPS(int64_t fps) {
    g_sdl_target_fps = fps > 0 ? fps : 0;
}

/* Held key/mouse state is tracked from explicit down/up EVENTS (not the live
 * SDL_GetKeyboardState/SDL_GetMouseState snapshots) so a release is never lost:
 *  - on window focus loss the OS stops sending our up events -> clear all held
 *    state, else a key/button held at focus-out would stick forever;
 *  - while any mouse button is held we SDL_CaptureMouse so a drag that releases
 *    OUTSIDE the window still delivers its button-up (the stuck-drag bug). */
rae_Bool rae_ext_sdl3_shouldClose(void) {
    memset(g_sdl_pressed, 0, sizeof(g_sdl_pressed));
    SDL_Event ev;
    rae_Bool quit = false;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT: quit = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) quit = true;
                if (ev.key.scancode < SDL_SCANCODE_COUNT) {
                    g_sdl_keydown[ev.key.scancode] = 1;
                    if (!ev.key.repeat) g_sdl_pressed[ev.key.scancode] = 1;  /* edge */
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (ev.key.scancode < SDL_SCANCODE_COUNT) g_sdl_keydown[ev.key.scancode] = 0;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button < 8) g_sdl_mouse[ev.button.button] = 1;
                if (!g_sdl_mouse_captured) { SDL_CaptureMouse(true); g_sdl_mouse_captured = true; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (ev.button.button < 8) g_sdl_mouse[ev.button.button] = 0;
                bool any = false;
                for (int b = 0; b < 8; b++) if (g_sdl_mouse[b]) any = true;
                if (!any && g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                /* never see the up events while unfocused -> nothing stays stuck */
                memset(g_sdl_keydown, 0, sizeof(g_sdl_keydown));
                memset(g_sdl_mouse, 0, sizeof(g_sdl_mouse));
                if (g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            default: break;
        }
    }
    if (quit) return true;
    if (g_sdl_headless_ms > 0 && rae_ext_nowMs() - g_sdl_start_ms >= g_sdl_headless_ms) return true;
    return false;
}

int64_t rae_ext_sdl3_getMouseX(void) {
    float x = 0, y = 0; SDL_GetMouseState(&x, &y); return (int64_t)x;
}
int64_t rae_ext_sdl3_getMouseY(void) {
    float x = 0, y = 0; SDL_GetMouseState(&x, &y); return (int64_t)y;
}
/* Current renderer output size in pixels — tracks window resizes (the window
 * is created SDL_WINDOW_RESIZABLE). Apps poll this to re-render at the new size. */
int64_t rae_ext_sdl3_windowWidth(void) {
    int w = g_sdl_w, h = 0; if (g_sdl_ren) SDL_GetRenderOutputSize(g_sdl_ren, &w, &h); return (int64_t)w;
}
int64_t rae_ext_sdl3_windowHeight(void) {
    int w = 0, h = g_sdl_h; if (g_sdl_ren) SDL_GetRenderOutputSize(g_sdl_ren, &w, &h); return (int64_t)h;
}
rae_Bool rae_ext_sdl3_isMouseButtonDown(int64_t button) {
    /* raylib button (0=L,1=R,2=M) -> SDL button index (1=L,2=M,3=R). */
    int sdlb = button == 1 ? SDL_BUTTON_RIGHT : (button == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT);
    return sdlb < 8 && g_sdl_mouse[sdlb] != 0;
}
rae_Bool rae_ext_sdl3_isKeyDown(int64_t key) {
    SDL_Scancode sc = rae_sdl_scancode(key);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT) return false;
    return g_sdl_keydown[sc] != 0;
}
rae_Bool rae_ext_sdl3_isKeyPressed(int64_t key) {
    SDL_Scancode sc = rae_sdl_scancode(key);
    if (sc == SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT) return false;
    return g_sdl_pressed[sc] != 0;
}

void rae_ext_sdl3_updatePixels(const int64_t* pixels, int64_t w, int64_t h) {
    if (!pixels || w <= 0 || h <= 0 || !g_sdl_ren) return;
    int64_t count = w * h;
    /* (Re)create the texture when the framebuffer size changes. */
    if (!g_sdl_tex || (int)w != g_sdl_tex_w || (int)h != g_sdl_tex_h) {
        if (g_sdl_tex) SDL_DestroyTexture(g_sdl_tex);
        g_sdl_tex = SDL_CreateTexture(g_sdl_ren, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
        if (!g_sdl_tex) { fprintf(stderr, "[sdl] texture failed: %s\n", SDL_GetError()); return; }
        SDL_SetTextureScaleMode(g_sdl_tex, SDL_SCALEMODE_LINEAR);
        g_sdl_tex_w = (int)w; g_sdl_tex_h = (int)h;
    }
    if (count > g_sdl_scratch_px) {
        unsigned char* grown = (unsigned char*)realloc(g_sdl_scratch, (size_t)count * 4);
        if (!grown) return;
        g_sdl_scratch = grown; g_sdl_scratch_px = count;
    }
    for (int64_t i = 0; i < count; i++) {
        int64_t p = pixels[i];
        g_sdl_scratch[i * 4 + 0] = (unsigned char)((p >> 16) & 0xFF);  /* R */
        g_sdl_scratch[i * 4 + 1] = (unsigned char)((p >> 8) & 0xFF);   /* G */
        g_sdl_scratch[i * 4 + 2] = (unsigned char)(p & 0xFF);          /* B */
        g_sdl_scratch[i * 4 + 3] = 255;                                /* A */
    }
    SDL_UpdateTexture(g_sdl_tex, NULL, g_sdl_scratch, (int)w * 4);
}

void rae_ext_sdl3_present(void) {
    if (!g_sdl_ren) return;
    SDL_SetRenderDrawColor(g_sdl_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_sdl_ren);
    if (g_sdl_tex) {
        /* Fit the texture into the window preserving its aspect ratio —
         * pillarbox (bars left/right) or letterbox (bars top/bottom) — instead
         * of stretching, so a non-matching window doesn't skew the image. */
        int ow = 0, oh = 0;
        SDL_GetRenderOutputSize(g_sdl_ren, &ow, &oh);
        if (ow > 0 && oh > 0 && g_sdl_tex_w > 0 && g_sdl_tex_h > 0) {
            float ta = (float)g_sdl_tex_w / (float)g_sdl_tex_h;
            float wa = (float)ow / (float)oh;
            SDL_FRect dst;
            if (wa > ta) {            /* window wider than image -> pillarbox */
                dst.h = (float)oh; dst.w = (float)oh * ta;
                dst.x = ((float)ow - dst.w) * 0.5f; dst.y = 0.0f;
            } else {                  /* window taller than image -> letterbox */
                dst.w = (float)ow; dst.h = (float)ow / ta;
                dst.x = 0.0f; dst.y = ((float)oh - dst.h) * 0.5f;
            }
            SDL_RenderTexture(g_sdl_ren, g_sdl_tex, NULL, &dst);
        } else {
            SDL_RenderTexture(g_sdl_ren, g_sdl_tex, NULL, NULL);
        }
    }
    SDL_RenderPresent(g_sdl_ren);
    if (g_sdl_target_fps > 0) {
        int64_t frame_ms = 1000 / g_sdl_target_fps;
        int64_t now = rae_ext_nowMs();
        int64_t elapsed = now - g_sdl_last_present_ms;
        if (elapsed < frame_ms) SDL_Delay((Uint32)(frame_ms - elapsed));
        g_sdl_last_present_ms = rae_ext_nowMs();
    }
}

void rae_ext_sdl3_setTitle(rae_String title) {
    if (g_sdl_win && title.data) SDL_SetWindowTitle(g_sdl_win, (const char*)title.data);
}

void rae_ext_sdl3_closeWindow(void) {
    /* Headless snapshot: dump the last uploaded frame as a BMP (reliable —
     * straight from our pixel buffer, not a GPU read-back). */
    const char* shot = getenv("RAE_SDL_SCREENSHOT");
    if (shot && g_sdl_scratch && g_sdl_tex_w > 0 && g_sdl_tex_h > 0) {
        SDL_Surface* s = SDL_CreateSurfaceFrom(g_sdl_tex_w, g_sdl_tex_h, SDL_PIXELFORMAT_RGBA32,
                                               g_sdl_scratch, g_sdl_tex_w * 4);
        if (s) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
    }
    if (g_sdl_tex) SDL_DestroyTexture(g_sdl_tex);
    if (g_sdl_ren) SDL_DestroyRenderer(g_sdl_ren);
    if (g_sdl_win) SDL_DestroyWindow(g_sdl_win);
    SDL_Quit();
    g_sdl_tex = NULL; g_sdl_ren = NULL; g_sdl_win = NULL;
}

/* ---- MTSDF text compositing into the packed-0xRRGGBB framebuffer (see
 * lib/sdf_text.rae). Rae parses the atlas JSON + lays out glyphs; here we hold
 * the raw RGBA atlas and composite one glyph quad at a time: bilinear-sample
 * the field, median(r,g,b), screenPxRange smoothstep -> coverage, alpha-blend
 * the text colour over the framebuffer. This is the SDL3 (CPU-framebuffer) port
 * of the raylib MSDF shader — no GPU shader needed. ---- */
#define RAE_SDF_MAX_ATLAS 8
static unsigned char* g_sdf_atlas[RAE_SDF_MAX_ATLAS];
static int g_sdf_atlas_w[RAE_SDF_MAX_ATLAS];
static int g_sdf_atlas_h[RAE_SDF_MAX_ATLAS];
static int g_sdf_atlas_n = 0;

int64_t rae_ext_sdf_text_loadAtlas(rae_String path, int64_t w, int64_t h) {
    if (!path.data || w <= 0 || h <= 0 || g_sdf_atlas_n >= RAE_SDF_MAX_ATLAS) return 0;
    FILE* f = fopen((const char*)path.data, "rb");
    if (!f) { fprintf(stderr, "[sdf] cannot open %s\n", (const char*)path.data); return 0; }
    size_t bytes = (size_t)w * (size_t)h * 4;
    unsigned char* px = (unsigned char*)malloc(bytes);
    if (!px) { fclose(f); return 0; }
    size_t got = fread(px, 1, bytes, f);
    fclose(f);
    if (got != bytes) { free(px); fprintf(stderr, "[sdf] short read on %s\n", (const char*)path.data); return 0; }
    g_sdf_atlas[g_sdf_atlas_n] = px; g_sdf_atlas_w[g_sdf_atlas_n] = (int)w; g_sdf_atlas_h[g_sdf_atlas_n] = (int)h;
    return ++g_sdf_atlas_n;  /* 1-based */
}

static float rae_sdf_median(float a, float b, float c) {
    return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
}

/* sx0..sy1: dest rect in framebuffer pixels (sy0 top, sy1 bottom). au0..av1:
 * source rect in atlas pixels, top-left origin. */
void rae_ext_sdf_text_blitGlyph(int64_t* fb, int64_t fbW, int64_t fbH, int64_t atlas,
                          double sx0, double sy0, double sx1, double sy1,
                          double au0, double av0, double au1, double av1,
                          double screenPxRange, int64_t rgb) {
    if (!fb || atlas < 1 || atlas > g_sdf_atlas_n) return;
    const unsigned char* ap = g_sdf_atlas[atlas - 1];
    int aw = g_sdf_atlas_w[atlas - 1], ah = g_sdf_atlas_h[atlas - 1];
    float tr = (float)((rgb >> 16) & 0xFF), tg = (float)((rgb >> 8) & 0xFF), tb = (float)(rgb & 0xFF);
    int x0 = (int)floor(sx0), x1 = (int)ceil(sx1);
    int y0 = (int)floor(sy0), y1 = (int)ceil(sy1);
    double sw = sx1 - sx0, sh = sy1 - sy0;
    if (sw <= 0 || sh <= 0) return;
    for (int py = y0; py < y1; py++) {
        if (py < 0 || py >= fbH) continue;
        for (int px = x0; px < x1; px++) {
            if (px < 0 || px >= fbW) continue;
            double fx = ((double)px + 0.5 - sx0) / sw;
            double fy = ((double)py + 0.5 - sy0) / sh;
            /* atlas sample point (texel-centre bilinear) */
            double gx = au0 + fx * (au1 - au0) - 0.5;
            double gy = av0 + fy * (av1 - av0) - 0.5;
            int ix = (int)floor(gx), iy = (int)floor(gy);
            double tx = gx - ix, ty = gy - iy;
            float chan[3] = {0, 0, 0};
            for (int ch = 0; ch < 3; ch++) {
                float s00, s10, s01, s11;
                #define RAE_SDF_TX(xx, yy) (ap[(((yy) < 0 ? 0 : (yy) >= ah ? ah - 1 : (yy)) * aw + ((xx) < 0 ? 0 : (xx) >= aw ? aw - 1 : (xx))) * 4 + ch] / 255.0f)
                s00 = RAE_SDF_TX(ix, iy);     s10 = RAE_SDF_TX(ix + 1, iy);
                s01 = RAE_SDF_TX(ix, iy + 1); s11 = RAE_SDF_TX(ix + 1, iy + 1);
                #undef RAE_SDF_TX
                float top = s00 + (s10 - s00) * (float)tx;
                float bot = s01 + (s11 - s01) * (float)tx;
                chan[ch] = top + (bot - top) * (float)ty;
            }
            float sd = rae_sdf_median(chan[0], chan[1], chan[2]);
            float cov = (sd - 0.5f) * (float)screenPxRange + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            int64_t bg = fb[py * fbW + px];
            float br = (float)((bg >> 16) & 0xFF), bgc = (float)((bg >> 8) & 0xFF), bb = (float)(bg & 0xFF);
            int rr = (int)(tr * cov + br * (1.0f - cov) + 0.5f);
            int gg = (int)(tg * cov + bgc * (1.0f - cov) + 0.5f);
            int bbv = (int)(tb * cov + bb * (1.0f - cov) + 0.5f);
            fb[py * fbW + px] = (int64_t)((rr << 16) | (gg << 8) | bbv);
        }
    }
}

/* ---- Rae Filesystem & Paths API (lib/filesystem.rae) — thin wrappers over
 * SDL3's SDL_filesystem.h: known folders, mkdir, exists, plus a date helper and
 * a render-output next-index scan. See docs/filesystem-and-paths.md. ---- */
rae_String rae_ext_filesystem_userFolder(int64_t kind) {
    SDL_Folder f = SDL_FOLDER_DESKTOP;
    if (kind == 1) f = SDL_FOLDER_PICTURES;
    else if (kind == 2) f = SDL_FOLDER_DOCUMENTS;
    else if (kind == 3) f = SDL_FOLDER_HOME;
    const char* p = SDL_GetUserFolder(f);
    return rae_str_from_cstr_impl(p ? p : "", RAE_SITE_READ_FILE);
}

rae_String rae_ext_filesystem_prefDir(rae_String org, rae_String app) {
    char* p = SDL_GetPrefPath(org.data ? (const char*)org.data : "Rae",
                              app.data ? (const char*)app.data : "app");
    rae_String s = rae_str_from_cstr_impl(p ? p : "", RAE_SITE_READ_FILE);
    if (p) SDL_free(p);
    return s;
}

rae_Bool rae_ext_filesystem_makeDir(rae_String path) {
    if (!path.data) return false;
    return SDL_CreateDirectory((const char*)path.data);
}

rae_Bool rae_ext_filesystem_exists(rae_String path) {
    if (!path.data) return false;
    SDL_PathInfo info;
    return SDL_GetPathInfo((const char*)path.data, &info);
}

/* Today's local date as "YYYY-MM-DD". */
rae_String rae_ext_filesystem_today(void) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return rae_str_from_cstr_impl(buf, RAE_SITE_READ_FILE);
}

/* Scan `dir` for files named "<prefix><N>.png" and return max(N)+1 (1 if none),
 * so a caller can mint a non-overwriting filename. */
int64_t rae_ext_filesystem_nextIndex(rae_String dir, rae_String prefix) {
    if (!dir.data || !prefix.data) return 1;
    DIR* d = opendir((const char*)dir.data);
    if (!d) return 1;
    int max_n = 0;
    size_t plen = (size_t)prefix.len;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, (const char*)prefix.data, plen) == 0) {
            int v = atoi(e->d_name + plen);
            if (v > max_n) max_n = v;
        }
    }
    closedir(d);
    return (int64_t)(max_n + 1);
}
#endif /* RAE_HAS_SDL3 */

/* ============================================================
 * Native WebGPU via wgpu-native — see lib/webgpu.rae (RAE_HAS_WEBGPU).
 *
 * Runs a WGSL compute shader on the native GPU (Metal/Vulkan/D3D12 through
 * wgpu-native's webgpu.h) — the SAME shader the browser runs (WebGPU-everywhere
 * per docs/tech-stack-and-dependencies.md). The device/queue are created once
 * and cached; each webgpuRaytrace call uploads the scene, dispatches one
 * invocation per pixel, reads the framebuffer back, and writes packed-0xRRGGBB
 * ints (so the SDL3 backend can present it). v29 API: Future/CallbackInfo +
 * WGPUStringView; readback via wgpuBufferMapAsync + wgpuDevicePoll.
 * ============================================================ */
#ifdef RAE_HAS_WEBGPU
#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

static WGPUInstance g_wgpu_inst = NULL;
static WGPUDevice   g_wgpu_dev = NULL;
static WGPUQueue    g_wgpu_queue = NULL;

static WGPUStringView rae_wgpu_sv(const char* s) { WGPUStringView v; v.data = s; v.length = WGPU_STRLEN; return v; }

static WGPUAdapter g_wgpu_adapter; static int g_wgpu_adapter_done;
static void rae_wgpu_on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_adapter = a; g_wgpu_adapter_done = 1;
}
static int g_wgpu_device_done;
static void rae_wgpu_on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_dev = d; g_wgpu_device_done = 1;
}
static int g_wgpu_map_done;
static void rae_wgpu_on_map(WGPUMapAsyncStatus st, WGPUStringView m, void* u1, void* u2) {
    (void)st;(void)m;(void)u1;(void)u2; g_wgpu_map_done = 1;
}

static int rae_wgpu_init(void) {
    if (g_wgpu_dev) return 1;
    g_wgpu_inst = wgpuCreateInstance(NULL);
    if (!g_wgpu_inst) { fprintf(stderr, "[wgpu] no instance\n"); return 0; }
    WGPURequestAdapterOptions ao; memset(&ao, 0, sizeof(ao));
    WGPURequestAdapterCallbackInfo aci; memset(&aci, 0, sizeof(aci));
    aci.mode = WGPUCallbackMode_AllowProcessEvents; aci.callback = rae_wgpu_on_adapter;
    wgpuInstanceRequestAdapter(g_wgpu_inst, &ao, aci);
    while (!g_wgpu_adapter_done) wgpuInstanceProcessEvents(g_wgpu_inst);
    if (!g_wgpu_adapter) { fprintf(stderr, "[wgpu] no adapter\n"); return 0; }
    WGPURequestDeviceCallbackInfo dci; memset(&dci, 0, sizeof(dci));
    dci.mode = WGPUCallbackMode_AllowProcessEvents; dci.callback = rae_wgpu_on_device;
    wgpuAdapterRequestDevice(g_wgpu_adapter, NULL, dci);
    while (!g_wgpu_device_done) wgpuInstanceProcessEvents(g_wgpu_inst);
    if (!g_wgpu_dev) { fprintf(stderr, "[wgpu] no device\n"); return 0; }
    g_wgpu_queue = wgpuDeviceGetQueue(g_wgpu_dev);
    return 1;
}

/* scene: sceneLen f64 (camera 19 + spheres*10) -> narrowed to f32 for the GPU.
 * fb: width*height int64 written as packed 0xRRGGBB. wgsl: shader source. */
void rae_ext_webgpu_raytrace(const double* scene, int64_t sceneLen, int64_t* fb,
                            int64_t width, int64_t height, int64_t samples,
                            int64_t maxDepth, rae_String wgsl) {
    if (!fb || width <= 0 || height <= 0) return;
    if (!rae_wgpu_init()) return;

    float* sf = (float*)malloc((size_t)sceneLen * sizeof(float));
    if (!sf) return;
    for (int64_t i = 0; i < sceneLen; i++) sf[i] = (float)scene[i];
    int64_t sphereCount = (sceneLen - 19) / 10;

    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = wgsl.data ? rae_wgpu_sv((const char*)wgsl.data) : rae_wgpu_sv("");
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    uint32_t params[8] = { (uint32_t)width, (uint32_t)height, (uint32_t)samples,
                           (uint32_t)maxDepth, (uint32_t)sphereCount, 0, 0, 0 };
    WGPUBufferDescriptor pbd; memset(&pbd, 0, sizeof(pbd));
    pbd.size = sizeof(params); pbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    WGPUBuffer pbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &pbd);
    wgpuQueueWriteBuffer(g_wgpu_queue, pbuf, 0, params, sizeof(params));

    size_t scene_bytes = (size_t)sceneLen * sizeof(float);
    WGPUBufferDescriptor sbd; memset(&sbd, 0, sizeof(sbd));
    sbd.size = scene_bytes; sbd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    WGPUBuffer sbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &sbd);
    wgpuQueueWriteBuffer(g_wgpu_queue, sbuf, 0, sf, scene_bytes);

    size_t obytes = (size_t)width * (size_t)height * 4;
    WGPUBufferDescriptor obd; memset(&obd, 0, sizeof(obd));
    obd.size = obytes; obd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    WGPUBuffer obuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &obd);
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = obytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer rbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);

    WGPUComputePipelineDescriptor cpd; memset(&cpd, 0, sizeof(cpd));
    cpd.compute.module = mod; cpd.compute.entryPoint = rae_wgpu_sv("main");
    WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(g_wgpu_dev, &cpd);
    if (!pipe) { fprintf(stderr, "[wgpu] pipeline failed\n"); free(sf); return; }

    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroupEntry be[3]; memset(be, 0, sizeof(be));
    be[0].binding = 0; be[0].buffer = pbuf; be[0].size = sizeof(params);
    be[1].binding = 1; be[1].buffer = sbuf; be[1].size = scene_bytes;
    be[2].binding = 2; be[2].buffer = obuf; be[2].size = obytes;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUComputePassDescriptor cpassd; memset(&cpassd, 0, sizeof(cpassd));
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpassd);
    wgpuComputePassEncoderSetPipeline(pass, pipe);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (uint32_t)((width + 7) / 8), (uint32_t)((height + 7) / 8), 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuCommandEncoderCopyBufferToBuffer(enc, obuf, 0, rbuf, 0, obytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);

    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    g_wgpu_map_done = 0;
    wgpuBufferMapAsync(rbuf, WGPUMapMode_Read, 0, obytes, mci);
    while (!g_wgpu_map_done) wgpuDevicePoll(g_wgpu_dev, true, NULL);
    const uint32_t* px = (const uint32_t*)wgpuBufferGetConstMappedRange(rbuf, 0, obytes);
    if (px) {
        int64_t n = width * height;
        for (int64_t i = 0; i < n; i++) {
            uint32_t p = px[i];                      /* GPU packed R|G<<8|B<<16|A<<24 */
            uint32_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
            fb[i] = (int64_t)((r << 16) | (g << 8) | b);  /* -> 0xRRGGBB for SDL */
        }
    }
    wgpuBufferUnmap(rbuf);
    /* per-call GPU objects */
    wgpuBindGroupRelease(bg); wgpuBindGroupLayoutRelease(bgl);
    wgpuComputePipelineRelease(pipe); wgpuShaderModuleRelease(mod);
    wgpuBufferRelease(rbuf); wgpuBufferRelease(obuf);
    wgpuBufferRelease(sbuf); wgpuBufferRelease(pbuf);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    wgpuComputePassEncoderRelease(pass);
    free(sf);
}

/* ---- Generic GPU compute (lib/gpu.rae): handle tables over the cached
 * device, so arbitrary WGSL kernels + buffers can be authored from Rae, not
 * just the raytracer. Handles are 1-based (0 = failure). ---- */
#define RAE_GPU_MAX_BUF 256
#define RAE_GPU_MAX_PIPE 64
static WGPUBuffer g_gpu_buf[RAE_GPU_MAX_BUF];
static size_t     g_gpu_buf_size[RAE_GPU_MAX_BUF];
static int        g_gpu_buf_n = 0;
static WGPUComputePipeline g_gpu_pipe[RAE_GPU_MAX_PIPE];
static int        g_gpu_pipe_n = 0;

static int rae_gpu_add_buf(WGPUBuffer b, size_t size) {
    if (!b || g_gpu_buf_n >= RAE_GPU_MAX_BUF) return 0;
    g_gpu_buf[g_gpu_buf_n] = b; g_gpu_buf_size[g_gpu_buf_n] = size;
    return ++g_gpu_buf_n;  /* 1-based handle */
}

int64_t rae_ext_gpu_storageF32(const double* data, int64_t count) {
    if (!rae_wgpu_init() || count <= 0) return 0;
    size_t bytes = (size_t)count * 4;
    float* tmp = (float*)malloc(bytes);
    if (!tmp) return 0;
    for (int64_t i = 0; i < count; i++) tmp[i] = (float)data[i];
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = bytes; bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    WGPUBuffer b = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    if (b) wgpuQueueWriteBuffer(g_wgpu_queue, b, 0, tmp, bytes);
    free(tmp);
    return rae_gpu_add_buf(b, bytes);
}
int64_t rae_ext_gpu_uniformU32(const int64_t* data, int64_t count) {
    if (!rae_wgpu_init() || count <= 0) return 0;
    size_t bytes = (size_t)count * 4;
    uint32_t* tmp = (uint32_t*)malloc(bytes);
    if (!tmp) return 0;
    for (int64_t i = 0; i < count; i++) tmp[i] = (uint32_t)data[i];
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = bytes; bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    WGPUBuffer b = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    if (b) wgpuQueueWriteBuffer(g_wgpu_queue, b, 0, tmp, bytes);
    free(tmp);
    return rae_gpu_add_buf(b, bytes);
}
static int64_t rae_gpu_alloc(int64_t count) {
    if (!rae_wgpu_init() || count <= 0) return 0;
    size_t bytes = (size_t)count * 4;
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = bytes; bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    WGPUBuffer b = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    return rae_gpu_add_buf(b, bytes);
}
int64_t rae_ext_gpu_allocF32(int64_t count) { return rae_gpu_alloc(count); }
int64_t rae_ext_gpu_allocU32(int64_t count) { return rae_gpu_alloc(count); }

void rae_ext_gpu_writeF32(int64_t buf, const double* data, int64_t count) {
    if (!g_wgpu_queue || buf < 1 || buf > g_gpu_buf_n || count <= 0) return;
    size_t bytes = (size_t)count * 4;
    float* tmp = (float*)malloc(bytes);
    if (!tmp) return;
    for (int64_t i = 0; i < count; i++) tmp[i] = (float)data[i];
    wgpuQueueWriteBuffer(g_wgpu_queue, g_gpu_buf[buf - 1], 0, tmp, bytes);
    free(tmp);
}
void rae_ext_gpu_writeU32(int64_t buf, const int64_t* data, int64_t count) {
    if (!g_wgpu_queue || buf < 1 || buf > g_gpu_buf_n || count <= 0) return;
    size_t bytes = (size_t)count * 4;
    uint32_t* tmp = (uint32_t*)malloc(bytes);
    if (!tmp) return;
    for (int64_t i = 0; i < count; i++) tmp[i] = (uint32_t)data[i];
    wgpuQueueWriteBuffer(g_wgpu_queue, g_gpu_buf[buf - 1], 0, tmp, bytes);
    free(tmp);
}

int64_t rae_ext_gpu_kernel(rae_String wgsl, rae_String entry) {
    if (!rae_wgpu_init() || g_gpu_pipe_n >= RAE_GPU_MAX_PIPE) return 0;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(wgsl.data ? (const char*)wgsl.data : "");
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);
    WGPUComputePipelineDescriptor cpd; memset(&cpd, 0, sizeof(cpd));
    cpd.compute.module = mod;
    cpd.compute.entryPoint = rae_wgpu_sv(entry.data ? (const char*)entry.data : "main");
    WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(g_wgpu_dev, &cpd);
    wgpuShaderModuleRelease(mod);
    if (!pipe) { fprintf(stderr, "[gpu] kernel compile failed\n"); return 0; }
    g_gpu_pipe[g_gpu_pipe_n] = pipe;
    return ++g_gpu_pipe_n;  /* 1-based */
}

void rae_ext_gpu_run(int64_t kernel, const int64_t* bufs, int64_t bufCount,
                    int64_t gx, int64_t gy, int64_t gz) {
    if (!g_wgpu_dev || kernel < 1 || kernel > g_gpu_pipe_n || bufCount < 0 || bufCount > 16) return;
    WGPUComputePipeline pipe = g_gpu_pipe[kernel - 1];
    WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
    WGPUBindGroupEntry be[16]; memset(be, 0, sizeof(be));
    for (int64_t i = 0; i < bufCount; i++) {
        int64_t h = bufs[i];
        if (h < 1 || h > g_gpu_buf_n) return;
        be[i].binding = (uint32_t)i;
        be[i].buffer = g_gpu_buf[h - 1];
        be[i].size = g_gpu_buf_size[h - 1];
    }
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = (size_t)bufCount; bgd.entries = be;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUComputePassDescriptor cpassd; memset(&cpassd, 0, sizeof(cpassd));
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpassd);
    wgpuComputePassEncoderSetPipeline(pass, pipe);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (uint32_t)gx, (uint32_t)gy, (uint32_t)gz);
    wgpuComputePassEncoderEnd(pass);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuDevicePoll(g_wgpu_dev, true, NULL);  /* wait for completion */
    wgpuCommandBufferRelease(cmd); wgpuComputePassEncoderRelease(pass);
    wgpuCommandEncoderRelease(enc); wgpuBindGroupRelease(bg); wgpuBindGroupLayoutRelease(bgl);
}

/* Copy a GPU buffer to a staging buffer, map it, and read into `out`. */
static const void* rae_gpu_readback(int64_t buf, size_t bytes, WGPUBuffer* staging_out) {
    if (!g_wgpu_dev || buf < 1 || buf > g_gpu_buf_n) return NULL;
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = bytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    wgpuCommandEncoderCopyBufferToBuffer(enc, g_gpu_buf[buf - 1], 0, staging, 0, bytes);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    g_wgpu_map_done = 0;
    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
    while (!g_wgpu_map_done) wgpuDevicePoll(g_wgpu_dev, true, NULL);
    *staging_out = staging;
    return wgpuBufferGetConstMappedRange(staging, 0, bytes);
}
void rae_ext_gpu_downloadF32(int64_t buf, double* out, int64_t count) {
    if (!out || count <= 0) return;
    WGPUBuffer staging = NULL;
    const float* p = (const float*)rae_gpu_readback(buf, (size_t)count * 4, &staging);
    if (p) for (int64_t i = 0; i < count; i++) out[i] = (double)p[i];
    if (staging) { wgpuBufferUnmap(staging); wgpuBufferRelease(staging); }
}
void rae_ext_gpu_downloadU32(int64_t buf, int64_t* out, int64_t count) {
    if (!out || count <= 0) return;
    WGPUBuffer staging = NULL;
    const uint32_t* p = (const uint32_t*)rae_gpu_readback(buf, (size_t)count * 4, &staging);
    if (p) for (int64_t i = 0; i < count; i++) out[i] = (int64_t)p[i];
    if (staging) { wgpuBufferUnmap(staging); wgpuBufferRelease(staging); }
}
void rae_ext_gpu_reset(void) {
    for (int i = 0; i < g_gpu_buf_n; i++) if (g_gpu_buf[i]) wgpuBufferRelease(g_gpu_buf[i]);
    for (int i = 0; i < g_gpu_pipe_n; i++) if (g_gpu_pipe[i]) wgpuComputePipelineRelease(g_gpu_pipe[i]);
    g_gpu_buf_n = 0; g_gpu_pipe_n = 0;
}

/* ============================================================
 * GPU 2D renderer surface — lib/gpu2d.rae (#109,
 * docs/webgpu-2d-ui-renderer.md). Wraps the SDL3 window's
 * CAMetalLayer as a wgpu surface and presents frames on the GPU
 * (no CPU framebuffer round-trip, unlike sdl3.updatePixels).
 * Needs both SDL3 (window) and wgpu-native (surface); main.c links
 * both when lib/gpu2d.rae is imported. Window globals (g_sdl_win,
 * g_sdl_w/h) are shared with the SDL3 block so sdl3 input works.
 * Tier 0 slice: window + clear-colour present.
 * ============================================================ */
#ifdef RAE_HAS_SDL3
static SDL_MetalView g_g2d_metal_view = NULL;
static WGPUSurface   g_g2d_surface = NULL;
static WGPUTextureFormat g_g2d_fmt = WGPUTextureFormat_BGRA8Unorm;

/* Coordinate system (#112): draw coords are in DESIGN units; the renderer
 * maps them to physical pixels via a per-frame scale + offset. Default
 * (design w/h <= 0) is identity — 1 unit = 1 physical px. setDesignResolution
 * opts into a fixed virtual canvas fitted into the window (DPI-independent). */
static double g_g2d_design_w = 0.0, g_g2d_design_h = 0.0;
static int    g_g2d_fit_mode = 0;   /* 0=fit/contain, 1=fill/cover, 2=stretch */
/* Per-frame mouse-wheel accumulator (reset + summed in pollClose, read by
 * gpu2d.wheelMove). Mirrors raylib's GetMouseWheelMove per-frame semantics. */
static float  g_g2d_wheel = 0.0f;
/* Set when the OS reports a window resize; consumed (cleared) once by
 * gpu2d.windowResized() so the app rebuilds its layout for the new size. */
static int    g_g2d_win_resized = 0;
static int    g_g2d_cursor_kind = -1;
static SDL_Cursor* g_g2d_cursors[7] = {0};
/* Last endFrame surface-present result. Rendering always targets the offscreen
 * texture first, but startup/occlusion can make the surface drawable
 * unavailable; apps use this to keep their first visible frame dirty. */
static int    g_g2d_last_present_ok = 0;

/* Fill `out` (8 floats = 2*vec4): (physW,physH,scaleX,scaleY),(offX,offY,0,0). */
static void rae_g2d_compute_xform(float* out) {
    float physW = (float)g_sdl_w, physH = (float)g_sdl_h;
    float scaleX = 1.0f, scaleY = 1.0f, offX = 0.0f, offY = 0.0f;
    if (g_g2d_design_w > 0.0 && g_g2d_design_h > 0.0) {
        float dW = (float)g_g2d_design_w, dH = (float)g_g2d_design_h;
        float sx = physW / dW, sy = physH / dH;
        if (g_g2d_fit_mode == 2) { scaleX = sx; scaleY = sy; }       /* stretch */
        else {
            float s = (g_g2d_fit_mode == 1) ? (sx > sy ? sx : sy)    /* fill/cover */
                                            : (sx < sy ? sx : sy);   /* fit/contain */
            scaleX = s; scaleY = s;
            offX = (physW - dW * s) * 0.5f;
            offY = (physH - dH * s) * 0.5f;
        }
    }
    out[0] = physW; out[1] = physH; out[2] = scaleX; out[3] = scaleY;
    out[4] = offX;  out[5] = offY;  out[6] = 0.0f;   out[7] = 0.0f;
}
/* per-frame transient handles */
static WGPUCommandEncoder    g_g2d_enc = NULL;
static WGPURenderPassEncoder g_g2d_pass = NULL;

/* ---- Clip / scissor (#144) --------------------------------------------
 * A clip-rect stack in DESIGN units. Each queued box primitive, glyph, and
 * image records the current clip index (parallel arrays); at flush we set
 * the render-pass scissor per contiguous run of same-clip draws, so a run
 * is drawn with wgpuRenderPassEncoderDraw's firstInstance offset. Index 0
 * is the sentinel "no clip" (full framebuffer). pushClipRect intersects
 * with the parent so nested clips compose (a rounded card holding a scroll
 * list clips to the intersection). Reset each beginFrame. The rounded /
 * per-instance clip variant is #118; this is the axis-aligned fast path. */
#define RAE_G2D_MAX_CLIPS 256
typedef struct { float x, y, w, h, radius; int full; } RaeG2dClip;
static RaeG2dClip g_g2d_clips[RAE_G2D_MAX_CLIPS];
static int g_g2d_clip_count = 1;         /* [0] = full sentinel */
static int g_g2d_clip_stack[RAE_G2D_MAX_CLIPS];
static int g_g2d_clip_sp = 0;            /* stack depth */
static int g_g2d_cur_clip = 0;           /* current clip index */
/* parallel clip index per box primitive */
static int* g_g2d_prim_clip = NULL;
static int  g_g2d_prim_clip_cap = 0;     /* in prims */
/* parallel clip index per glyph, per atlas */
static int* g_g2d_text_clip[RAE_SDF_MAX_ATLAS];
static int  g_g2d_text_clip_cap[RAE_SDF_MAX_ATLAS];

static float rae_g2d_maxf(float a, float b) { return a > b ? a : b; }
static float rae_g2d_minf(float a, float b) { return a < b ? a : b; }

static void rae_g2d_clip_reset(void) {
    g_g2d_clips[0].full = 1;
    g_g2d_clips[0].x = 0.0f; g_g2d_clips[0].y = 0.0f;
    g_g2d_clips[0].w = 0.0f; g_g2d_clips[0].h = 0.0f;
    g_g2d_clips[0].radius = 0.0f;
    g_g2d_clip_count = 1;
    g_g2d_clip_sp = 0;
    g_g2d_cur_clip = 0;
}

static void rae_g2d_prim_clip_ensure(int prims) {
    if (prims <= g_g2d_prim_clip_cap) return;
    int cap = g_g2d_prim_clip_cap ? g_g2d_prim_clip_cap : 64;
    while (cap < prims) cap *= 2;
    g_g2d_prim_clip = (int*)realloc(g_g2d_prim_clip, (size_t)cap * sizeof(int));
    g_g2d_prim_clip_cap = cap;
}

static void rae_g2d_text_clip_ensure(int ai, int glyphs) {
    if (glyphs <= g_g2d_text_clip_cap[ai]) return;
    int cap = g_g2d_text_clip_cap[ai] ? g_g2d_text_clip_cap[ai] : 256;
    while (cap < glyphs) cap *= 2;
    g_g2d_text_clip[ai] = (int*)realloc(g_g2d_text_clip[ai], (size_t)cap * sizeof(int));
    g_g2d_text_clip_cap[ai] = cap;
}

/* Resolve a clip index to a framebuffer-pixel scissor rect (via the design→
 * physical xform) and set it on the active pass, clamped to the attachment. */
static void rae_g2d_set_scissor(int clipidx) {
    if (!g_g2d_pass) return;
    float xf[8]; rae_g2d_compute_xform(xf);
    float physW = xf[0], physH = xf[1], sx = xf[2], sy = xf[3], ox = xf[4], oy = xf[5];
    float x0, y0, x1, y1;
    if (clipidx <= 0 || clipidx >= g_g2d_clip_count || g_g2d_clips[clipidx].full) {
        x0 = 0.0f; y0 = 0.0f; x1 = physW; y1 = physH;
    } else {
        RaeG2dClip* c = &g_g2d_clips[clipidx];
        x0 = c->x * sx + ox;            y0 = c->y * sy + oy;
        x1 = (c->x + c->w) * sx + ox;   y1 = (c->y + c->h) * sy + oy;
    }
    x0 = rae_g2d_maxf(0.0f, x0); y0 = rae_g2d_maxf(0.0f, y0);
    x1 = rae_g2d_minf(physW, x1); y1 = rae_g2d_minf(physH, y1);
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    uint32_t ix = (uint32_t)(x0 + 0.5f), iy = (uint32_t)(y0 + 0.5f);
    uint32_t iw = (uint32_t)(x1 - x0 + 0.5f), ih = (uint32_t)(y1 - y0 + 0.5f);
    uint32_t pw = (uint32_t)(physW + 0.5f), ph = (uint32_t)(physH + 0.5f);
    if (ix > pw) ix = pw;
    if (iy > ph) iy = ph;
    if (ix + iw > pw) iw = pw - ix;
    if (iy + ih > ph) ih = ph - iy;
    wgpuRenderPassEncoderSetScissorRect(g_g2d_pass, ix, iy, iw, ih);
}

static void rae_g2d_push_clip(double x, double y, double w, double h, double radius) {
    RaeG2dClip child; child.full = 0; child.radius = (float)radius;
    RaeG2dClip* parent = &g_g2d_clips[g_g2d_cur_clip];
    if (parent->full) {
        child.x = (float)x; child.y = (float)y; child.w = (float)w; child.h = (float)h;
    } else {
        float px0 = rae_g2d_maxf(parent->x, (float)x);
        float py0 = rae_g2d_maxf(parent->y, (float)y);
        float px1 = rae_g2d_minf(parent->x + parent->w, (float)(x + w));
        float py1 = rae_g2d_minf(parent->y + parent->h, (float)(y + h));
        child.x = px0; child.y = py0;
        child.w = rae_g2d_maxf(0.0f, px1 - px0);
        child.h = rae_g2d_maxf(0.0f, py1 - py0);
        /* Keep the larger of the two radii — a rounded child inside a
         * rectangular parent still wants its own rounding; the scissor
         * bbox already enforces the parent's straight edges. */
        child.radius = rae_g2d_maxf((float)radius, parent->radius);
    }
    int idx = g_g2d_cur_clip;
    if (g_g2d_clip_count < RAE_G2D_MAX_CLIPS) {
        idx = g_g2d_clip_count++;
        g_g2d_clips[idx] = child;
    }
    if (g_g2d_clip_sp < RAE_G2D_MAX_CLIPS) g_g2d_clip_stack[g_g2d_clip_sp++] = g_g2d_cur_clip;
    g_g2d_cur_clip = idx;
}

/* Fill an 8-float box-clip uniform: [x,y,w,h] design units + [radius,enabled].
 * `enabled` is set only for a rounded clip — a rectangular clip is handled by
 * the scissor alone, so its SDF stays off. */
static void rae_g2d_fill_clip_uniform(int clipidx, float* cu) {
    if (clipidx > 0 && clipidx < g_g2d_clip_count
        && !g_g2d_clips[clipidx].full && g_g2d_clips[clipidx].radius > 0.0f) {
        RaeG2dClip* c = &g_g2d_clips[clipidx];
        cu[0] = c->x; cu[1] = c->y; cu[2] = c->w; cu[3] = c->h;
        cu[4] = c->radius; cu[5] = 1.0f; cu[6] = 0.0f; cu[7] = 0.0f;
    } else {
        for (int i = 0; i < 8; i++) cu[i] = 0.0f;
    }
}

void rae_ext_gpu2d_pushClipRect(double x, double y, double w, double h) {
    rae_g2d_push_clip(x, y, w, h, 0.0);
}

/* #118: rounded clip. The box pipeline applies the rounded-rect SDF in the
 * fragment shader (analytic AA on the corners); the axis-aligned scissor
 * (#144) still bounds all pipelines to the clip bbox. */
void rae_ext_gpu2d_pushClipRoundedRect(double x, double y, double w, double h, double radius) {
    rae_g2d_push_clip(x, y, w, h, radius);
}

void rae_ext_gpu2d_popClipRect(void) {
    if (g_g2d_clip_sp > 0) {
        g_g2d_cur_clip = g_g2d_clip_stack[--g_g2d_clip_sp];
    } else {
        g_g2d_cur_clip = 0;
    }
}

/* Frames render to this persistent OFFSCREEN texture, not directly to the
 * surface drawable. At endFrame we read it back for screenshots and copy it to
 * the surface drawable for present *best-effort* — so rendering + headless
 * screenshots work even when the OS won't vend a drawable (window occluded /
 * display asleep / headless), where wgpuSurfaceGetCurrentTexture returns no
 * texture. */
static WGPUTexture     g_g2d_off_tex = NULL;
static WGPUTextureView g_g2d_off_view = NULL;
static int g_g2d_off_w = 0, g_g2d_off_h = 0;

static void rae_g2d_configure(int pw, int ph) {
    if (!g_g2d_surface || pw <= 0 || ph <= 0) return;
    WGPUSurfaceConfiguration cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.device = g_wgpu_dev;
    cfg.format = g_g2d_fmt;
    /* CopyDst so we can copy our offscreen render into the drawable to present. */
    cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
    cfg.width = (uint32_t)pw;
    cfg.height = (uint32_t)ph;
    cfg.presentMode = WGPUPresentMode_Fifo;
    cfg.alphaMode = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(g_g2d_surface, &cfg);
    g_sdl_w = pw; g_sdl_h = ph;
    /* (Re)create the offscreen render target at the new size. */
    if (g_g2d_off_w != pw || g_g2d_off_h != ph || !g_g2d_off_tex) {
        if (g_g2d_off_view) { wgpuTextureViewRelease(g_g2d_off_view); g_g2d_off_view = NULL; }
        if (g_g2d_off_tex)  { wgpuTextureRelease(g_g2d_off_tex);  g_g2d_off_tex = NULL; }
        WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        td.dimension = WGPUTextureDimension_2D;
        td.size.width = (uint32_t)pw; td.size.height = (uint32_t)ph; td.size.depthOrArrayLayers = 1;
        td.format = g_g2d_fmt; td.mipLevelCount = 1; td.sampleCount = 1;
        g_g2d_off_tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
        g_g2d_off_view = wgpuTextureCreateView(g_g2d_off_tex, NULL);
        g_g2d_off_w = pw; g_g2d_off_h = ph;
    }
}

void rae_ext_gpu2d_initWindow(int64_t width, int64_t height, rae_String title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[gpu2d] SDL init failed: %s\n", SDL_GetError());
        return;
    }
    const char* t = title.data ? (const char*)title.data : "Rae (GPU 2D)";
    g_sdl_win = SDL_CreateWindow(t, (int)width, (int)height,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_sdl_win) { fprintf(stderr, "[gpu2d] window failed: %s\n", SDL_GetError()); return; }
    SDL_RaiseWindow(g_sdl_win);
    g_g2d_metal_view = SDL_Metal_CreateView(g_sdl_win);
    if (!g_g2d_metal_view) { fprintf(stderr, "[gpu2d] metal view failed: %s\n", SDL_GetError()); return; }
    void* layer = SDL_Metal_GetLayer(g_g2d_metal_view);

    if (!rae_wgpu_init()) { fprintf(stderr, "[gpu2d] wgpu init failed\n"); return; }

    WGPUSurfaceSourceMetalLayer ms; memset(&ms, 0, sizeof(ms));
    ms.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    ms.layer = layer;
    WGPUSurfaceDescriptor sd; memset(&sd, 0, sizeof(sd));
    sd.nextInChain = &ms.chain;
    g_g2d_surface = wgpuInstanceCreateSurface(g_wgpu_inst, &sd);
    if (!g_g2d_surface) { fprintf(stderr, "[gpu2d] surface failed\n"); return; }

    WGPUSurfaceCapabilities caps; memset(&caps, 0, sizeof(caps));
    wgpuSurfaceGetCapabilities(g_g2d_surface, g_wgpu_adapter, &caps);
    if (caps.formatCount > 0) {
        g_g2d_fmt = caps.formats[0];
        for (size_t i = 0; i < caps.formatCount; i++) {
            if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm) { g_g2d_fmt = WGPUTextureFormat_BGRA8Unorm; break; }
        }
    }
    int pw = (int)width, ph = (int)height;
    SDL_GetWindowSizeInPixels(g_sdl_win, &pw, &ph);
    rae_g2d_configure(pw, ph);

    /* Test hook: RAE_GPU2D_TEST_RESIZE=WxH resizes the window (logical
     * points) just after boot so the resize path can be exercised
     * headlessly. Fires a WINDOW_PIXEL_SIZE_CHANGED on the next poll. */
    const char* trs = getenv("RAE_GPU2D_TEST_RESIZE");
    if (trs) {
        int rw = 0, rh = 0;
        if (sscanf(trs, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            SDL_SetWindowSize(g_sdl_win, rw, rh);
        }
    }

    g_sdl_start_ms = rae_ext_nowMs();
    const char* hm = getenv("RAE_SDL_HEADLESS_MS");
    if (hm) g_sdl_headless_ms = (int64_t)atoll(hm);
}

/* Pump the OS event queue once per frame AND record input state into the
 * shared SDL3 input arrays (g_sdl_mouse / g_sdl_keydown / g_sdl_pressed) plus
 * the wheel accumulator, so gpu2d apps get working mouse/keyboard/wheel input.
 * (The bare SDL3 backend records this in sdl3_shouldClose, which the gpu2d
 * window path never calls — hence the duplication here.) Edge state
 * (g_sdl_pressed) and the wheel delta are reset each call so they describe
 * only this frame. */
rae_Bool rae_ext_gpu2d_pollClose(void) {
    memset(g_sdl_pressed, 0, sizeof(g_sdl_pressed));
    memset(g_sdl_mouse_pressed, 0, sizeof(g_sdl_mouse_pressed));
    memset(g_sdl_mouse_released, 0, sizeof(g_sdl_mouse_released));
    g_g2d_wheel = 0.0f;
    SDL_Event e;
    rae_Bool quit = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: quit = 1; break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit = 1; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED: {
                int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(g_sdl_win, &pw, &ph);
                rae_g2d_configure(pw, ph);
                g_g2d_win_resized = 1;   /* consumed once by gpu2d.windowResized() */
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) quit = 1;
                if (e.key.scancode < SDL_SCANCODE_COUNT) {
                    g_sdl_keydown[e.key.scancode] = 1;
                    if (!e.key.repeat) g_sdl_pressed[e.key.scancode] = 1;  /* edge */
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode < SDL_SCANCODE_COUNT) g_sdl_keydown[e.key.scancode] = 0;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button < 8) {
                    g_sdl_mouse[e.button.button] = 1;
                    g_sdl_mouse_pressed[e.button.button] = 1;
                }
                if (!g_sdl_mouse_captured) { SDL_CaptureMouse(true); g_sdl_mouse_captured = true; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (e.button.button < 8) {
                    g_sdl_mouse[e.button.button] = 0;
                    g_sdl_mouse_released[e.button.button] = 1;
                }
                bool any = false;
                for (int b = 0; b < 8; b++) if (g_sdl_mouse[b]) any = true;
                if (!any && g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                g_g2d_wheel += e.wheel.y;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                memset(g_sdl_keydown, 0, sizeof(g_sdl_keydown));
                memset(g_sdl_mouse, 0, sizeof(g_sdl_mouse));
                memset(g_sdl_mouse_pressed, 0, sizeof(g_sdl_mouse_pressed));
                memset(g_sdl_mouse_released, 0, sizeof(g_sdl_mouse_released));
                if (g_sdl_mouse_captured) { SDL_CaptureMouse(false); g_sdl_mouse_captured = false; }
                break;
            default: break;
        }
    }
    if (quit) return 1;
    if (g_sdl_headless_ms > 0 && (rae_ext_nowMs() - g_sdl_start_ms) >= g_sdl_headless_ms) return 1;
    return 0;
}

/* Block up to timeoutSec for the next OS event (or wake immediately if one is
 * already queued), leaving events in the queue for the following pollClose to
 * drain. Passing NULL means SDL doesn't dequeue the event. This is the idle
 * half of the hybrid loop: busy-render while animating, park here when idle so
 * the app sits at ~0% CPU until input arrives. timeoutSec <= 0 returns at once. */
void rae_ext_gpu2d_waitEvents(double timeoutSec) {
    int ms = (int)(timeoutSec * 1000.0);
    if (ms < 0) ms = 0;
    SDL_WaitEventTimeout(NULL, ms);
}

/* Pointer position in DESIGN units (the same coordinate space drawRect etc.
 * take), so hit-testing matches what was drawn. SDL reports logical window
 * points; we scale to physical px (× dpr) then invert the design fit transform
 * (subtract the letterbox offset, divide by scale). */
static void rae_g2d_pointer_design(double* dx, double* dy) {
    float mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
    int lw = 0, lh = 0; if (g_sdl_win) SDL_GetWindowSize(g_sdl_win, &lw, &lh);
    double sclx = (lw > 0) ? (double)g_sdl_w / (double)lw : 1.0;
    double scly = (lh > 0) ? (double)g_sdl_h / (double)lh : 1.0;
    float xf[8]; rae_g2d_compute_xform(xf);
    double physX = (double)mx * sclx, physY = (double)my * scly;
    *dx = (xf[2] != 0.0f) ? (physX - xf[4]) / xf[2] : physX;
    *dy = (xf[3] != 0.0f) ? (physY - xf[5]) / xf[3] : physY;
}
double rae_ext_gpu2d_pointerX(void) { double x, y; rae_g2d_pointer_design(&x, &y); return x; }
double rae_ext_gpu2d_pointerY(void) { double x, y; rae_g2d_pointer_design(&x, &y); return y; }
/* Left mouse button held this frame (button index 1 in SDL). */
rae_Bool rae_ext_gpu2d_pointerDown(void) { return g_sdl_mouse[SDL_BUTTON_LEFT] != 0; }
rae_Bool rae_ext_gpu2d_pointerPressed(void) { return g_sdl_mouse_pressed[SDL_BUTTON_LEFT] != 0; }
rae_Bool rae_ext_gpu2d_pointerReleased(void) { return g_sdl_mouse_released[SDL_BUTTON_LEFT] != 0; }
/* Per-frame wheel delta (positive = wheel/scroll up). */
double rae_ext_gpu2d_wheelMove(void) { return (double)g_g2d_wheel; }

void rae_ext_gpu2d_setMouseCursor(int64_t kind) {
    if (!g_sdl_win) return;
    if (kind < 0 || kind > 6) kind = 0;
    if ((int)kind == g_g2d_cursor_kind) return;

    SDL_SystemCursor cursor = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (kind) {
        case 1: cursor = SDL_SYSTEM_CURSOR_POINTER; break;
        case 2: cursor = SDL_SYSTEM_CURSOR_TEXT; break;
        case 3: cursor = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
        case 4: cursor = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
        case 5: cursor = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
        case 6: cursor = SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
        default: cursor = SDL_SYSTEM_CURSOR_DEFAULT; break;
    }

    if (!g_g2d_cursors[kind]) {
        g_g2d_cursors[kind] = SDL_CreateSystemCursor(cursor);
    }
    if (g_g2d_cursors[kind]) {
        SDL_SetCursor(g_g2d_cursors[kind]);
        g_g2d_cursor_kind = (int)kind;
    }
}
/* Monotonic wall-clock seconds since process start — for scroll timing without
 * pulling in the raylib-backed getTime. */
double rae_ext_gpu2d_nowSeconds(void) { return (double)rae_ext_nowMs() / 1000.0; }

int64_t rae_ext_gpu2d_windowWidth(void) { return g_sdl_w; }
int64_t rae_ext_gpu2d_windowHeight(void) { return g_sdl_h; }
void rae_ext_gpu2d_setWindowPosition(int64_t x, int64_t y) {
    if (g_sdl_win) SDL_SetWindowPosition(g_sdl_win, (int)x, (int)y);
}
int64_t rae_ext_gpu2d_windowPositionX(void) {
    int x = 0, y = 0;
    if (g_sdl_win) SDL_GetWindowPosition(g_sdl_win, &x, &y);
    (void)y;
    return (int64_t)x;
}
int64_t rae_ext_gpu2d_windowPositionY(void) {
    int x = 0, y = 0;
    if (g_sdl_win) SDL_GetWindowPosition(g_sdl_win, &x, &y);
    (void)x;
    return (int64_t)y;
}
/* True once per OS resize (edge-triggered): returns the pending flag and
 * clears it, so the app rebuilds its layout extent for the new window. */
rae_Bool rae_ext_gpu2d_windowResized(void) {
    rae_Bool r = (rae_Bool)g_g2d_win_resized;
    g_g2d_win_resized = 0;
    return r;
}

/* Coordinate system (#112). */
void rae_ext_gpu2d_setDesignResolution(double w, double h, int64_t fit) {
    g_g2d_design_w = w; g_g2d_design_h = h; g_g2d_fit_mode = (int)fit;
}
double rae_ext_gpu2d_designWidth(void)  { return (g_g2d_design_w > 0.0) ? g_g2d_design_w : (double)g_sdl_w; }
double rae_ext_gpu2d_designHeight(void) { return (g_g2d_design_h > 0.0) ? g_g2d_design_h : (double)g_sdl_h; }
double rae_ext_gpu2d_dpr(void) {
    int lw = 0, lh = 0; if (g_sdl_win) SDL_GetWindowSize(g_sdl_win, &lw, &lh);
    (void)lh; return (lw > 0) ? (double)g_sdl_w / (double)lw : 1.0;
}

/* --- Box uber-shader pipeline (#110) ----------------------------------
 * Instanced rounded-box SDF with analytic AA: one quad per primitive, the
 * fragment shader evaluates a rounded-box signed distance and antialiases
 * with screen-space derivatives (no MSAA). One pipeline → filled/rounded
 * rects, per-corner radius, borders. Primitives are accumulated CPU-side
 * each frame and drawn in one instanced draw at endFrame (painter's order).
 * Instance layout = 6×vec4 (std430): rect, radius, fill, border, params, grad. */
#define G2D_PRIM_FLOATS 24

static const char* G2D_BOX_WGSL =
"struct Prim {\n"
"  rect: vec4<f32>,\n"
"  radius: vec4<f32>,\n"
"  fill: vec4<f32>,\n"
"  border: vec4<f32>,\n"
"  params: vec4<f32>,\n"
"  grad: vec4<f32>,\n"
"};\n"
/* uXform[0] = (physW, physH, scaleX, scaleY); uXform[1] = (offsetX, offsetY,..)
 * maps design-unit coords -> physical px: px = design*scale + offset. */
"@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;\n"
"@group(0) @binding(1) var<storage, read> prims: array<Prim>;\n"
/* #118 rounded clip: uClip[0]=(x,y,w,h) design units, uClip[1]=(radius,enabled,..) */
"@group(0) @binding(2) var<uniform> uClip: array<vec4<f32>, 2>;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) local: vec2<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"  @location(2) posD: vec2<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {\n"
"  var corners = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),\n"
"    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));\n"
"  let c = corners[vi];\n"
"  let p = prims[ii];\n"
"  let phys = uXform[0].xy;\n"
"  let local = c * p.rect.zw;\n"            /* box-local, unrotated (0..w, 0..h) */
"  let center = p.rect.zw * 0.5;\n"
"  let a = p.params.y;\n"                   /* rotation (radians), 0 for rects */
"  let ca = cos(a); let sa = sin(a);\n"
"  let rel = local - center;\n"
"  let rot = vec2<f32>(rel.x * ca - rel.y * sa, rel.x * sa + rel.y * ca);\n"
"  let posDesign = p.rect.xy + center + rot;\n"
"  let posPx = posDesign * uXform[0].zw + uXform[1].xy;\n"
"  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,\n"
"                      1.0 - posPx.y / phys.y * 2.0);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(ndc, 0.0, 1.0);\n"
"  o.local = local;\n"                      /* SDF evaluates in unrotated box frame */
"  o.inst = ii;\n"
"  o.posD = posDesign;\n"                   /* design-space pos for the clip SDF */
"  return o;\n"
"}\n"
"fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: vec4<f32>) -> f32 {\n"
"  let rad = select(r.zw, r.xy, p.x > 0.0);\n"
"  let rr = select(rad.y, rad.x, p.y > 0.0);\n"
"  let q = abs(p) - b + vec2<f32>(rr, rr);\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - rr;\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
"  let p = prims[in.inst];\n"
"  let halfSize = p.rect.zw * 0.5;\n"
"  let center = in.local - halfSize;\n"
"  let d = sdRoundBox(center, halfSize, p.radius);\n"
"  let aa = max(fwidth(d), 0.0001);\n"
"  var cov = 1.0 - smoothstep(-aa, aa, d);\n"
"  if (uClip[1].y > 0.5) {\n"                /* #118: rounded clip coverage */
"    let cc = uClip[0].xy + uClip[0].zw * 0.5;\n"
"    let ch = uClip[0].zw * 0.5;\n"
"    let cr = uClip[1].x;\n"
"    let cd = sdRoundBox(in.posD - cc, ch, vec4<f32>(cr, cr, cr, cr));\n"
"    let caa = max(fwidth(cd), 0.0001);\n"
"    cov = cov * (1.0 - smoothstep(-caa, caa, cd));\n"
"  }\n"
"  var col = p.fill;\n"
"  if (p.params.z > 0.5) {\n"
"    let uv = in.local / max(p.rect.zw, vec2<f32>(1.0, 1.0));\n"
"    let dir = vec2<f32>(cos(p.params.w), sin(p.params.w));\n"
"    let centered = uv - vec2<f32>(0.5, 0.5);\n"
"    let extent = max(abs(dir.x) * 0.5 + abs(dir.y) * 0.5, 0.0001);\n"
"    let t = clamp(dot(centered, dir) / (extent * 2.0) + 0.5, 0.0, 1.0);\n"
"    col = mix(p.fill, p.grad, t);\n"
"  }\n"
"  let bw = p.params.x;\n"
"  if (bw > 0.0) {\n"
"    let inner = 1.0 - smoothstep(-aa, aa, d + bw);\n"
"    col = mix(p.border, p.fill, inner);\n"
"  }\n"
"  return col * cov;\n"
"}\n";

static WGPURenderPipeline g_g2d_pipeline = NULL;
static WGPUBuffer    g_g2d_uniform = NULL;
static WGPUBuffer    g_g2d_instbuf = NULL;
static WGPUBindGroup g_g2d_bind = NULL;
static int   g_g2d_inst_cap = 0;       /* capacity in primitives */
static float* g_g2d_prims = NULL;      /* CPU accumulation (floats) */
static int   g_g2d_prim_count = 0;
static int   g_g2d_prim_capf = 0;      /* capacity in floats */

static void g2d_color(uint32_t c, float* out) {
    /* 0xAARRGGBB -> premultiplied RGBA in 0..1 */
    float a = (float)((c >> 24) & 0xFF) / 255.0f;
    float r = (float)((c >> 16) & 0xFF) / 255.0f;
    float g = (float)((c >> 8)  & 0xFF) / 255.0f;
    float b = (float)( c        & 0xFF) / 255.0f;
    out[0] = r * a; out[1] = g * a; out[2] = b * a; out[3] = a;
}

static void rae_g2d_push(double x, double y, double w, double h,
                         double rtl, double rtr, double rbr, double rbl,
                         uint32_t fill, uint32_t border, double bw, double angle) {
    int need = (g_g2d_prim_count + 1) * G2D_PRIM_FLOATS;
    if (need > g_g2d_prim_capf) {
        int cap = g_g2d_prim_capf ? g_g2d_prim_capf : (64 * G2D_PRIM_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_prims = (float*)realloc(g_g2d_prims, (size_t)cap * sizeof(float));
        g_g2d_prim_capf = cap;
    }
    float* p = g_g2d_prims + g_g2d_prim_count * G2D_PRIM_FLOATS;
    p[0]=(float)x; p[1]=(float)y; p[2]=(float)w; p[3]=(float)h;
    p[4]=(float)rtl; p[5]=(float)rtr; p[6]=(float)rbr; p[7]=(float)rbl;
    g2d_color(fill, &p[8]);
    g2d_color(border, &p[12]);
    p[16]=(float)bw; p[17]=(float)angle; p[18]=0.0f; p[19]=0.0f;
    g2d_color(fill, &p[20]);
    rae_g2d_prim_clip_ensure(g_g2d_prim_count + 1);
    g_g2d_prim_clip[g_g2d_prim_count] = g_g2d_cur_clip;
    g_g2d_prim_count++;
}

static void rae_g2d_push_gradient(double x, double y, double w, double h,
                                  double radius, uint32_t from, uint32_t to,
                                  double angle_deg) {
    int need = (g_g2d_prim_count + 1) * G2D_PRIM_FLOATS;
    if (need > g_g2d_prim_capf) {
        int cap = g_g2d_prim_capf ? g_g2d_prim_capf : (64 * G2D_PRIM_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_prims = (float*)realloc(g_g2d_prims, (size_t)cap * sizeof(float));
        g_g2d_prim_capf = cap;
    }
    float* p = g_g2d_prims + g_g2d_prim_count * G2D_PRIM_FLOATS;
    p[0]=(float)x; p[1]=(float)y; p[2]=(float)w; p[3]=(float)h;
    p[4]=(float)radius; p[5]=(float)radius; p[6]=(float)radius; p[7]=(float)radius;
    g2d_color(from, &p[8]);
    g2d_color(0, &p[12]);
    p[16]=0.0f;
    p[17]=0.0f;
    p[18]=1.0f;
    p[19]=(float)(angle_deg * 0.017453292519943295);
    g2d_color(to, &p[20]);
    rae_g2d_prim_clip_ensure(g_g2d_prim_count + 1);
    g_g2d_prim_clip[g_g2d_prim_count] = g_g2d_cur_clip;
    g_g2d_prim_count++;
}

static void rae_g2d_init_pipeline(void) {
    if (g_g2d_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G2D_BOX_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUBlendState blend; memset(&blend, 0, sizeof(blend));
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;            /* premultiplied alpha */
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.blend = &blend; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;  /* auto layout from shader bindings */
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_g2d_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);

    WGPUBufferDescriptor ud; memset(&ud, 0, sizeof(ud));
    ud.size = 32; ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;  /* 2*vec4 xform */
    g_g2d_uniform = wgpuDeviceCreateBuffer(g_wgpu_dev, &ud);
}

static void rae_g2d_rebuild_bind(void) {
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_pipeline, 0);
    WGPUBindGroupEntry e[2]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
    e[1].binding = 1; e[1].buffer = g_g2d_instbuf;
    e[1].size = (uint64_t)g_g2d_inst_cap * G2D_PRIM_FLOATS * sizeof(float);
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 2; bgd.entries = e;
    g_g2d_bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void rae_g2d_ensure_inst(int prims) {
    if (g_g2d_instbuf && prims <= g_g2d_inst_cap) return;
    int cap = g_g2d_inst_cap ? g_g2d_inst_cap : 64;
    while (cap < prims) cap *= 2;
    if (g_g2d_instbuf) wgpuBufferRelease(g_g2d_instbuf);
    if (g_g2d_bind) { wgpuBindGroupRelease(g_g2d_bind); g_g2d_bind = NULL; }
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = (uint64_t)cap * G2D_PRIM_FLOATS * sizeof(float);
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g_g2d_instbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    g_g2d_inst_cap = cap;
}

void rae_ext_gpu2d_drawRect(double x, double y, double w, double h, int64_t color) {
    rae_g2d_push(x, y, w, h, 0, 0, 0, 0, (uint32_t)color, 0, 0.0, 0.0);
}
void rae_ext_gpu2d_drawRoundedRect(double x, double y, double w, double h, double radius, int64_t color) {
    rae_g2d_push(x, y, w, h, radius, radius, radius, radius, (uint32_t)color, 0, 0.0, 0.0);
}
void rae_ext_gpu2d_drawBox(double x, double y, double w, double h, double radius,
                           int64_t fill, double borderWidth, int64_t border) {
    rae_g2d_push(x, y, w, h, radius, radius, radius, radius,
                 (uint32_t)fill, (uint32_t)border, borderWidth, 0.0);
}
void rae_ext_gpu2d_drawGradientRect(double x, double y, double w, double h,
                                    double radius, int64_t from, int64_t to,
                                    double angleDeg) {
    rae_g2d_push_gradient(x, y, w, h, radius, (uint32_t)from, (uint32_t)to, angleDeg);
}
/* A line from (x0,y0) to (x1,y1), `thickness` px wide, with rounded caps —
 * a rotated capsule (rounded rect of length x thickness, radius thickness/2). */
void rae_ext_gpu2d_drawLine(double x0, double y0, double x1, double y1,
                            double thickness, int64_t color) {
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    if (len < 1e-6 || thickness <= 0.0) return;
    double angle = atan2(dy, dx);
    double cx = (x0 + x1) * 0.5, cy = (y0 + y1) * 0.5;
    double r = thickness * 0.5;
    if (r > len * 0.5) r = len * 0.5;
    rae_g2d_push(cx - len * 0.5, cy - thickness * 0.5, len, thickness,
                 r, r, r, r, (uint32_t)color, 0, 0.0, angle);
}

/* --- Text pipeline (#111): MSDF glyph quads --------------------------
 * A second instanced pipeline that samples the MSDF atlas (the same raw
 * RGBA the CPU blit path holds in g_sdf_atlas[]) and antialiases with the
 * median-of-3 + screen-px-range trick. Shares the viewport uniform and
 * premultiplied blend with the box pipeline, so glyphs composite in the
 * same pass after boxes. Instance = 4*vec4: rect, uv (normalised), colour,
 * params(pxRange). One atlas/font per frame (the common UI case). */
#define G2D_TEXT_FLOATS 20

static const char* G2D_TEXT_WGSL =
"struct Glyph {\n"
"  rect: vec4<f32>,\n"
"  uv: vec4<f32>,\n"
"  color: vec4<f32>,\n"
"  params: vec4<f32>,\n"          /* x=pxRange, y=outlineWidth(px), z=softness(px) */
"  outline: vec4<f32>,\n"         /* straight outline colour */
"};\n"
"@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;\n"
"@group(0) @binding(1) var<storage, read> glyphs: array<Glyph>;\n"
"@group(0) @binding(2) var atlasTex: texture_2d<f32>;\n"
"@group(0) @binding(3) var atlasSamp: sampler;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) uv: vec2<f32>,\n"
"  @location(1) @interpolate(flat) inst: u32,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VsOut {\n"
"  var corners = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),\n"
"    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));\n"
"  let c = corners[vi];\n"
"  let g = glyphs[ii];\n"
"  let phys = uXform[0].xy;\n"
"  let posPx = (g.rect.xy + c * g.rect.zw) * uXform[0].zw + uXform[1].xy;\n"
"  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0,\n"
"                      1.0 - posPx.y / phys.y * 2.0);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(ndc, 0.0, 1.0);\n"
"  o.uv = g.uv.xy + c * g.uv.zw;\n"
"  o.inst = ii;\n"
"  return o;\n"
"}\n"
"fn median3(r: f32, g: f32, b: f32) -> f32 {\n"
"  return max(min(r, g), min(max(r, g), b));\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
"  let g = glyphs[in.inst];\n"
"  let s = textureSample(atlasTex, atlasSamp, in.uv);\n"
/* signed distance from the glyph edge, in physical px (design pxRange * avg
 * scale). Positive inside the glyph. */
"  let sc = (uXform[0].z + uXform[0].w) * 0.5;\n"
"  let sd = g.params.x * sc * (median3(s.r, s.g, s.b) - 0.5);\n"
/* softness widens the coverage falloff (px) — 1 = crisp AA, larger = a soft
 * blurred edge (used for soft drop-shadows). */
"  let sw = max(g.params.z, 1.0);\n"
"  let bodyCov = clamp(sd / sw + 0.5, 0.0, 1.0);\n"
"  let ow = g.params.y;\n"
"  if (ow <= 0.0) {\n"
"    let a = g.color.a * bodyCov;\n"
"    return vec4<f32>(g.color.rgb * a, a);\n"   /* premultiplied */
"  }\n"
/* Outline = the glyph dilated by `ow` px; composite body OVER outline,
 * both premultiplied. */
"  let outerCov = clamp((sd + ow) / sw + 0.5, 0.0, 1.0);\n"
"  let ba = g.color.a * bodyCov;\n"
"  let oa = g.outline.a * outerCov;\n"
"  let outA = ba + oa * (1.0 - ba);\n"
"  let outRGB = g.color.rgb * ba + g.outline.rgb * oa * (1.0 - ba);\n"
"  return vec4<f32>(outRGB, outA);\n"           /* premultiplied */
"}\n";

static WGPURenderPipeline g_g2d_text_pipeline = NULL;
/* Text glyphs accumulate PER ATLAS so a single frame can mix multiple MSDF
 * fonts (e.g. Roboto body text + the Material-icon atlas) — endFrame emits one
 * text draw per atlas that has glyphs this frame. Indexed by atlas handle-1. */
static WGPUBuffer    g_g2d_text_instbuf[RAE_SDF_MAX_ATLAS];
static WGPUBindGroup g_g2d_text_bind[RAE_SDF_MAX_ATLAS];
static int   g_g2d_text_cap[RAE_SDF_MAX_ATLAS];   /* glyph capacity of instbuf[i] */
static float* g_g2d_text_prims[RAE_SDF_MAX_ATLAS]; /* CPU float accumulation */
static int   g_g2d_text_count[RAE_SDF_MAX_ATLAS];  /* glyphs this frame */
static int   g_g2d_text_capf[RAE_SDF_MAX_ATLAS];   /* float capacity */
static WGPUSampler g_g2d_sampler = NULL;
static WGPUTexture     g_g2d_atlas_tex[RAE_SDF_MAX_ATLAS];
static WGPUTextureView g_g2d_atlas_view[RAE_SDF_MAX_ATLAS];

/* Lazily upload atlas `handle` (1-based, from sdf_text.loadAtlas) as a wgpu
 * texture; cached. Returns its view, or NULL if the atlas isn't loaded. */
static WGPUTextureView rae_g2d_atlas_texview(int handle) {
    int i = handle - 1;
    if (i < 0 || i >= RAE_SDF_MAX_ATLAS || !g_sdf_atlas[i]) return NULL;
    if (g_g2d_atlas_view[i]) return g_g2d_atlas_view[i];
    int aw = g_sdf_atlas_w[i], ah = g_sdf_atlas_h[i];
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = (uint32_t)aw; td.size.height = (uint32_t)ah; td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm;   /* MSDF data is linear, not sRGB */
    td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    WGPUTexelCopyTextureInfo dst; memset(&dst, 0, sizeof(dst));
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout; memset(&layout, 0, sizeof(layout));
    layout.bytesPerRow = (uint32_t)(aw * 4); layout.rowsPerImage = (uint32_t)ah;
    WGPUExtent3D ext; ext.width = (uint32_t)aw; ext.height = (uint32_t)ah; ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(g_wgpu_queue, &dst, g_sdf_atlas[i], (size_t)aw * ah * 4, &layout, &ext);
    g_g2d_atlas_tex[i] = tex;
    g_g2d_atlas_view[i] = wgpuTextureCreateView(tex, NULL);
    return g_g2d_atlas_view[i];
}

static void rae_g2d_init_text_pipeline(void) {
    if (g_g2d_text_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G2D_TEXT_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);

    WGPUBlendState blend; memset(&blend, 0, sizeof(blend));
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.blend = &blend; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;

    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_g2d_text_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);

    if (!g_g2d_sampler) {
        WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = WGPUFilterMode_Linear;   /* MSDF requires bilinear */
        sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMaxClamp = 1.0f; sd.maxAnisotropy = 1;
        g_g2d_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    }
}

static void rae_g2d_rebuild_text_bind(int ai) {
    WGPUTextureView view = rae_g2d_atlas_texview(ai + 1);
    if (!view) return;
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_text_pipeline, 0);
    WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
    e[1].binding = 1; e[1].buffer = g_g2d_text_instbuf[ai];
    e[1].size = (uint64_t)g_g2d_text_cap[ai] * G2D_TEXT_FLOATS * sizeof(float);
    e[2].binding = 2; e[2].textureView = view;
    e[3].binding = 3; e[3].sampler = g_g2d_sampler;
    WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
    bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
    if (g_g2d_text_bind[ai]) wgpuBindGroupRelease(g_g2d_text_bind[ai]);
    g_g2d_text_bind[ai] = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
    wgpuBindGroupLayoutRelease(bgl);
}

static void rae_g2d_ensure_text_inst(int ai, int prims) {
    if (g_g2d_text_instbuf[ai] && prims <= g_g2d_text_cap[ai]) return;
    int cap = g_g2d_text_cap[ai] ? g_g2d_text_cap[ai] : 256;
    while (cap < prims) cap *= 2;
    if (g_g2d_text_instbuf[ai]) wgpuBufferRelease(g_g2d_text_instbuf[ai]);
    if (g_g2d_text_bind[ai]) { wgpuBindGroupRelease(g_g2d_text_bind[ai]); g_g2d_text_bind[ai] = NULL; }
    WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
    bd.size = (uint64_t)cap * G2D_TEXT_FLOATS * sizeof(float);
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    g_g2d_text_instbuf[ai] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
    g_g2d_text_cap[ai] = cap;
}

/* Full glyph submit with an optional outline (outlineWidth px + colour) and
 * softness (edge-falloff width in px; 1 = crisp, larger = soft/blurred). */
void rae_ext_gpu2d_drawGlyphEx(double sx0, double sy0, double sx1, double sy1,
                               double u0, double v0, double u1, double v1,
                               int64_t atlas, double pxRange, int64_t color,
                               double outlineWidth, int64_t outlineColor, double softness) {
    int ai = (int)atlas - 1;
    if (ai < 0 || ai >= RAE_SDF_MAX_ATLAS) return;
    int need = (g_g2d_text_count[ai] + 1) * G2D_TEXT_FLOATS;
    if (need > g_g2d_text_capf[ai]) {
        int cap = g_g2d_text_capf[ai] ? g_g2d_text_capf[ai] : (256 * G2D_TEXT_FLOATS);
        while (cap < need) cap *= 2;
        g_g2d_text_prims[ai] = (float*)realloc(g_g2d_text_prims[ai], (size_t)cap * sizeof(float));
        g_g2d_text_capf[ai] = cap;
    }
    float* p = g_g2d_text_prims[ai] + g_g2d_text_count[ai] * G2D_TEXT_FLOATS;
    p[0]=(float)sx0; p[1]=(float)sy0; p[2]=(float)(sx1-sx0); p[3]=(float)(sy1-sy0);
    p[4]=(float)u0; p[5]=(float)v0; p[6]=(float)(u1-u0); p[7]=(float)(v1-v0);
    /* straight (non-premultiplied) colour 0xAARRGGBB; the shader premultiplies */
    uint32_t c = (uint32_t)color;
    p[8]  = (float)((c >> 16) & 0xFF) / 255.0f;
    p[9]  = (float)((c >> 8)  & 0xFF) / 255.0f;
    p[10] = (float)( c        & 0xFF) / 255.0f;
    p[11] = (float)((c >> 24) & 0xFF) / 255.0f;
    p[12]=(float)pxRange; p[13]=(float)outlineWidth; p[14]=(float)softness; p[15]=0.0f;
    uint32_t oc = (uint32_t)outlineColor;
    p[16] = (float)((oc >> 16) & 0xFF) / 255.0f;
    p[17] = (float)((oc >> 8)  & 0xFF) / 255.0f;
    p[18] = (float)( oc        & 0xFF) / 255.0f;
    p[19] = (float)((oc >> 24) & 0xFF) / 255.0f;
    rae_g2d_text_clip_ensure(ai, g_g2d_text_count[ai] + 1);
    g_g2d_text_clip[ai][g_g2d_text_count[ai]] = g_g2d_cur_clip;
    g_g2d_text_count[ai]++;
}

/* Back-compat: glyph with no outline. */
void rae_ext_gpu2d_drawGlyph(double sx0, double sy0, double sx1, double sy1,
                             double u0, double v0, double u1, double v1,
                             int64_t atlas, double pxRange, int64_t color) {
    rae_ext_gpu2d_drawGlyphEx(sx0, sy0, sx1, sy1, u0, v0, u1, v1, atlas, pxRange, color, 0.0, 0, 1.0);
}

/* --- Image pipeline (#143): textured rounded quads -------------------
 * A third pipeline that samples a per-image RGBA texture with a tint
 * multiply and the same rounded-rect SDF mask the box pipeline uses, so
 * album covers and (white-on-alpha) Material-style icons render on the
 * GPU. Unlike box/text (one instanced draw), each image samples its OWN
 * texture, so images draw one-per-call with a per-draw uniform + bind
 * group. The image count per frame is tiny (a cover + a few icons), so
 * the per-draw bind group is cheap. Drawn after boxes, before text. */
static const char* G2D_IMG_WGSL =
"@group(0) @binding(0) var<uniform> uXform: array<vec4<f32>, 2>;\n"
"@group(0) @binding(1) var<uniform> uImg: array<vec4<f32>, 4>;\n"  /* rect, tint, params(radius), uv */
"@group(0) @binding(2) var tex: texture_2d<f32>;\n"
"@group(0) @binding(3) var samp: sampler;\n"
"struct VsOut {\n"
"  @builtin(position) pos: vec4<f32>,\n"
"  @location(0) uv: vec2<f32>,\n"
"  @location(1) local: vec2<f32>,\n"
"};\n"
"@vertex\n"
"fn vs(@builtin(vertex_index) vi: u32) -> VsOut {\n"
"  var corners = array<vec2<f32>, 6>(\n"
"    vec2<f32>(0.0,0.0), vec2<f32>(1.0,0.0), vec2<f32>(0.0,1.0),\n"
"    vec2<f32>(0.0,1.0), vec2<f32>(1.0,0.0), vec2<f32>(1.0,1.0));\n"
"  let c = corners[vi];\n"
"  let rect = uImg[0];\n"
"  let phys = uXform[0].xy;\n"
"  let posPx = (rect.xy + c * rect.zw) * uXform[0].zw + uXform[1].xy;\n"
"  let ndc = vec2<f32>(posPx.x / phys.x * 2.0 - 1.0, 1.0 - posPx.y / phys.y * 2.0);\n"
"  var o: VsOut;\n"
"  o.pos = vec4<f32>(ndc, 0.0, 1.0);\n"
"  let uv = uImg[3];\n"
"  o.uv = uv.xy + c * uv.zw;\n"
"  o.local = c * rect.zw;\n"
"  return o;\n"
"}\n"
"fn sdRoundBox(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {\n"
"  let q = abs(p) - b + vec2<f32>(r, r);\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0, 0.0))) - r;\n"
"}\n"
"@fragment\n"
"fn fs(in: VsOut) -> @location(0) vec4<f32> {\n"
"  let texel = textureSample(tex, samp, in.uv);\n"
"  let tint = uImg[1];\n"
"  let half = uImg[0].zw * 0.5;\n"
"  let rad = uImg[2].x;\n"
"  let d = sdRoundBox(in.local - half, half, rad);\n"
"  let aa = max(fwidth(d), 0.0001);\n"
"  let cov = 1.0 - smoothstep(-aa, aa, d);\n"
"  let a = texel.a * tint.a * cov;\n"
"  return vec4<f32>(texel.rgb * tint.rgb * a, a);\n"  /* premultiplied */
"}\n";

#define RAE_G2D_MAX_IMG 128
static WGPUTexture     g_g2d_img_tex[RAE_G2D_MAX_IMG];
static WGPUTextureView g_g2d_img_view[RAE_G2D_MAX_IMG];
static int g_g2d_img_w[RAE_G2D_MAX_IMG];
static int g_g2d_img_h[RAE_G2D_MAX_IMG];
static int g_g2d_img_n = 0;

typedef struct { int handle; float rect[4]; float tint[4]; float radius; float uv[4]; int clip; } RaeG2dImgCmd;
static RaeG2dImgCmd* g_g2d_img_cmds = NULL;
static int g_g2d_img_cmd_count = 0;
static int g_g2d_img_cmd_cap = 0;

static WGPURenderPipeline g_g2d_img_pipeline = NULL;
static WGPUBuffer* g_g2d_img_ubuf = NULL;   /* pool of 48-byte per-draw uniforms */
static int g_g2d_img_ubuf_n = 0;
static WGPUBindGroup* g_g2d_img_frame_binds = NULL;  /* transient, released after submit */
static int g_g2d_img_frame_bind_n = 0;
static int g_g2d_img_frame_bind_cap = 0;
static WGPUBuffer* g_g2d_frame_bufs = NULL;          /* transient per-flush buffers */
static int g_g2d_frame_buf_n = 0;
static int g_g2d_frame_buf_cap = 0;
static WGPUBindGroup* g_g2d_frame_binds = NULL;      /* transient per-flush bind groups */
static int g_g2d_frame_bind_n = 0;
static int g_g2d_frame_bind_cap = 0;
void rae_ext_gpu2d_flush(void);

static void rae_g2d_keep_frame_buf(WGPUBuffer b) {
    if (!b) return;
    if (g_g2d_frame_buf_n + 1 > g_g2d_frame_buf_cap) {
        int cap = g_g2d_frame_buf_cap ? g_g2d_frame_buf_cap * 2 : 64;
        g_g2d_frame_bufs = (WGPUBuffer*)realloc(g_g2d_frame_bufs, (size_t)cap * sizeof(WGPUBuffer));
        g_g2d_frame_buf_cap = cap;
    }
    g_g2d_frame_bufs[g_g2d_frame_buf_n++] = b;
}

static void rae_g2d_keep_frame_bind(WGPUBindGroup b) {
    if (!b) return;
    if (g_g2d_frame_bind_n + 1 > g_g2d_frame_bind_cap) {
        int cap = g_g2d_frame_bind_cap ? g_g2d_frame_bind_cap * 2 : 64;
        g_g2d_frame_binds = (WGPUBindGroup*)realloc(g_g2d_frame_binds, (size_t)cap * sizeof(WGPUBindGroup));
        g_g2d_frame_bind_cap = cap;
    }
    g_g2d_frame_binds[g_g2d_frame_bind_n++] = b;
}

/* Read a whole file into a malloc'd buffer. Returns NULL on failure. */
static unsigned char* rae_g2d_read_whole_file(const char* path, size_t* out_len) {
    if (!path || !out_len) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* Decode an image file to RGBA8 (#228; contract + rationale in
 * docs/image-decoding-design.md): strict magic-byte dispatch, ONE
 * decoder per format, no fallback cascade.
 *   FF D8 FF     -> vendored stb_image (JPEG, every platform)
 *   89 50 4E 47  -> lodepng (PNG)
 *   anything else -> unsupported
 * Output is straight-alpha RGBA8, no colour management.
 *
 * stb is the sole JPEG decoder. macOS previously used ImageIO, but
 * ImageIO silently rendered truncated downloads half-grey (no error);
 * stb correctly rejects them, and decodes every valid file (verified
 * ok=102/102 on the cached Spotify artwork set). The truncation guard
 * below turns a partial download into a loud failure the caller can
 * evict + re-fetch, rather than a decoder-dependent glitch.
 * Returns 1 with a malloc-compatible *out_rgba on success; on
 * failure returns 0 and points *out_err at a static reason string. */
static int rae_g2d_decode_rgba(const char* path, unsigned char** out_rgba,
                               unsigned* out_w, unsigned* out_h,
                               const char** out_err) {
    *out_rgba = NULL; *out_w = 0; *out_h = 0; *out_err = "unreadable file";
    size_t len = 0;
    unsigned char* bytes = rae_g2d_read_whole_file(path, &len);
    if (!bytes) return 0;
    if (len >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        /* Truncation guard: a JPEG without its EOI marker (FF D9) in
         * the last 64 bytes is an interrupted download. Fail loudly
         * so callers can evict the bad cache entry and re-fetch. */
        {
            size_t scan = len < 64 ? len : 64;
            int has_eoi = 0;
            for (size_t i = len - scan; i + 1 < len; i++) {
                if (bytes[i] == 0xFF && bytes[i + 1] == 0xD9) { has_eoi = 1; break; }
            }
            if (!has_eoi) {
                free(bytes);
                *out_err = "truncated JPEG (missing EOI marker)";
                return 0;
            }
        }
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(bytes, (int)len, &w, &h, &comp, 4);
        free(bytes);
        if (!px) {
            *out_err = stbi_failure_reason();
            return 0;
        }
        *out_rgba = px; *out_w = (unsigned)w; *out_h = (unsigned)h;
        return 1;
    }
    if (len >= 4 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
        unsigned err = lodepng_decode32(out_rgba, out_w, out_h, bytes, len);
        free(bytes);
        if (err) {
            *out_err = lodepng_error_text(err);
            return 0;
        }
        return 1;
    }
    free(bytes);
    *out_err = "unsupported format (not JPEG/PNG)";
    return 0;
}

/* Device-free decode probe (#228): run the exact decode + error
 * policy of gpu2d.loadImage without needing a WebGPU device, so the
 * corrupt-file behaviour is testable in the headless suite. Returns
 * 1 when the file decodes, 0 (plus the standard stderr line) when it
 * doesn't. Also handy as a CLI-side asset validator. */
int64_t rae_ext_gpu2d_decodeImageProbe(rae_String path) {
    if (!path.data) return 0;
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    const char* why = "decode failed";
    const char* cpath = (const char*)path.data;
    if (!rae_g2d_decode_rgba(cpath, &rgba, &uw, &uh, &why)) {
        fprintf(stderr, "[gpu2d] image decode failed (%s): %s\n", cpath, why);
        return 0;
    }
    free(rgba);
    return 1;
}

/* Decode an image file and upload it as an RGBA8 texture. Decode policy
 * lives in rae_g2d_decode_rgba; a failure logs one line and returns
 * handle 0, which callers already render as their placeholder. */
int64_t rae_ext_gpu2d_loadImage(rae_String path) {
    if (!path.data || !g_wgpu_dev || g_g2d_img_n >= RAE_G2D_MAX_IMG) return 0;
    unsigned char* rgba = NULL; unsigned uw = 0, uh = 0;
    const char* cpath = (const char*)path.data;
    const char* why = "decode failed";
    if (!rae_g2d_decode_rgba(cpath, &rgba, &uw, &uh, &why)) {
        fprintf(stderr, "[gpu2d] image decode failed (%s): %s\n", cpath, why);
        return 0;
    }
    WGPUTextureDescriptor td; memset(&td, 0, sizeof(td));
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size.width = uw; td.size.height = uh; td.size.depthOrArrayLayers = 1;
    td.format = WGPUTextureFormat_RGBA8Unorm; td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(g_wgpu_dev, &td);
    WGPUTexelCopyTextureInfo dst; memset(&dst, 0, sizeof(dst));
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout; memset(&layout, 0, sizeof(layout));
    layout.bytesPerRow = uw * 4; layout.rowsPerImage = uh;
    WGPUExtent3D ext; ext.width = uw; ext.height = uh; ext.depthOrArrayLayers = 1;
    wgpuQueueWriteTexture(g_wgpu_queue, &dst, rgba, (size_t)uw * uh * 4, &layout, &ext);
    free(rgba);
    int i = g_g2d_img_n;
    g_g2d_img_tex[i] = tex;
    g_g2d_img_view[i] = wgpuTextureCreateView(tex, NULL);
    g_g2d_img_w[i] = (int)uw; g_g2d_img_h[i] = (int)uh;
    return (int64_t)(++g_g2d_img_n);   /* 1-based */
}

/* Name->handle registry, so a renderer can resolve a Sprite.textureKey to an
 * uploaded image without a Rae-side map (module-level heap globals miscompile).
 * The gpu2d UI backend loads album covers / icons by key and draws by key. */
#define RAE_G2D_MAX_IMG_KEYS 128
static char g_g2d_img_key[RAE_G2D_MAX_IMG_KEYS][96];
static int  g_g2d_img_key_handle[RAE_G2D_MAX_IMG_KEYS];
static int  g_g2d_img_key_n = 0;

void rae_ext_gpu2d_drawImage(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint);  /* defined below */

static int rae_g2d_handle_for_key(const char* k) {
    if (!k) return 0;
    for (int i = 0; i < g_g2d_img_key_n; i++)
        if (strcmp(g_g2d_img_key[i], k) == 0) return g_g2d_img_key_handle[i];
    return 0;
}

/* Decode+upload `path` and register it under `key` (returns the handle, 0 on
 * failure). Re-registering a key updates it. */
int64_t rae_ext_gpu2d_loadImageKey(rae_String key, rae_String path) {
    int64_t h = rae_ext_gpu2d_loadImage(path);
    if (h <= 0 || !key.data) return h;
    int slot = -1;
    for (int i = 0; i < g_g2d_img_key_n; i++)
        if (strcmp(g_g2d_img_key[i], (const char*)key.data) == 0) { slot = i; break; }
    if (slot < 0 && g_g2d_img_key_n < RAE_G2D_MAX_IMG_KEYS) slot = g_g2d_img_key_n++;
    if (slot >= 0) {
        strncpy(g_g2d_img_key[slot], (const char*)key.data, 95);
        g_g2d_img_key[slot][95] = '\0';
        g_g2d_img_key_handle[slot] = (int)h;
    }
    return h;
}

/* True if `key` resolves to a loaded image (so a renderer can fall back to a
 * placeholder / mat: glyph when it doesn't). */
rae_Bool rae_ext_gpu2d_hasImageKey(rae_String key) {
    return key.data && rae_g2d_handle_for_key((const char*)key.data) > 0;
}

/* Draw a registered image by key (no-op if the key isn't registered). */
void rae_ext_gpu2d_drawImageKey(rae_String key, double x, double y, double w, double h, double radius, int64_t tint) {
    if (!key.data) return;
    int handle = rae_g2d_handle_for_key((const char*)key.data);
    if (handle > 0) rae_ext_gpu2d_drawImage(x, y, w, h, radius, (int64_t)handle, tint);
}

static void rae_g2d_queue_image(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint,
                                float u0, float v0, float u1, float v1) {
    if (handle < 1 || handle > g_g2d_img_n) return;
    if (g_g2d_img_cmd_count + 1 > g_g2d_img_cmd_cap) {
        int cap = g_g2d_img_cmd_cap ? g_g2d_img_cmd_cap * 2 : 32;
        g_g2d_img_cmds = (RaeG2dImgCmd*)realloc(g_g2d_img_cmds, (size_t)cap * sizeof(RaeG2dImgCmd));
        g_g2d_img_cmd_cap = cap;
    }
    RaeG2dImgCmd* c = &g_g2d_img_cmds[g_g2d_img_cmd_count++];
    c->handle = (int)handle;
    c->rect[0] = (float)x; c->rect[1] = (float)y; c->rect[2] = (float)w; c->rect[3] = (float)h;
    uint32_t t = (uint32_t)tint;   /* straight (non-premultiplied); shader premultiplies */
    c->tint[0] = (float)((t >> 16) & 0xFF) / 255.0f;
    c->tint[1] = (float)((t >> 8)  & 0xFF) / 255.0f;
    c->tint[2] = (float)( t        & 0xFF) / 255.0f;
    c->tint[3] = (float)((t >> 24) & 0xFF) / 255.0f;
    c->radius = (float)radius;
    c->uv[0] = u0; c->uv[1] = v0; c->uv[2] = u1 - u0; c->uv[3] = v1 - v0;
    c->clip = g_g2d_cur_clip;
}

/* Queue an image draw for this frame (handle from loadImage). tint is
 * 0xAARRGGBB applied multiplicatively (use 0xFFFFFFFF for the unmodified
 * image). radius rounds the corners (design units). */
void rae_ext_gpu2d_drawImage(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint) {
    rae_g2d_queue_image(x, y, w, h, radius, handle, tint, 0.0f, 0.0f, 1.0f, 1.0f);
}

void rae_ext_gpu2d_drawImageKeyScaled(rae_String key, double x, double y, double w, double h, double radius, int64_t tint, int64_t scaleMode) {
    if (!key.data) return;
    int handle = rae_g2d_handle_for_key((const char*)key.data);
    if (handle <= 0 || handle > g_g2d_img_n || w <= 0.0 || h <= 0.0) return;
    int idx = handle - 1;
    double iw = (double)g_g2d_img_w[idx];
    double ih = (double)g_g2d_img_h[idx];
    if (iw <= 0.0 || ih <= 0.0) {
        rae_ext_gpu2d_drawImage(x, y, w, h, radius, (int64_t)handle, tint);
        return;
    }
    double img_aspect = iw / ih;
    double dst_aspect = w / h;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (scaleMode == 0) { /* fit: preserve aspect by letterboxing inside dst */
        if (img_aspect > dst_aspect) {
            double draw_h = w / img_aspect;
            y += (h - draw_h) * 0.5;
            h = draw_h;
        } else {
            double draw_w = h * img_aspect;
            x += (w - draw_w) * 0.5;
            w = draw_w;
        }
    } else if (scaleMode == 1) { /* fill: preserve aspect by center-cropping source */
        if (img_aspect > dst_aspect) {
            double visible_w = dst_aspect / img_aspect;
            u0 = (float)((1.0 - visible_w) * 0.5);
            u1 = (float)(u0 + visible_w);
        } else if (img_aspect < dst_aspect) {
            double visible_h = img_aspect / dst_aspect;
            v0 = (float)((1.0 - visible_h) * 0.5);
            v1 = (float)(v0 + visible_h);
        }
    }
    rae_g2d_queue_image(x, y, w, h, radius, (int64_t)handle, tint, u0, v0, u1, v1);
}

static void rae_g2d_init_img_pipeline(void) {
    if (g_g2d_img_pipeline) return;
    WGPUShaderSourceWGSL src; memset(&src, 0, sizeof(src));
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = rae_wgpu_sv(G2D_IMG_WGSL);
    WGPUShaderModuleDescriptor smd; memset(&smd, 0, sizeof(smd));
    smd.nextInChain = &src.chain;
    WGPUShaderModule mod = wgpuDeviceCreateShaderModule(g_wgpu_dev, &smd);
    WGPUBlendState blend; memset(&blend, 0, sizeof(blend));
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    WGPUColorTargetState cts; memset(&cts, 0, sizeof(cts));
    cts.format = g_g2d_fmt; cts.blend = &blend; cts.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs; memset(&fs, 0, sizeof(fs));
    fs.module = mod; fs.entryPoint = rae_wgpu_sv("fs"); fs.targetCount = 1; fs.targets = &cts;
    WGPURenderPipelineDescriptor pd; memset(&pd, 0, sizeof(pd));
    pd.layout = NULL;
    pd.vertex.module = mod; pd.vertex.entryPoint = rae_wgpu_sv("vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    g_g2d_img_pipeline = wgpuDeviceCreateRenderPipeline(g_wgpu_dev, &pd);
    wgpuShaderModuleRelease(mod);
    /* Reuse the text sampler (linear, clamp-to-edge); create if text path
     * hasn't run yet. */
    if (!g_g2d_sampler) {
        WGPUSamplerDescriptor sd; memset(&sd, 0, sizeof(sd));
        sd.addressModeU = WGPUAddressMode_ClampToEdge;
        sd.addressModeV = WGPUAddressMode_ClampToEdge;
        sd.addressModeW = WGPUAddressMode_ClampToEdge;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        sd.lodMaxClamp = 1.0f; sd.maxAnisotropy = 1;
        g_g2d_sampler = wgpuDeviceCreateSampler(g_wgpu_dev, &sd);
    }
}

/* Emit one draw per queued image into the active render pass. */
static void rae_g2d_flush_images(void) {
    if (g_g2d_img_cmd_count <= 0) return;
    rae_g2d_init_img_pipeline();
    /* Grow the per-draw uniform-buffer pool to cover this frame. */
    int total_img_uniforms = g_g2d_img_frame_bind_n + g_g2d_img_cmd_count;
    if (g_g2d_img_ubuf_n < total_img_uniforms) {
        g_g2d_img_ubuf = (WGPUBuffer*)realloc(g_g2d_img_ubuf, (size_t)total_img_uniforms * sizeof(WGPUBuffer));
        for (int i = g_g2d_img_ubuf_n; i < total_img_uniforms; i++) {
            WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
            bd.size = 64; bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            g_g2d_img_ubuf[i] = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        }
        g_g2d_img_ubuf_n = total_img_uniforms;
    }
    WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_img_pipeline, 0);
    if (g_g2d_img_frame_bind_cap < total_img_uniforms) {
        g_g2d_img_frame_binds = (WGPUBindGroup*)realloc(g_g2d_img_frame_binds, (size_t)total_img_uniforms * sizeof(WGPUBindGroup));
        g_g2d_img_frame_bind_cap = total_img_uniforms;
    }
    wgpuRenderPassEncoderSetPipeline(g_g2d_pass, g_g2d_img_pipeline);
    for (int i = 0; i < g_g2d_img_cmd_count; i++) {
        RaeG2dImgCmd* c = &g_g2d_img_cmds[i];
        int idx = c->handle - 1;
        if (idx < 0 || idx >= g_g2d_img_n || !g_g2d_img_view[idx]) continue;
        int uniform_slot = g_g2d_img_frame_bind_n;
        float u[16];
        u[0]=c->rect[0]; u[1]=c->rect[1]; u[2]=c->rect[2]; u[3]=c->rect[3];
        u[4]=c->tint[0]; u[5]=c->tint[1]; u[6]=c->tint[2]; u[7]=c->tint[3];
        u[8]=c->radius;  u[9]=0.0f; u[10]=0.0f; u[11]=0.0f;
        u[12]=c->uv[0];  u[13]=c->uv[1]; u[14]=c->uv[2]; u[15]=c->uv[3];
        wgpuQueueWriteBuffer(g_wgpu_queue, g_g2d_img_ubuf[uniform_slot], 0, u, sizeof(u));
        WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
        e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
        e[1].binding = 1; e[1].buffer = g_g2d_img_ubuf[uniform_slot]; e[1].size = 64;
        e[2].binding = 2; e[2].textureView = g_g2d_img_view[idx];
        e[3].binding = 3; e[3].sampler = g_g2d_sampler;
        WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
        bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
        WGPUBindGroup bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
        g_g2d_img_frame_binds[g_g2d_img_frame_bind_n++] = bind;   /* released after submit */
        rae_g2d_set_scissor(c->clip);
        wgpuRenderPassEncoderSetBindGroup(g_g2d_pass, 0, bind, 0, NULL);
        wgpuRenderPassEncoderDraw(g_g2d_pass, 6, 1, 0, 0);
    }
    wgpuBindGroupLayoutRelease(bgl);
    g_g2d_img_cmd_count = 0;
}

void rae_ext_gpu2d_beginFrame(double r, double g, double b, double a) {
    g_g2d_last_present_ok = 0;
    g_g2d_prim_count = 0;
    for (int i = 0; i < RAE_SDF_MAX_ATLAS; i++) g_g2d_text_count[i] = 0;
    g_g2d_img_cmd_count = 0;
    rae_g2d_clip_reset();
    g_g2d_img_frame_bind_n = 0;
    g_g2d_frame_buf_n = 0;
    g_g2d_frame_bind_n = 0;
    if (!g_g2d_off_view) return;
    /* Render into the persistent offscreen target (NOT the surface drawable),
     * so a frame always renders regardless of window compositing. */
    g_g2d_enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPURenderPassColorAttachment ca; memset(&ca, 0, sizeof(ca));
    ca.view = g_g2d_off_view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue.r = r; ca.clearValue.g = g; ca.clearValue.b = b; ca.clearValue.a = a;
    WGPURenderPassDescriptor rp; memset(&rp, 0, sizeof(rp));
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
    g_g2d_pass = wgpuCommandEncoderBeginRenderPass(g_g2d_enc, &rp);
}

/* Headless verification: copy the just-rendered surface texture back to a
 * mapped buffer and save it as a BMP. Called from endFrame after submit while
 * g_g2d_frame_tex is still alive (before present/release). The surface is
 * configured with CopySrc usage (rae_g2d_configure). Gated on the env var so
 * it costs nothing in normal runs. The readback row stride must be 256-aligned
 * (WebGPU copy requirement); we unpad into a tight RGBA buffer for SDL. */
static void rae_g2d_save_screenshot(const char* path) {
    if (!path || !g_g2d_off_tex || !g_wgpu_dev) return;
    int w = g_g2d_off_w, h = g_g2d_off_h;
    if (w <= 0 || h <= 0) return;
    uint32_t bpr = (uint32_t)w * 4u;
    uint32_t padded = (bpr + 255u) & ~255u;            /* 256-byte row align */
    size_t bytes = (size_t)padded * (size_t)h;
    WGPUBufferDescriptor rbd; memset(&rbd, 0, sizeof(rbd));
    rbd.size = bytes; rbd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer staging = wgpuDeviceCreateBuffer(g_wgpu_dev, &rbd);
    if (!staging) return;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
    WGPUTexelCopyTextureInfo src; memset(&src, 0, sizeof(src));
    src.texture = g_g2d_off_tex; src.mipLevel = 0; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst; memset(&dst, 0, sizeof(dst));
    dst.buffer = staging; dst.layout.offset = 0;
    dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext; ext.width = (uint32_t)w; ext.height = (uint32_t)h; ext.depthOrArrayLayers = 1;
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);
    g_wgpu_map_done = 0;
    WGPUBufferMapCallbackInfo mci; memset(&mci, 0, sizeof(mci));
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = rae_wgpu_on_map;
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
    while (!g_wgpu_map_done) wgpuDevicePoll(g_wgpu_dev, true, NULL);
    const unsigned char* px = (const unsigned char*)wgpuBufferGetConstMappedRange(staging, 0, bytes);
    if (px) {
        unsigned char* rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4u);
        if (rgba) {
            int swap_rb = (g_g2d_fmt == WGPUTextureFormat_BGRA8Unorm ||
                           g_g2d_fmt == WGPUTextureFormat_BGRA8UnormSrgb);
            for (int y = 0; y < h; y++) {
                const unsigned char* row = px + (size_t)y * padded;
                unsigned char* orow = rgba + (size_t)y * (size_t)w * 4u;
                for (int x = 0; x < w; x++) {
                    unsigned char c0 = row[x*4+0], c1 = row[x*4+1], c2 = row[x*4+2];
                    if (swap_rb) { orow[x*4+0] = c2; orow[x*4+1] = c1; orow[x*4+2] = c0; }
                    else        { orow[x*4+0] = c0; orow[x*4+1] = c1; orow[x*4+2] = c2; }
                    orow[x*4+3] = 255;
                }
            }
            SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, rgba, w * 4);
            if (s) { SDL_SaveBMP(s, path); SDL_DestroySurface(s); }
            free(rgba);
        }
        wgpuBufferUnmap(staging);
    }
    wgpuBufferRelease(staging);
}

void rae_ext_gpu2d_endFrame(void) {
    if (!g_g2d_pass) return;
    rae_ext_gpu2d_flush();
    wgpuRenderPassEncoderEnd(g_g2d_pass);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g_g2d_enc, NULL);
    wgpuQueueSubmit(g_wgpu_queue, 1, &cb);
    for (int i = 0; i < g_g2d_frame_bind_n; i++) wgpuBindGroupRelease(g_g2d_frame_binds[i]);
    g_g2d_frame_bind_n = 0;
    for (int i = 0; i < g_g2d_frame_buf_n; i++) wgpuBufferRelease(g_g2d_frame_bufs[i]);
    g_g2d_frame_buf_n = 0;
    /* The per-image bind groups were live for the pass; safe to free now. */
    for (int i = 0; i < g_g2d_img_frame_bind_n; i++) wgpuBindGroupRelease(g_g2d_img_frame_binds[i]);
    g_g2d_img_frame_bind_n = 0;
    wgpuCommandBufferRelease(cb);
    wgpuRenderPassEncoderRelease(g_g2d_pass); g_g2d_pass = NULL;
    wgpuCommandEncoderRelease(g_g2d_enc); g_g2d_enc = NULL;

    /* Headless screenshot reads the offscreen target — works even when the
     * surface can't vend a drawable. */
    if (g_sdl_headless_ms > 0) {
        const char* shot = getenv("RAE_GPU2D_SCREENSHOT");
        if (shot) rae_g2d_save_screenshot(shot);
    }

    /* Present best-effort: copy the offscreen image into the surface drawable
     * and present. If the OS won't vend a drawable (window occluded / display
     * asleep / headless), skip — the frame already rendered + screenshotted. */
    WGPUSurfaceTexture st; memset(&st, 0, sizeof(st));
    wgpuSurfaceGetCurrentTexture(g_g2d_surface, &st);
    if (st.texture &&
        (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
         st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
        WGPUCommandEncoder penc = wgpuDeviceCreateCommandEncoder(g_wgpu_dev, NULL);
        WGPUTexelCopyTextureInfo cs; memset(&cs, 0, sizeof(cs)); cs.texture = g_g2d_off_tex; cs.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo cd; memset(&cd, 0, sizeof(cd)); cd.texture = st.texture; cd.aspect = WGPUTextureAspect_All;
        WGPUExtent3D ext; ext.width = (uint32_t)g_sdl_w; ext.height = (uint32_t)g_sdl_h; ext.depthOrArrayLayers = 1;
        wgpuCommandEncoderCopyTextureToTexture(penc, &cs, &cd, &ext);
        WGPUCommandBuffer pcb = wgpuCommandEncoderFinish(penc, NULL);
        wgpuQueueSubmit(g_wgpu_queue, 1, &pcb);
        wgpuCommandBufferRelease(pcb); wgpuCommandEncoderRelease(penc);
        wgpuSurfacePresent(g_g2d_surface);
        g_g2d_last_present_ok = 1;
    }
    if (st.texture) wgpuTextureRelease(st.texture);
}

rae_Bool rae_ext_gpu2d_lastPresentOk(void) {
    return g_g2d_last_present_ok != 0;
}

void rae_ext_gpu2d_flush(void) {
    if (!g_g2d_pass) return;
    int have_text = 0;
    for (int i = 0; i < RAE_SDF_MAX_ATLAS; i++) if (g_g2d_text_count[i] > 0) have_text = 1;
    int have_img = (g_g2d_img_cmd_count > 0);
    if (g_g2d_prim_count > 0 || have_text || have_img) {
        /* rae_g2d_init_pipeline also creates the shared viewport uniform that
         * the box, image, and text bind groups reference at binding 0. */
        rae_g2d_init_pipeline();
        float xf[8]; rae_g2d_compute_xform(xf);
        wgpuQueueWriteBuffer(g_wgpu_queue, g_g2d_uniform, 0, xf, sizeof(xf));
    }
    if (g_g2d_prim_count > 0) {
        WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
        bd.size = (uint64_t)g_g2d_prim_count * G2D_PRIM_FLOATS * sizeof(float);
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        WGPUBuffer instbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
        wgpuQueueWriteBuffer(g_wgpu_queue, instbuf, 0, g_g2d_prims,
                             (size_t)g_g2d_prim_count * G2D_PRIM_FLOATS * sizeof(float));
        WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_pipeline, 0);
        rae_g2d_keep_frame_buf(instbuf);
        wgpuRenderPassEncoderSetPipeline(g_g2d_pass, g_g2d_pipeline);
        /* Draw contiguous same-clip runs. Each run sets the scissor (#144, the
         * axis-aligned bbox cull) and binds a per-run clip uniform (#118, the
         * rounded-corner SDF applied in the fragment shader). instance_index in
         * the shader includes firstInstance, so the storage buffer still
         * indexes the right primitive. */
        int bs = 0;
        while (bs < g_g2d_prim_count) {
            int clip = (g_g2d_prim_clip && bs < g_g2d_prim_clip_cap) ? g_g2d_prim_clip[bs] : 0;
            int be = bs + 1;
            while (be < g_g2d_prim_count) {
                int ec = (g_g2d_prim_clip && be < g_g2d_prim_clip_cap) ? g_g2d_prim_clip[be] : 0;
                if (ec != clip) break;
                be++;
            }
            float cu[8]; rae_g2d_fill_clip_uniform(clip, cu);
            WGPUBufferDescriptor cbd; memset(&cbd, 0, sizeof(cbd));
            cbd.size = sizeof(cu); cbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            WGPUBuffer cub = wgpuDeviceCreateBuffer(g_wgpu_dev, &cbd);
            wgpuQueueWriteBuffer(g_wgpu_queue, cub, 0, cu, sizeof(cu));
            rae_g2d_keep_frame_buf(cub);
            WGPUBindGroupEntry e[3]; memset(e, 0, sizeof(e));
            e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
            e[1].binding = 1; e[1].buffer = instbuf;
            e[1].size = (uint64_t)g_g2d_prim_count * G2D_PRIM_FLOATS * sizeof(float);
            e[2].binding = 2; e[2].buffer = cub; e[2].size = sizeof(cu);
            WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
            bgd.layout = bgl; bgd.entryCount = 3; bgd.entries = e;
            WGPUBindGroup bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            rae_g2d_keep_frame_bind(bind);
            rae_g2d_set_scissor(clip);
            wgpuRenderPassEncoderSetBindGroup(g_g2d_pass, 0, bind, 0, NULL);
            wgpuRenderPassEncoderDraw(g_g2d_pass, 6, (uint32_t)(be - bs), 0, (uint32_t)bs);
            bs = be;
        }
        wgpuBindGroupLayoutRelease(bgl);
        g_g2d_prim_count = 0;
    }
    /* Images on top of boxes, under text. */
    rae_g2d_flush_images();
    if (have_text) {
        /* One text draw per atlas that has glyphs this frame (so Roboto text
         * and the Material-icon atlas coexist). */
        rae_g2d_init_text_pipeline();
        wgpuRenderPassEncoderSetPipeline(g_g2d_pass, g_g2d_text_pipeline);
        for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
            if (g_g2d_text_count[ai] <= 0) continue;
            if (!rae_g2d_atlas_texview(ai + 1)) continue;
            WGPUBufferDescriptor bd; memset(&bd, 0, sizeof(bd));
            bd.size = (uint64_t)g_g2d_text_count[ai] * G2D_TEXT_FLOATS * sizeof(float);
            bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
            WGPUBuffer instbuf = wgpuDeviceCreateBuffer(g_wgpu_dev, &bd);
            wgpuQueueWriteBuffer(g_wgpu_queue, instbuf, 0, g_g2d_text_prims[ai],
                                 (size_t)g_g2d_text_count[ai] * G2D_TEXT_FLOATS * sizeof(float));
            WGPUTextureView view = rae_g2d_atlas_texview(ai + 1);
            WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(g_g2d_text_pipeline, 0);
            WGPUBindGroupEntry e[4]; memset(e, 0, sizeof(e));
            e[0].binding = 0; e[0].buffer = g_g2d_uniform; e[0].size = 32;
            e[1].binding = 1; e[1].buffer = instbuf;
            e[1].size = (uint64_t)g_g2d_text_count[ai] * G2D_TEXT_FLOATS * sizeof(float);
            e[2].binding = 2; e[2].textureView = view;
            e[3].binding = 3; e[3].sampler = g_g2d_sampler;
            WGPUBindGroupDescriptor bgd; memset(&bgd, 0, sizeof(bgd));
            bgd.layout = bgl; bgd.entryCount = 4; bgd.entries = e;
            WGPUBindGroup bind = wgpuDeviceCreateBindGroup(g_wgpu_dev, &bgd);
            wgpuBindGroupLayoutRelease(bgl);
            rae_g2d_keep_frame_buf(instbuf);
            rae_g2d_keep_frame_bind(bind);
            wgpuRenderPassEncoderSetBindGroup(g_g2d_pass, 0, bind, 0, NULL);
            int cnt = g_g2d_text_count[ai];
            int* tclip = g_g2d_text_clip[ai];
            int tcap = g_g2d_text_clip_cap[ai];
            int ts = 0;
            while (ts < cnt) {
                int clip = (tclip && ts < tcap) ? tclip[ts] : 0;
                int te = ts + 1;
                while (te < cnt) {
                    int ec = (tclip && te < tcap) ? tclip[te] : 0;
                    if (ec != clip) break;
                    te++;
                }
                rae_g2d_set_scissor(clip);
                wgpuRenderPassEncoderDraw(g_g2d_pass, 6, (uint32_t)(te - ts), 0, (uint32_t)ts);
                ts = te;
            }
            g_g2d_text_count[ai] = 0;
        }
    }
}

void rae_ext_gpu2d_closeWindow(void) {
    if (g_g2d_off_view) { wgpuTextureViewRelease(g_g2d_off_view); g_g2d_off_view = NULL; }
    if (g_g2d_off_tex)  { wgpuTextureRelease(g_g2d_off_tex);  g_g2d_off_tex = NULL; }
    g_g2d_off_w = 0; g_g2d_off_h = 0;
    for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
        if (g_g2d_text_bind[ai]) { wgpuBindGroupRelease(g_g2d_text_bind[ai]); g_g2d_text_bind[ai] = NULL; }
        if (g_g2d_text_instbuf[ai]) { wgpuBufferRelease(g_g2d_text_instbuf[ai]); g_g2d_text_instbuf[ai] = NULL; g_g2d_text_cap[ai] = 0; }
        if (g_g2d_text_prims[ai]) { free(g_g2d_text_prims[ai]); g_g2d_text_prims[ai] = NULL; g_g2d_text_capf[ai] = 0; }
        g_g2d_text_count[ai] = 0;
    }
    if (g_g2d_text_pipeline) { wgpuRenderPipelineRelease(g_g2d_text_pipeline); g_g2d_text_pipeline = NULL; }
    if (g_g2d_sampler) { wgpuSamplerRelease(g_g2d_sampler); g_g2d_sampler = NULL; }
    for (int ai = 0; ai < RAE_SDF_MAX_ATLAS; ai++) {
        if (g_g2d_atlas_view[ai]) { wgpuTextureViewRelease(g_g2d_atlas_view[ai]); g_g2d_atlas_view[ai] = NULL; }
        if (g_g2d_atlas_tex[ai]) { wgpuTextureRelease(g_g2d_atlas_tex[ai]); g_g2d_atlas_tex[ai] = NULL; }
    }
    if (g_g2d_bind) { wgpuBindGroupRelease(g_g2d_bind); g_g2d_bind = NULL; }
    if (g_g2d_instbuf) { wgpuBufferRelease(g_g2d_instbuf); g_g2d_instbuf = NULL; g_g2d_inst_cap = 0; }
    if (g_g2d_uniform) { wgpuBufferRelease(g_g2d_uniform); g_g2d_uniform = NULL; }
    if (g_g2d_pipeline) { wgpuRenderPipelineRelease(g_g2d_pipeline); g_g2d_pipeline = NULL; }
    if (g_g2d_prims) { free(g_g2d_prims); g_g2d_prims = NULL; g_g2d_prim_capf = 0; }
    g_g2d_prim_count = 0;
    for (int i = 0; i < g_g2d_frame_bind_n; i++) wgpuBindGroupRelease(g_g2d_frame_binds[i]);
    g_g2d_frame_bind_n = 0;
    if (g_g2d_frame_binds) { free(g_g2d_frame_binds); g_g2d_frame_binds = NULL; g_g2d_frame_bind_cap = 0; }
    for (int i = 0; i < g_g2d_frame_buf_n; i++) wgpuBufferRelease(g_g2d_frame_bufs[i]);
    g_g2d_frame_buf_n = 0;
    if (g_g2d_frame_bufs) { free(g_g2d_frame_bufs); g_g2d_frame_bufs = NULL; g_g2d_frame_buf_cap = 0; }
    /* Image pipeline + textures. */
    for (int i = 0; i < g_g2d_img_frame_bind_n; i++) wgpuBindGroupRelease(g_g2d_img_frame_binds[i]);
    g_g2d_img_frame_bind_n = 0;
    if (g_g2d_img_frame_binds) { free(g_g2d_img_frame_binds); g_g2d_img_frame_binds = NULL; g_g2d_img_frame_bind_cap = 0; }
    for (int i = 0; i < g_g2d_img_ubuf_n; i++) if (g_g2d_img_ubuf[i]) wgpuBufferRelease(g_g2d_img_ubuf[i]);
    if (g_g2d_img_ubuf) { free(g_g2d_img_ubuf); g_g2d_img_ubuf = NULL; g_g2d_img_ubuf_n = 0; }
    for (int i = 0; i < g_g2d_img_n; i++) {
        if (g_g2d_img_view[i]) { wgpuTextureViewRelease(g_g2d_img_view[i]); g_g2d_img_view[i] = NULL; }
        if (g_g2d_img_tex[i]) { wgpuTextureRelease(g_g2d_img_tex[i]); g_g2d_img_tex[i] = NULL; }
    }
    g_g2d_img_n = 0;
    g_g2d_img_key_n = 0;
    if (g_g2d_img_cmds) { free(g_g2d_img_cmds); g_g2d_img_cmds = NULL; g_g2d_img_cmd_cap = 0; }
    g_g2d_img_cmd_count = 0;
    if (g_g2d_img_pipeline) { wgpuRenderPipelineRelease(g_g2d_img_pipeline); g_g2d_img_pipeline = NULL; }
    if (g_g2d_surface) { wgpuSurfaceRelease(g_g2d_surface); g_g2d_surface = NULL; }
    for (int i = 0; i < 7; i++) {
        if (g_g2d_cursors[i]) { SDL_DestroyCursor(g_g2d_cursors[i]); g_g2d_cursors[i] = NULL; }
    }
    g_g2d_cursor_kind = -1;
    if (g_g2d_metal_view) { SDL_Metal_DestroyView(g_g2d_metal_view); g_g2d_metal_view = NULL; }
    if (g_sdl_win) { SDL_DestroyWindow(g_sdl_win); g_sdl_win = NULL; }
}
#else  /* no SDL3: stubs so a webgpu-only build still links */
void rae_ext_gpu2d_initWindow(int64_t w, int64_t h, rae_String t) { (void)w; (void)h; (void)t; }
rae_Bool rae_ext_gpu2d_pollClose(void) { return 1; }
void rae_ext_gpu2d_waitEvents(double timeoutSec) { (void)timeoutSec; }
int64_t rae_ext_gpu2d_loadImage(rae_String path) { (void)path; return 0; }
int64_t rae_ext_gpu2d_decodeImageProbe(rae_String path) { (void)path; return 0; }
int64_t rae_ext_gpu2d_loadImageKey(rae_String key, rae_String path) { (void)key; (void)path; return 0; }
rae_Bool rae_ext_gpu2d_hasImageKey(rae_String key) { (void)key; return 0; }
void rae_ext_gpu2d_drawImageKey(rae_String key, double x, double y, double w, double h, double radius, int64_t tint) { (void)key; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; }
void rae_ext_gpu2d_drawImageKeyScaled(rae_String key, double x, double y, double w, double h, double radius, int64_t tint, int64_t scaleMode) { (void)key; (void)x; (void)y; (void)w; (void)h; (void)radius; (void)tint; (void)scaleMode; }
void rae_ext_gpu2d_drawImage(double x, double y, double w, double h, double radius, int64_t handle, int64_t tint) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)handle; (void)tint; }
double rae_ext_gpu2d_pointerX(void) { return 0.0; }
double rae_ext_gpu2d_pointerY(void) { return 0.0; }
rae_Bool rae_ext_gpu2d_pointerDown(void) { return 0; }
rae_Bool rae_ext_gpu2d_pointerPressed(void) { return 0; }
rae_Bool rae_ext_gpu2d_pointerReleased(void) { return 0; }
double rae_ext_gpu2d_wheelMove(void) { return 0.0; }
void rae_ext_gpu2d_setMouseCursor(int64_t kind) { (void)kind; }
double rae_ext_gpu2d_nowSeconds(void) { return 0.0; }
int64_t rae_ext_gpu2d_windowWidth(void) { return 0; }
int64_t rae_ext_gpu2d_windowHeight(void) { return 0; }
void rae_ext_gpu2d_setWindowPosition(int64_t x, int64_t y) { (void)x; (void)y; }
int64_t rae_ext_gpu2d_windowPositionX(void) { return 0; }
int64_t rae_ext_gpu2d_windowPositionY(void) { return 0; }
rae_Bool rae_ext_gpu2d_windowResized(void) { return 0; }
void rae_ext_gpu2d_setDesignResolution(double w, double h, int64_t fit) { (void)w; (void)h; (void)fit; }
double rae_ext_gpu2d_designWidth(void) { return 0.0; }
double rae_ext_gpu2d_designHeight(void) { return 0.0; }
double rae_ext_gpu2d_dpr(void) { return 1.0; }
void rae_ext_gpu2d_beginFrame(double r, double g, double b, double a) { (void)r; (void)g; (void)b; (void)a; }
void rae_ext_gpu2d_endFrame(void) {}
rae_Bool rae_ext_gpu2d_lastPresentOk(void) { return 0; }
void rae_ext_gpu2d_flush(void) {}
void rae_ext_gpu2d_closeWindow(void) {}
void rae_ext_gpu2d_drawRect(double x, double y, double w, double h, int64_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void rae_ext_gpu2d_drawRoundedRect(double x, double y, double w, double h, double radius, int64_t color) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)color; }
void rae_ext_gpu2d_drawBox(double x, double y, double w, double h, double radius, int64_t fill, double borderWidth, int64_t border) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)fill; (void)borderWidth; (void)border; }
void rae_ext_gpu2d_drawGradientRect(double x, double y, double w, double h, double radius, int64_t from, int64_t to, double angleDeg) { (void)x; (void)y; (void)w; (void)h; (void)radius; (void)from; (void)to; (void)angleDeg; }
void rae_ext_gpu2d_drawLine(double x0, double y0, double x1, double y1, double thickness, int64_t color) { (void)x0; (void)y0; (void)x1; (void)y1; (void)thickness; (void)color; }
void rae_ext_gpu2d_drawGlyph(double sx0, double sy0, double sx1, double sy1, double u0, double v0, double u1, double v1, int64_t atlas, double pxRange, int64_t color) { (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)u0; (void)v0; (void)u1; (void)v1; (void)atlas; (void)pxRange; (void)color; }
void rae_ext_gpu2d_drawGlyphEx(double sx0, double sy0, double sx1, double sy1, double u0, double v0, double u1, double v1, int64_t atlas, double pxRange, int64_t color, double outlineWidth, int64_t outlineColor, double softness) { (void)sx0; (void)sy0; (void)sx1; (void)sy1; (void)u0; (void)v0; (void)u1; (void)v1; (void)atlas; (void)pxRange; (void)color; (void)outlineWidth; (void)outlineColor; (void)softness; }
#endif /* RAE_HAS_SDL3 */
#endif /* RAE_HAS_WEBGPU */

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
#ifndef __wasm__
#include <sys/wait.h>
#endif

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

void rae_ext_sys_spotify_launch(void) {
    fprintf(stderr, "[spotify-c] launch\n");
    static const char* lines[] = {
        "tell application \"Spotify\" to if it is not running then launch",
        NULL
    };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] launch failed (osascript rc=%d)\n", rc);
}

void rae_ext_sys_spotify_play(void) {
    fprintf(stderr, "[spotify-c] play\n");
    static const char* lines[] = { "tell application \"Spotify\" to play", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] play failed (osascript rc=%d) — Spotify not running or Automation permission denied?\n", rc);
}

void rae_ext_sys_spotify_pause(void) {
    fprintf(stderr, "[spotify-c] pause\n");
    static const char* lines[] = { "tell application \"Spotify\" to pause", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] pause failed (osascript rc=%d)\n", rc);
}

void rae_ext_sys_spotify_next(void) {
    fprintf(stderr, "[spotify-c] next\n");
    static const char* lines[] = { "tell application \"Spotify\" to next track", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] next failed (osascript rc=%d)\n", rc);
}

void rae_ext_sys_spotify_previous(void) {
    fprintf(stderr, "[spotify-c] previous\n");
    static const char* lines[] = { "tell application \"Spotify\" to previous track", NULL };
    int rc = rae_osascript_run(lines);
    if (rc != 0) fprintf(stderr, "[spotify-c] previous failed (osascript rc=%d)\n", rc);
}

/* Play a specific Spotify URI directly. Accepts spotify:track:<id>,
 * spotify:album:<id>, spotify:playlist:<id>, etc. Used when the local
 * album.json carries an explicit Spotify URI. */
void rae_ext_sys_spotify_playUri(rae_String uri) {
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
void rae_ext_sys_spotify_playQuery(rae_String query) {
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

void rae_ext_sys_spotify_refresh(void) {
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

rae_String rae_ext_sys_spotify_state(void)       { return rae_cstr_to_owned_rae_string(g_spotify_cache.state); }
rae_String rae_ext_sys_spotify_trackId(void)     { return rae_cstr_to_owned_rae_string(g_spotify_cache.trackId); }
rae_String rae_ext_sys_spotify_trackName(void)   { return rae_cstr_to_owned_rae_string(g_spotify_cache.trackName); }
rae_String rae_ext_sys_spotify_artistName(void)  { return rae_cstr_to_owned_rae_string(g_spotify_cache.artistName); }
rae_String rae_ext_sys_spotify_albumName(void)   { return rae_cstr_to_owned_rae_string(g_spotify_cache.albumName); }
rae_String rae_ext_sys_spotify_artworkUrl(void)  { return rae_cstr_to_owned_rae_string(g_spotify_cache.artworkUrl); }
double rae_ext_sys_spotify_position(void)        { return g_spotify_cache.positionSec; }
double rae_ext_sys_spotify_duration(void)        { return g_spotify_cache.durationSec; }

/* Completeness check for a downloaded artwork file. Interrupted curl
 * writes leave truncated files (sizes are clean 4 KiB multiples), and
 * lenient decoders (ImageIO) render those half-image-half-grey with
 * NO error — the "In Electric Blue" glitch. A JPEG must carry its EOI
 * marker (FF D9) near the end (some encoders pad a few trailing
 * bytes, so scan the last 64). Non-JPEG payloads only need to be
 * non-empty — PNG never comes through this path today. */
static int rae_spotify_artwork_file_complete(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[3] = {0, 0, 0};
    if (fread(head, 1, 3, f) != 3) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 4) { fclose(f); return 0; }
    if (head[0] != 0xFF || head[1] != 0xD8 || head[2] != 0xFF) {
        fclose(f);
        return 1;
    }
    long tail_len = sz < 64 ? sz : 64;
    if (fseek(f, -tail_len, SEEK_END) != 0) { fclose(f); return 0; }
    unsigned char tail[64];
    if (fread(tail, 1, (size_t)tail_len, f) != (size_t)tail_len) { fclose(f); return 0; }
    fclose(f);
    for (long i = tail_len - 2; i >= 0; i--) {
        if (tail[i] == 0xFF && tail[i + 1] == 0xD9) return 1;
    }
    return 0;
}

/* Fetch `url_c` into `out_c` ATOMICALLY: curl writes to `<out>.part`,
 * the result is completeness-checked, and only then renamed into
 * place. An app quit mid-download (auto-exit headless runs included)
 * can no longer commit a half-file into the cache. */
static int rae_spotify_fetch_artwork_atomic(const char* url_c, const char* out_c) {
    if (!url_c || !out_c || !url_c[0] || !out_c[0]) return 0;
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.part", out_c) >= (int)sizeof(tmp)) return 0;
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        char* argv[] = { (char*)"curl", (char*)"-sLf", (char*)url_c, (char*)"-o", (char*)tmp, NULL };
        execvp("/usr/bin/curl", argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) ok = rae_spotify_artwork_file_complete(tmp);
    if (!ok) {
        unlink(tmp);
        return 0;
    }
    if (rename(tmp, out_c) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
}

rae_Bool rae_ext_sys_spotify_fetchArtwork(rae_String url, rae_String outPath) {
    if (!url.data || url.len == 0 || !outPath.data || outPath.len == 0) return false;
    char* url_c = malloc((size_t)url.len + 1);
    char* out_c = malloc((size_t)outPath.len + 1);
    if (!url_c || !out_c) { free(url_c); free(out_c); return false; }
    memcpy(url_c, url.data, (size_t)url.len); url_c[url.len] = '\0';
    memcpy(out_c, outPath.data, (size_t)outPath.len); out_c[outPath.len] = '\0';
    int ok = rae_spotify_fetch_artwork_atomic(url_c, out_c);
    free(url_c); free(out_c);
    return ok ? true : false;
}

typedef struct {
    char* url;
    char* out;
    int status; /* 0=pending, 1=success, 2=failed */
    pthread_t thread;
} RaeSpotifyArtworkJob;

#define RAE_SPOTIFY_ART_JOBS 64
static RaeSpotifyArtworkJob g_spotify_art_jobs[RAE_SPOTIFY_ART_JOBS];
static pthread_mutex_t g_spotify_art_mu = PTHREAD_MUTEX_INITIALIZER;

static char* rae_strndup_bytes(const uint8_t* data, int64_t len) {
    if (!data || len <= 0) return NULL;
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) return NULL;
    memcpy(out, data, (size_t)len);
    out[len] = '\0';
    return out;
}

static void* rae_spotify_art_worker(void* arg) {
    RaeSpotifyArtworkJob* job = (RaeSpotifyArtworkJob*)arg;
    int ok = rae_spotify_fetch_artwork_atomic(job->url, job->out);
    pthread_mutex_lock(&g_spotify_art_mu);
    job->status = ok ? 1 : 2;
    pthread_mutex_unlock(&g_spotify_art_mu);
    return NULL;
}

rae_Bool rae_ext_sys_spotify_fetchArtworkAsync(rae_String url, rae_String outPath) {
    if (!url.data || url.len == 0 || !outPath.data || outPath.len == 0) return false;
    char* url_c = rae_strndup_bytes(url.data, url.len);
    char* out_c = rae_strndup_bytes(outPath.data, outPath.len);
    if (!url_c || !out_c) { free(url_c); free(out_c); return false; }

    pthread_mutex_lock(&g_spotify_art_mu);
    int free_idx = -1;
    for (int i = 0; i < RAE_SPOTIFY_ART_JOBS; i++) {
        if (g_spotify_art_jobs[i].out) {
            if (strcmp(g_spotify_art_jobs[i].out, out_c) == 0) {
                pthread_mutex_unlock(&g_spotify_art_mu);
                free(url_c);
                free(out_c);
                return true;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx < 0) {
        pthread_mutex_unlock(&g_spotify_art_mu);
        free(url_c);
        free(out_c);
        return false;
    }
    RaeSpotifyArtworkJob* job = &g_spotify_art_jobs[free_idx];
    job->url = url_c;
    job->out = out_c;
    job->status = 0;
    if (pthread_create(&job->thread, NULL, rae_spotify_art_worker, job) != 0) {
        job->url = NULL;
        job->out = NULL;
        job->status = 2;
        pthread_mutex_unlock(&g_spotify_art_mu);
        free(url_c);
        free(out_c);
        return false;
    }
    pthread_detach(job->thread);
    pthread_mutex_unlock(&g_spotify_art_mu);
    return true;
}

int64_t rae_ext_sys_spotify_fetchArtworkStatus(rae_String outPath) {
    if (!outPath.data || outPath.len == 0) return 0;
    char* out_c = rae_strndup_bytes(outPath.data, outPath.len);
    if (!out_c) return 0;
    int result = 0;
    pthread_mutex_lock(&g_spotify_art_mu);
    for (int i = 0; i < RAE_SPOTIFY_ART_JOBS; i++) {
        RaeSpotifyArtworkJob* job = &g_spotify_art_jobs[i];
        if (job->out && strcmp(job->out, out_c) == 0) {
            result = job->status;
            if (job->status != 0) {
                free(job->url);
                free(job->out);
                job->url = NULL;
                job->out = NULL;
                job->status = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_spotify_art_mu);
    free(out_c);
    return result;
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

rae_String rae_ext_sys_spotify_itunesSearchArtworkUrl(rae_String term) {
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

void rae_ext_sys_spotify_launch(void)   {}
void rae_ext_sys_spotify_play(void)     {}
void rae_ext_sys_spotify_pause(void)    {}
void rae_ext_sys_spotify_next(void)     {}
void rae_ext_sys_spotify_previous(void) {}
void rae_ext_sys_spotify_refresh(void)  {}
rae_String rae_ext_sys_spotify_state(void)      { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_trackId(void)    { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_trackName(void)  { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_artistName(void) { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_albumName(void)  { return (rae_String){NULL, 0, 0, 0}; }
rae_String rae_ext_sys_spotify_artworkUrl(void) { return (rae_String){NULL, 0, 0, 0}; }
double rae_ext_sys_spotify_position(void)       { return 0.0; }
double rae_ext_sys_spotify_duration(void)       { return 0.0; }
void rae_ext_sys_spotify_playUri(rae_String uri) { (void)uri; }
void rae_ext_sys_spotify_playQuery(rae_String query) { (void)query; }
rae_Bool rae_ext_sys_spotify_fetchArtwork(rae_String url, rae_String outPath) { (void)url; (void)outPath; return false; }
rae_Bool rae_ext_sys_spotify_fetchArtworkAsync(rae_String url, rae_String outPath) { (void)url; (void)outPath; return false; }
int64_t rae_ext_sys_spotify_fetchArtworkStatus(rae_String outPath) { (void)outPath; return 2; }
rae_String rae_ext_sys_spotify_itunesSearchArtworkUrl(rae_String term) { (void)term; return (rae_String){NULL, 0, 0, 0}; }

#endif  /* __APPLE__ */
