/* Console, process, environment, and filesystem primitives. Raw OS calls stay C; path/convenience policy can migrate to Rae.
 *
 * Split from rae_runtime.c by runtime migration task #288.
 * This module is included by rae_runtime.c into one translation unit.
 * No behavior or ABI changes are intended here.
 */

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
