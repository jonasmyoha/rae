/* Console, process, environment, and filesystem primitives. Raw OS calls stay C; path/convenience policy can migrate to Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

/* One getline from stdin: the line as read, its newline included, or empty
 * at end of input. lib/Io.rae readLine drops the line ending. */
rae_String rae_ext_rae_io_read_line_raw(void) {
  char* buffer = NULL;
  size_t capacity = 0;
  ssize_t length = getline(&buffer, &capacity, stdin);
  if (length <= 0) {
    free(buffer);
    return (rae_String){NULL, 0, 0, 0};
  }
  rae_mem_str_tag(buffer, (int64_t)capacity, RAE_SITE_READ_LINE);
  return (rae_String){(uint8_t*)buffer, (int64_t)length, (int64_t)capacity, 1};
}

/* `text` written to stderr and flushed (lib/Sys.rae's asset-miss message) */
void rae_ext_rae_io_write_error(rae_String text) {
  if (text.data && text.len > 0) fwrite(text.data, 1, (size_t)text.len, stderr);
  fflush(stderr);
}

rae_Char rae_ext_rae_io_read_char(void) {
  // TODO: Proper UTF-8 read from stdin
  return (uint32_t)getchar();
}

void rae_ext_rae_sys_exit(int64_t code) {
  exit((int)code);
}

/* A library-level runtime error (docs/integer-semantics.md "trap"): one line
 * on stderr and exit RAE_TRAP_EXIT_CODE, the same shape as the compiler's own
 * Int traps minus the source position a library function does not have.
 * Used by lib/core for a List index past the end. */
void rae_ext_rae_runtime_error(rae_String message) {
  fprintf(stderr, "runtime error: %.*s\n", (int)message.len,
          message.data ? (const char*)message.data : "");
  fflush(stderr);
  exit(RAE_TRAP_EXIT_CODE);
}

/* A library-level runtime WARNING: one line on stderr, and the program goes
 * on. For a request the library can safely decline (a List write past the
 * end is dropped) — the bug is never silent, and the program never stops. */
void rae_ext_rae_runtime_warning(rae_String message) {
  fprintf(stderr, "warning: %.*s\n", (int)message.len,
          message.data ? (const char*)message.data : "");
  fflush(stderr);
}

/* The warning of `List.set` for an index past the end, for the inline form
 * the C backend emits for a plain-data element (c_expr.c
 * emit_list_fast_access). Same line as lib/core/List.rae's
 * runtimeWarning("List.set: ..."), kept out of line and cold so the hot
 * store stays a length check plus one store. */
