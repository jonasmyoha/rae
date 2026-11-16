/* import_case — an import's spelling checked against the names on disk
 * (docs/module-model-options.md rule 8).
 *
 * macOS and Windows ignore case in file names, so `import vec3` used to load lib/Vec3.rae there
 * and fail on Linux. Casing carries meaning in Rae's module names, so the compiler compares every
 * segment of an import path with the exact directory entry, on every system, and refuses a
 * mismatch with the spelling that is on disk. */
#include "import_case.h"
#include "rae_deps.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static bool is_dir(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The entry of `dir` named `name` ignoring case, preferring the exact one, as a directory
 * (`want_dir`) or a file; its spelling into `spelled`. */
static bool find_entry(const char* dir, const char* name, bool want_dir, char* spelled, size_t cap) {
  DIR* handle = opendir(dir);
  if (!handle) return false;
  bool found = false;
  struct dirent* entry;
  while ((entry = readdir(handle)) != NULL) {
    if (strcasecmp(entry->d_name, name) != 0) continue;
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", dir, entry->d_name);
    if (want_dir ? !is_dir(full) : !is_file(full)) continue;
    bool exact = strcmp(entry->d_name, name) == 0;
    if (!found || exact) {
      snprintf(spelled, cap, "%s", entry->d_name);
      found = true;
    }
    if (exact) break;
  }
  closedir(handle);
  return found;
}

/* `relative` (a/b/C) under `base`: every folder segment, then `C.rae` or the folder `C`. On a
 * match, writes the on-disk spelling of `relative` into `spelled` (without `.rae`). */
static bool walk(const char* base, const char* relative, char* spelled, size_t cap, bool* last_is_dir) {
  if (!base || !base[0] || !is_dir(base)) return false;
  char dir[PATH_MAX];
  snprintf(dir, sizeof dir, "%s", base);
  size_t out = 0;
  spelled[0] = '\0';
  const char* segment = relative;
  while (*segment) {
    const char* slash = strchr(segment, '/');
    size_t length = slash ? (size_t)(slash - segment) : strlen(segment);
    char name[NAME_MAX + 1];
    if (length == 0 || length > NAME_MAX - 4) return false;
    memcpy(name, segment, length);
    name[length] = '\0';
    char entry[NAME_MAX + 1];
    if (slash) {
      if (!find_entry(dir, name, true, entry, sizeof entry)) return false;
    } else {
      char file[NAME_MAX + 1];
      snprintf(file, sizeof file, "%s.rae", name);
      if (find_entry(dir, file, false, entry, sizeof entry)) {
        entry[strlen(entry) - 4] = '\0';
        *last_is_dir = false;
      } else if (find_entry(dir, name, true, entry, sizeof entry)) {
        *last_is_dir = true;
      } else {
        return false;
      }
    }
    int wrote = snprintf(spelled + out, cap - out, "%s%s", out ? "/" : "", entry);
    if (wrote < 0 || (size_t)wrote >= cap - out) return false;
    out += (size_t)wrote;
    size_t dir_length = strlen(dir);
    snprintf(dir + dir_length, sizeof dir - dir_length, "/%s", entry);
    if (!slash) break;
    segment = slash + 1;
  }
  return true;
}

/* The first segment that differs, as "the file is 'Vec3.rae'" or "the folder is 'ecs'" */
static void describe_difference(const char* written, const char* spelled, bool last_is_dir, char* out, size_t cap) {
  const char* w = written;
  const char* s = spelled;
  for (;;) {
    const char* w_end = strchr(w, '/');
    const char* s_end = strchr(s, '/');
    size_t w_len = w_end ? (size_t)(w_end - w) : strlen(w);
    size_t s_len = s_end ? (size_t)(s_end - s) : strlen(s);
    if (w_len != s_len || strncmp(w, s, w_len) != 0 || !s_end || !w_end) {
      if (s_end || last_is_dir) snprintf(out, cap, "the folder is '%.*s'", (int)s_len, s);
      else snprintf(out, cap, "the file is '%.*s.rae'", (int)s_len, s);
      return;
    }
    w = w_end + 1;
    s = s_end + 1;
  }
}

bool import_case_check(const char* normalized,
                       const char* keyword,
                       const char* root,
                       const char* stdlib,
                       char* message,
                       size_t cap) {
  char spelled[PATH_MAX];
  char prefix[PATH_MAX] = "";
  const char* relative = normalized;
  bool matched = false;
  bool last_is_dir = false;
  /* a dependency owns its import prefix: only the part after it is on disk */
  const char* rest = NULL;
  const char* dep_dir = rae_deps_dir_for(normalized, &rest);
  if (dep_dir && rest && rest[0]) {
    if (walk(dep_dir, rest, spelled, sizeof spelled, &last_is_dir)) {
      snprintf(prefix, sizeof prefix, "%.*s/", (int)(rest - normalized - 1), normalized);
      relative = rest;
      matched = true;
    }
  }
  if (!matched) {
    char lib[PATH_MAX];
    const char* bases[5] = { root, NULL, "../lib", "lib", stdlib };
    if (root) {
      snprintf(lib, sizeof lib, "%s/lib", root);
      bases[1] = lib;
    }
    for (int i = 0; i < 5 && !matched; i++) {
      matched = walk(bases[i], normalized, spelled, sizeof spelled, &last_is_dir);
    }
  }
  if (!matched || strcmp(spelled, relative) == 0) return true;
  char difference[PATH_MAX + 64];
  describe_difference(relative, spelled, last_is_dir, difference, sizeof difference);
  snprintf(message, cap, "no module '%s'; %s: write `%s %s%s` (an import is spelled as on disk, "
           "with the same upper and lower case)", normalized, difference, keyword, prefix, spelled);
  return false;
}
