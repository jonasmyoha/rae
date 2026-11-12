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

/* Set `name` to `value` in this process's environment (Sys.setEnv), which the
 * programs it starts or execs inherit. Platform reason: setenv. */
rae_Bool rae_ext_rae_sys_set_env(rae_String name, rae_String value) {
  if (!name.data) return 0;
  return setenv((const char*)name.data, value.data ? (const char*)value.data : "", 1) == 0;
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

/* Why the last file operation on this thread failed (lib/Sys.rae
 * lastFileError): the errno it ended with, 0 when it succeeded. Each file
 * shim below stores it as it returns; Rae maps it to a FileError. Platform
 * reason: errno, which the Rae runtime's own allocations between the call
 * and the read could change. */
static __thread int g_rae_file_errno = 0;

static void rae_file_errno_set(int code) { g_rae_file_errno = code; }

int64_t rae_ext_rae_sys_file_errno(void) { return (int64_t)g_rae_file_errno; }

/* The per-OS errno values lib/Sys.rae maps (FileError): 0 ENOSPC, 1 EDQUOT,
 * 2 ENOENT, 3 EACCES, 4 EPERM, 5 EROFS, 6 ENOTDIR; -1 for one this OS does
 * not have. Platform reason: per-OS constants. */
int64_t rae_ext_rae_sys_errno_constant(int64_t which) {
    switch (which) {
    case 0: return ENOSPC;
#ifdef EDQUOT
    case 1: return EDQUOT;
#endif
    case 2: return ENOENT;
    case 3: return EACCES;
    case 4: return EPERM;
#ifdef EROFS
    case 5: return EROFS;
#endif
    case 6: return ENOTDIR;
    default: return -1;
    }
}

/* The whole content, closed: false on any failure, the errno kept. fclose is
 * checked too: a full disk often shows only when the buffered bytes are
 * flushed there. */
rae_Bool rae_ext_rae_sys_write_file(rae_String path, rae_String content) {
  if (!path.data || !content.data) { rae_file_errno_set(EINVAL); return false; }
  FILE* f = fopen((const char*)path.data, "wb");
  if (!f) { rae_file_errno_set(errno); return false; }
  size_t written = fwrite(content.data, 1, (size_t)content.len, f);
  int writeErrno = written == (size_t)content.len ? 0 : (errno ? errno : EIO);
  int closed = fclose(f);
  int closeErrno = closed == 0 ? 0 : errno;
  rae_file_errno_set(writeErrno ? writeErrno : closeErrno);
  return writeErrno == 0 && closed == 0;
}

rae_Bool rae_ext_rae_sys_rename(rae_String oldPath, rae_String newPath) {
    if (!oldPath.data || !newPath.data) { rae_file_errno_set(EINVAL); return false; }
    int ok = rename((const char*)oldPath.data, (const char*)newPath.data) == 0;
    rae_file_errno_set(ok ? 0 : errno);
    return ok;
}

rae_Bool rae_ext_rae_sys_delete(rae_String path) {
    if (!path.data) { rae_file_errno_set(EINVAL); return false; }
    int ok = remove((const char*)path.data) == 0;
    rae_file_errno_set(ok ? 0 : errno);
    return ok;
}

#if defined(__wasm__) || defined(__EMSCRIPTEN__) || defined(_WIN32)
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#else
#include <sys/statvfs.h>
#endif

/* The bytes free for this program on the volume `path` is on, or -1 when it
 * cannot be read. Platform reason: one OS call per platform. Apple: the
 * capacity for important use (NSURLVolumeAvailableCapacityForImportantUsageKey,
 * which counts what the OS would purge for it, as Settings' "available" does);
 * Windows: GetDiskFreeSpaceExW's bytes free to the caller; other POSIX:
 * statvfs f_bavail * f_frsize; the web: -1. */
int64_t rae_ext_rae_sys_free_space(rae_String path) {
    if (!path.data) return -1;
#if defined(__wasm__) || defined(__EMSCRIPTEN__)
    return -1;
#elif defined(__APPLE__)
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8*)path.data, (CFIndex)path.len, true);
    if (!url) return -1;
    int64_t bytes = -1;
    CFNumberRef number = NULL;
    if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeAvailableCapacityForImportantUsageKey,
                                        &number, NULL) && number) {
        if (!CFNumberGetValue(number, kCFNumberSInt64Type, &bytes)) bytes = -1;
    }
    if (number) CFRelease(number);
    CFRelease(url);
    return bytes;
