/* The C runtime's object cache (runtime_cache.h).
 *
 * Why: a native `rae run` / `rae build` used to pass the generated program
 * AND the whole C runtime to one gcc command, so every build compiled the
 * runtime from source again. The runtime is the same for every program built
 * with the same flags, and at -O2 it is most of a small program's C compile:
 * 1.5 s of 1.6 s for a 27-line test case (its own C takes 0.1 s, the link
 * 0.1 s). The test suite does that ~800 times.
 *
 * So the runtime is compiled once into
 *   $XDG_CACHE_HOME/rae/runtime/<key>.o   (default ~/.cache/rae/runtime)
 * and linked from there. The key hashes (FNV-1a, 64 bits) the compile
 * command and the name and bytes of every file in runtime/: changing a
 * flag, editing the runtime or pulling a new one makes a new key, so a stale
 * object is never linked. The object is written to a temporary file and
 * renamed into place, so builds running in parallel (the test suite) never
 * link a half-written object; two of them compiling the same key at once
 * both produce the same bytes and either rename wins. To start over, delete
 * the directory. */
#include "runtime_cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t fnv1a(uint64_t hash, const void* data, size_t len) {
  const unsigned char* bytes = (const unsigned char*)data;
  for (size_t i = 0; i < len; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

static int compare_names(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* The hash of every regular file directly in `dir`: names in sorted order,
 * each followed by its bytes. false when the directory cannot be read. */
static bool hash_directory(const char* dir, uint64_t* hash) {
  DIR* d = opendir(dir);
  if (!d) return false;
  char* names[512];
  size_t count = 0;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL && count < sizeof(names) / sizeof(names[0])) {
    if (ent->d_name[0] == '.') continue;
    names[count++] = strdup(ent->d_name);
  }
  closedir(d);
  qsort(names, count, sizeof(names[0]), compare_names);
  bool ok = true;
  char path[4096];
  char buffer[65536];
  for (size_t i = 0; i < count; i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
      *hash = fnv1a(*hash, names[i], strlen(names[i]) + 1);
      FILE* file = fopen(path, "rb");
      if (!file) { ok = false; }
      else {
        size_t got;
        while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) *hash = fnv1a(*hash, buffer, got);
        fclose(file);
      }
    }
    free(names[i]);
  }
  return ok;
}

static bool make_directories(const char* path) {
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char* p = tmp + 1; *p; p++) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
    *p = '/';
  }
  return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

bool runtime_cached_object(const char* compile_command,
                           const char* runtime_dir,
                           char* out_path,
                           size_t cap) {
  uint64_t hash = 1469598103934665603ULL;
  hash = fnv1a(hash, compile_command, strlen(compile_command) + 1);
  if (!hash_directory(runtime_dir, &hash)) return false;

  char cache_dir[4096];
  const char* xdg = getenv("XDG_CACHE_HOME");
  if (xdg && *xdg) {
    snprintf(cache_dir, sizeof(cache_dir), "%s/rae/runtime", xdg);
  } else {
    const char* home = getenv("HOME");
    snprintf(cache_dir, sizeof(cache_dir), "%s/.cache/rae/runtime", home ? home : ".");
  }
  snprintf(out_path, cap, "%s/%016llx.o", cache_dir, (unsigned long long)hash);
  struct stat st;
  if (stat(out_path, &st) == 0 && st.st_size > 0) return true;

  if (!make_directories(cache_dir)) return false;
  char temporary[4200];
  snprintf(temporary, sizeof(temporary), "%s.%d.tmp", out_path, (int)getpid());
  char command[16384];
  snprintf(command, sizeof(command), "%s -c %s/rae_runtime.c -o %s", compile_command, runtime_dir, temporary);
  if (system(command) != 0) {
    unlink(temporary);
    return false;
  }
  if (rename(temporary, out_path) != 0) {
    unlink(temporary);
    return false;
  }
  return true;
}
