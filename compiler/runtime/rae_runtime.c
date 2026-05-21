#include "rae_runtime.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>

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

rae_String rae_ext_rae_str_from_cstr(const void* s) {
  if (!s) return (rae_String){NULL, 0, 0, 0};
  int64_t len = (int64_t)strlen((const char*)s);
  uint8_t* data = malloc(len + 1);
  if (data) {
    memcpy(data, s, len);
    data[len] = '\0';
  }
  return (rae_String){data, len, len + 1, 1};
}

rae_String rae_ext_rae_str_from_buf(const uint8_t* data, int64_t len) {
  if (!data || len < 0) return (rae_String){NULL, 0, 0, 0};
  uint8_t* buf = malloc(len + 1);
  if (buf) {
    memcpy(buf, data, len);
    buf[len] = '\0';
  }
  return (rae_String){buf, len, len + 1, 1};
}

void* rae_ext_rae_str_to_cstr(rae_String s) {
  // We ensure rae_String is always NUL-terminated for convenience,
  // but we should still handle the case where it might not be if we ever change that.
  return (void*)s.data;
}

// Free heap memory only when this String owns it. Static literals
// and borrowed views are passed in with is_owned=0 and are no-ops.
// Safe to call on a "moved-from" String (data=NULL, is_owned=0).
void rae_ext_rae_str_free(rae_String s) {
  if (s.is_owned && s.data) free(s.data);
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
  return (rae_String){buf, src.len, src.len + 1, 1};
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
  return rae_ext_rae_str_from_buf((uint8_t*)buffer, len);
}

rae_String rae_ext_rae_str_i64_ptr(const int64_t* v) {
  return rae_ext_rae_str_i64(*v);
}

rae_String rae_ext_rae_str_f64(double v) {
  char buffer[32];
  int len = snprintf(buffer, 32, "%g", v);
  return rae_ext_rae_str_from_buf((uint8_t*)buffer, len);
}

rae_String rae_ext_rae_str_f64_ptr(const double* v) {
  return rae_ext_rae_str_f64(*v);
}

rae_String rae_ext_rae_str_bool(rae_Bool v) {
  return rae_ext_rae_str_from_cstr(v ? "true" : "false");
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
  return rae_ext_rae_str_from_buf(buffer, len);
}

rae_String rae_ext_rae_str_char_ptr(const uint32_t* v) {
    return rae_ext_rae_str_char(*v);
}

rae_String rae_ext_rae_str_string(rae_String s) {
    return rae_ext_rae_str_from_buf(s.data, s.len);
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
        case RAE_TYPE_STRING:  return v.as.s;
        case RAE_TYPE_CHAR:    return rae_ext_rae_str_char((uint32_t)v.as.i);
        case RAE_TYPE_NONE:    return (rae_String){(uint8_t*)"none", 4};
        default:               return (rae_String){(uint8_t*)"", 0};
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
  RAE_BR_REGISTER(p, count, elem_size);
  return p;
}

void rae_ext_rae_buf_free(void* buf) {
  if (buf) {
    RAE_BR_UNREGISTER(buf);
    free(buf);
  }
}

void* rae_ext_rae_buf_resize(void* buf, int64_t new_count, int64_t elem_size) {
  if (new_count <= 0) {
    if (buf) {
      RAE_BR_UNREGISTER(buf);
      free(buf);
    }
    return NULL;
  }
  RAE_BR_UNREGISTER(buf);
  void* p = realloc(buf, (size_t)new_count * (size_t)elem_size);
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
    if (copy) { memcpy(copy, v, (size_t)len); copy[len] = 0; }
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

#ifdef RAE_HAS_RAYLIB
/* Raylib wrappers for C backend */
#include <raylib.h>

void rae_ext_initWindow(int64_t width, int64_t height, rae_String title) {
    InitWindow((int)width, (int)height, (const char*)title.data);
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
void rae_ext_beginDrawing(void) { BeginDrawing(); }
void rae_ext_endDrawing(void) { EndDrawing(); }
void rae_ext_clearBackground(Color color) { ClearBackground(color); }
rae_Bool rae_ext_isKeyDown(int64_t key) { return IsKeyDown((int)key); }
rae_Bool rae_ext_isKeyPressed(int64_t key) { return IsKeyPressed((int)key); }
int64_t rae_ext_getMouseX(void) { return (int64_t)GetMouseX(); }
int64_t rae_ext_getMouseY(void) { return (int64_t)GetMouseY(); }
rae_Bool rae_ext_isMouseButtonDown(int64_t button) { return IsMouseButtonDown((int)button); }
rae_Bool rae_ext_isMouseButtonPressed(int64_t button) { return IsMouseButtonPressed((int)button); }
rae_Bool rae_ext_isMouseButtonReleased(int64_t button) { return IsMouseButtonReleased((int)button); }
void rae_ext_setMouseScale(double scaleX, double scaleY) { SetMouseScale((float)scaleX, (float)scaleY); }
int64_t rae_ext_getScreenWidth(void) { return (int64_t)GetScreenWidth(); }
int64_t rae_ext_getScreenHeight(void) { return (int64_t)GetScreenHeight(); }
int64_t rae_ext_getRenderWidth(void) { return (int64_t)GetRenderWidth(); }
int64_t rae_ext_getRenderHeight(void) { return (int64_t)GetRenderHeight(); }
int64_t rae_ext_getCurrentMonitor(void) { return (int64_t)GetCurrentMonitor(); }
int64_t rae_ext_getMonitorWidth(int64_t monitor) { return (int64_t)GetMonitorWidth((int)monitor); }
int64_t rae_ext_getMonitorHeight(int64_t monitor) { return (int64_t)GetMonitorHeight((int)monitor); }
void rae_ext_setWindowSize(int64_t width, int64_t height) { SetWindowSize((int)width, (int)height); }
void rae_ext_setWindowPosition(int64_t x, int64_t y) { SetWindowPosition((int)x, (int)y); }
Texture rae_ext_loadTexture(rae_String fileName) { return LoadTexture((const char*)fileName.data); }
void rae_ext_unloadTexture(Texture texture) { UnloadTexture(texture); }
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
    0xe7fd  /* person */
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