__attribute__((cold, noinline)) void rae_list_set_out_of_range(int64_t index, int64_t length) {
  fprintf(stderr, "warning: List.set: index %lld is out of range for length %lld\n",
          (long long)index, (long long)length);
  fflush(stderr);
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

/* Read a file as raw BYTES.
 *
 * rae_ext_rae_sys_read_file already opens "rb" and carries an explicit
 * length, so binary content survives it — but the Rae-side String API
 * decodes UTF-8 (`at` yields a Char32), so there is no way to get at
 * individual bytes of a .glb or .png through it. This returns a Buffer
 * whose elements are one byte each, widened to Int, which is the shape
 * lib/png.rae and lib/gltf.rae already work in.
 *
 * The buffer comes from rae_ext_rae_buf_alloc so the caller frees it with
 * rae_ext_rae_buf_free and it lands in the same accounting as every other
 * Rae allocation. Returns NULL with *outLen = 0 when the file cannot be
 * read; an empty file returns NULL too, which is the same "nothing to
 * read" answer and needs no separate case at the call site.
 */
void* rae_ext_rae_sys_read_file_bytes(rae_String path, rae_Mod_Int64 out) {
  int64_t* outLen = out.ptr;
  if (outLen) *outLen = 0;
  if (!path.data) return NULL;
  FILE* f = fopen((const char*)path.data, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0) { fclose(f); return NULL; }
  int64_t* buf = (int64_t*)rae_ext_rae_buf_alloc((int64_t)len, (int64_t)sizeof(int64_t));
  if (!buf) { fclose(f); return NULL; }
  /* Read into a separate byte buffer, then widen. An earlier version read
   * into the TAIL of the Int allocation and widened downward to save the
   * second allocation; that is wrong. Writing buf[i] spans bytes
   * [8i, 8i+8), which for the last few indices overlaps source bytes that
   * have not been consumed yet, so the final bytes of every file came back
   * corrupted — the .glb magic still read correctly, and the damage only
   * showed up at the end of the buffer. Not worth one malloc. */
  uint8_t* raw = (uint8_t*)malloc((size_t)len);
  if (!raw) { rae_ext_rae_buf_free(buf); fclose(f); return NULL; }
  size_t got = fread(raw, 1, (size_t)len, f);
  fclose(f);
  if (got != (size_t)len) { free(raw); rae_ext_rae_buf_free(buf); return NULL; }
  for (long i = 0; i < len; i++) buf[i] = (int64_t)raw[i];
  free(raw);
  if (outLen) *outLen = (int64_t)len;
  return buf;
}

/* Read a byte RANGE of a file as text.
 *
 * Binary containers embed text chunks — a .glb's JSON, an ID3 tag, an EXIF
 * block — at an offset the caller has already located. Widening those
 * bytes to Int and back through Rae would be both slow and lossy, since
 * Rae's String indexes codepoints rather than bytes. Handing the range
 * straight to the String constructor keeps it exact.
 */
rae_String rae_ext_rae_sys_read_file_text(rae_String path, int64_t offset, int64_t len) {
  if (!path.data || len <= 0 || offset < 0) return (rae_String){NULL, 0, 0, 0};
  FILE* f = fopen((const char*)path.data, "rb");
  if (!f) return (rae_String){NULL, 0, 0, 0};
  if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return (rae_String){NULL, 0, 0, 0}; }
  uint8_t* tmp = (uint8_t*)malloc((size_t)len);
  if (!tmp) { fclose(f); return (rae_String){NULL, 0, 0, 0}; }
  size_t got = fread(tmp, 1, (size_t)len, f);
  fclose(f);
  rae_String out = (got == (size_t)len)
      ? rae_ext_rae_str_from_buf(tmp, (int64_t)len)
      : (rae_String){NULL, 0, 0, 0};
  free(tmp);
  return out;
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

/* A directory read one entry per call (lib/Files.rae listDir): opendir, one
 * readdir (the entry's name, empty after the last), closedir. A NULL
 * directory (one that could not be opened) reads as empty. */
void* rae_ext_rae_sys_dir_open(rae_String folder) {
  if (!folder.data) return NULL;
  return opendir((const char*)folder.data);
}

rae_String rae_ext_rae_sys_dir_next(void* dir) {
  if (!dir) return (rae_String){NULL, 0, 0, 0};
  struct dirent* entry = readdir((DIR*)dir);
  if (!entry) return (rae_String){NULL, 0, 0, 0};
  return rae_ext_rae_str_from_cstr((void*)entry->d_name);
}

void rae_ext_rae_sys_dir_close(void* dir) {
  if (dir) closedir((DIR*)dir);
}

double rae_ext_rae_sys_file_mtime(rae_String path){
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




rae_String rae_ext_rae_str_f64(double v) {
  char buffer[32];
  int len = snprintf(buffer, 32, "%g", v);
  return rae_str_from_buf_impl((uint8_t*)buffer, len, RAE_SITE_FLOAT_TO_STR);
}

rae_String rae_ext_rae_str_f64_ptr(const double* v) {
  return rae_ext_rae_str_f64(*v);
}

/* JSON number formatting for the .raescene writer: the SHORTEST decimal that
 * round-trips the 32-bit float (through double->float on reload via strtod).
 * `%g` defaults to 6 significant digits, so an authored 47.710842 saved as
 * "47.7108" reloaded as a DIFFERENT value and repeated save cycles eroded
 * coordinates. Try increasing precision and keep the shortest string whose
 * `(float)strtod(...)` equals the original; normal coordinates never reach
 * scientific notation. */
rae_String rae_ext_json_number(float v) {
  double d = (double)v;
  char best[40]; int best_len = -1;
  for (int prec = 1; prec <= 9; prec++) {
    char tmp[40];
    int len = snprintf(tmp, sizeof tmp, "%.*g", prec, d);
    if (len > 0 && len < (int)sizeof tmp && (float)strtod(tmp, NULL) == v
        && (best_len < 0 || len < best_len)) {
      best_len = len; memcpy(best, tmp, (size_t)len + 1);
    }
  }
  if (best_len < 0) best_len = snprintf(best, sizeof best, "%.9g", d);
  return rae_str_from_buf_impl((uint8_t*)best, best_len, RAE_SITE_FLOAT_TO_STR);
}

/* Float (f32) views. Formatting promotes to double, so the shared f64
 * formatter produces the same text a plain Float would. */




rae_String rae_ext_rae_str_string(rae_String s) {
    return rae_str_from_buf_impl(s.data, s.len, RAE_SITE_STR_STRING);
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