#elif defined(_WIN32)
    int wide = MultiByteToWideChar(CP_UTF8, 0, (const char*)path.data, (int)path.len, NULL, 0);
    if (wide <= 0) return -1;
    wchar_t* widePath = (wchar_t*)calloc((size_t)wide + 1, sizeof(wchar_t));
    if (!widePath) return -1;
    MultiByteToWideChar(CP_UTF8, 0, (const char*)path.data, (int)path.len, widePath, wide);
    ULARGE_INTEGER available;
    BOOL ok = GetDiskFreeSpaceExW(widePath, &available, NULL, NULL);
    free(widePath);
    return ok ? (int64_t)available.QuadPart : -1;
#else
    struct statvfs volume;
    if (statvfs((const char*)path.data, &volume) != 0) return -1;
    return (int64_t)volume.f_bavail * (int64_t)volume.f_frsize;
#endif
}

/* Private files (lib/Sys.rae writeFilePrivate, setFileMode, fileMode). Each
 * shim is ONE system call answering its result or a negated errno; the
 * temporary name, the retries and the rename are Rae. Platform reason:
 * open(2) with a mode is variadic, and mode bits and errno are per-OS. */

/* open(path, O_WRONLY | O_CREAT | O_EXCL, 0600): a NEW file only its owner
 * can read or write (the umask can only take bits away). The descriptor, or
 * -errno (-EEXIST when the path exists). */
int64_t rae_ext_rae_sys_open_private(rae_String path) {
    if (!path.data) return -EINVAL;
    int fd = open((const char*)path.data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    rae_file_errno_set(fd >= 0 ? 0 : errno);
    return fd >= 0 ? (int64_t)fd : -(int64_t)errno;
}

/* write(fd, text + offset, count): the bytes written, or -errno */
int64_t rae_ext_rae_sys_write_fd(int64_t fd, rae_String text, int64_t offset, int64_t count) {
    if (count <= 0) return 0;
    if (!text.data || offset < 0 || offset + count > text.len) return -EINVAL;
    ssize_t n = write((int)fd, text.data + offset, (size_t)count);
    rae_file_errno_set(n >= 0 ? 0 : errno);
    return n >= 0 ? (int64_t)n : -(int64_t)errno;
}

/* fsync(fd): 0 or -errno */
int64_t rae_ext_rae_sys_fsync(int64_t fd) {
    int ok = fsync((int)fd) == 0;
    rae_file_errno_set(ok ? 0 : errno);
    return ok ? 0 : -(int64_t)errno;
}

/* close(fd): 0 or -errno */
int64_t rae_ext_rae_sys_close_fd(int64_t fd) {
    int ok = close((int)fd) == 0;
    rae_file_errno_set(ok ? 0 : errno);
    return ok ? 0 : -(int64_t)errno;
}

/* chmod(path, mode): 0 or -errno */
int64_t rae_ext_rae_sys_chmod(rae_String path, int64_t mode) {
    if (!path.data) return -EINVAL;
#ifdef __wasm__
    (void)mode;
    return -ENOSYS;
#else
    return chmod((const char*)path.data, (mode_t)mode) == 0 ? 0 : -(int64_t)errno;
#endif
}

/* stat(path).st_mode's permission bits (07777), or -errno */
int64_t rae_ext_rae_sys_file_mode(rae_String path) {
    if (!path.data) return -EINVAL;
    struct stat st;
    if (stat((const char*)path.data, &st) != 0) return -(int64_t)errno;
    return (int64_t)(st.st_mode & 07777);
}

rae_Bool rae_ext_rae_sys_make_dir(rae_String path) {
    if (!path.data) return false;
    // mkdir(2) returns -1 with EEXIST when the dir already exists; we
    // treat that as success so callers can use this idempotently.
    if (mkdir((const char*)path.data, 0755) == 0 || errno == EEXIST) {
        rae_file_errno_set(0);
        return true;
    }
    rae_file_errno_set(errno);
    return false;
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

/* The same for a double (lib/Json jbNumber64): the shortest of %.1g .. %.17g
 * that strtod reads back to the same double, so an epoch time in ms or any
 * Float64 survives a save and reload unchanged. */
rae_String rae_ext_json_number64(double v) {
  char best[40]; int best_len = -1;
  for (int prec = 1; prec <= 17; prec++) {
    char tmp[40];
    int len = snprintf(tmp, sizeof tmp, "%.*g", prec, v);
    if (len > 0 && len < (int)sizeof tmp && strtod(tmp, NULL) == v) {
      best_len = len; memcpy(best, tmp, (size_t)len + 1);
      break;
    }
  }
  if (best_len < 0) best_len = snprintf(best, sizeof best, "%.17g", v);
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
