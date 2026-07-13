/* String allocation, drop/copy, interpolation, and temp-pool support. Allocation/drop remain kernel for now; higher string algorithms can migrate later.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

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

