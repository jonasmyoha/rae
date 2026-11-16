/* module_folder — a module is a PascalCase file or a PascalCase folder
 * (docs/module-model-options.md).
 *
 * `import Gpu2d` finds lib/Gpu2d.rae or the folder lib/Gpu2d/, whose files (and those of its
 * camelCase subfolders) are one module: the loader gives each its own internal path and the one
 * module name. A PascalCase subfolder is a module of its own. A file and a folder of one name in
 * one place is an error, so adding a folder never moves a file's functions to another module. */
#include "module_folder.h"
#include "rae_deps.h"
#include "diag.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool module_folder_name_is_module(const char* name, size_t length) {
  return length > 0 && name[0] >= 'A' && name[0] <= 'Z';
}

static bool path_is_dir(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool path_is_file(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Whether `dir` has an entry spelled exactly `name` (macOS and Windows ignore case in stat, so
 * `lib/Sys` would otherwise find the folder `lib/sys/`) */
static bool has_exact_entry(const char* dir, const char* name) {
  DIR* handle = opendir(dir);
  if (!handle) return false;
  bool found = false;
  struct dirent* entry;
  while (!found && (entry = readdir(handle)) != NULL) found = strcmp(entry->d_name, name) == 0;
  closedir(handle);
  return found;
}

/* `path` is a folder (want_dir) or a file, and its last name is spelled exactly as written */
static bool exists_exact(const char* path, bool want_dir) {
  if (want_dir ? !path_is_dir(path) : !path_is_file(path)) return false;
  char parent[PATH_MAX];
  snprintf(parent, sizeof parent, "%s", path);
  char* slash = strrchr(parent, '/');
  const char* name = path;
  if (slash) {
    *slash = '\0';
    name = slash + 1;
    if (!parent[0]) snprintf(parent, sizeof parent, "/");
  } else {
    snprintf(parent, sizeof parent, ".");
  }
  return has_exact_entry(parent, name);
}

static const char* last_segment(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* 1 folder, 0 file, -1 both, 2 neither */
static int look_in(const char* base,
                   const char* relative,
                   char* dir_out,
                   size_t dir_cap,
                   char* error,
                   size_t error_cap) {
  if (!base || !base[0]) return 2;
  char file[PATH_MAX];
  char dir[PATH_MAX];
  snprintf(file, sizeof file, "%s/%s.rae", base, relative);
  snprintf(dir, sizeof dir, "%s/%s", base, relative);
  bool has_file = exists_exact(file, false);
  bool has_dir = exists_exact(dir, true);
  const char* name = last_segment(relative);
  if (has_file && has_dir && module_folder_name_is_module(name, strlen(name))) {
    snprintf(error, error_cap,
             "'%s' is both a file and a folder (%s and %s/): a module is one of them, so remove the "
             "file or move it into the folder", relative, diag_simplify_path(file), diag_simplify_path(dir));
    return -1;
  }
  if (has_file) return 0;
  if (has_dir && module_folder_name_is_module(name, strlen(name))) {
    snprintf(dir_out, dir_cap, "%s", dir);
    return 1;
  }
  return 2;
}

int module_folder_resolve(const char* normalized,
                          const char* root,
                          const char* stdlib,
                          char* dir_out,
                          size_t dir_cap,
                          char* error,
                          size_t error_cap) {
  const char* rest = NULL;
  const char* dep_dir = rae_deps_dir_for(normalized, &rest);
  if (dep_dir && rest && rest[0]) {
    int found = look_in(dep_dir, rest, dir_out, dir_cap, error, error_cap);
    if (found != 2) return found < 0 ? -1 : found;
  }
  char lib[PATH_MAX];
  const char* bases[5] = { root, NULL, "../lib", "lib", stdlib };
  if (root) {
    snprintf(lib, sizeof lib, "%s/lib", root);
    bases[1] = lib;
  }
  for (int i = 0; i < 5; i++) {
    int found = look_in(bases[i], normalized, dir_out, dir_cap, error, error_cap);
    if (found != 2) return found < 0 ? -1 : found;
  }
  return 0;
}

static int compare_names(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static bool add_file(ModuleFolderFiles* out, const char* path, const char* stem) {
  if (out->count == out->capacity) {
    int capacity = out->capacity ? out->capacity * 2 : 16;
    ModuleFolderFile* grown = realloc(out->items, sizeof(ModuleFolderFile) * (size_t)capacity);
    if (!grown) return false;
    out->items = grown;
    out->capacity = capacity;
  }
  snprintf(out->items[out->count].path, sizeof out->items[out->count].path, "%s", path);
  snprintf(out->items[out->count].stem, sizeof out->items[out->count].stem, "%s", stem);
  out->count++;
  return true;
}

static bool list_into(const char* dir,
                      const char* prefix,
                      ModuleFolderFiles* out,
                      char* error,
                      size_t error_cap) {
  DIR* handle = opendir(dir);
  if (!handle) return true;
  char* names[4096];
  int count = 0;
  struct dirent* entry;
  while ((entry = readdir(handle)) != NULL && count < 4096) {
    if (entry->d_name[0] == '.') continue;
    names[count++] = strdup(entry->d_name);
  }
  closedir(handle);
  qsort(names, (size_t)count, sizeof names[0], compare_names);
  bool ok = true;
  for (int i = 0; i < count && ok; i++) {
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s/%s", dir, names[i]);
    size_t length = strlen(names[i]);
    if (path_is_dir(full)) {
      /* a file of the same name beside this folder: rule 1 */
      char file[PATH_MAX];
      snprintf(file, sizeof file, "%s/%s.rae", dir, names[i]);
      if (exists_exact(file, false) && module_folder_name_is_module(names[i], length)) {
        snprintf(error, error_cap,
                 "'%s' is both a file and a folder (%s and %s/): a module is one of them, and a "
                 "folder beside a file of its name would move that file's functions to another "
                 "module", names[i], diag_simplify_path(file), diag_simplify_path(full));
        ok = false;
        break;
      }
      if (module_folder_name_is_module(names[i], length)) continue;  /* a module of its own */
      char inner[512];
      snprintf(inner, sizeof inner, "%s%s/", prefix, names[i]);
      ok = list_into(full, inner, out, error, error_cap);
      continue;
    }
    if (length <= 4 || strcmp(names[i] + length - 4, ".rae") != 0 || !path_is_file(full)) continue;
    char stem[512];
    snprintf(stem, sizeof stem, "%s%.*s", prefix, (int)(length - 4), names[i]);
    if (!add_file(out, full, stem)) {
      snprintf(error, error_cap, "out of memory while listing %s", dir);
      ok = false;
    }
  }
  for (int i = 0; i < count; i++) free(names[i]);
  return ok;
}

bool module_folder_list(const char* dir, ModuleFolderFiles* out, char* error, size_t error_cap) {
  out->items = NULL;
  out->count = 0;
  out->capacity = 0;
  return list_into(dir, "", out, error, error_cap);
}

void module_folder_files_free(ModuleFolderFiles* files) {
  free(files->items);
  files->items = NULL;
  files->count = 0;
  files->capacity = 0;
}

bool module_folder_owner(const char* normalized, char* module_out, size_t cap) {
  const char* segment = normalized;
  const char* owner_end = NULL;
  for (;;) {
    const char* slash = strchr(segment, '/');
    if (!slash) break;  /* the last segment is the file itself */
    if (module_folder_name_is_module(segment, (size_t)(slash - segment))) owner_end = slash;
    segment = slash + 1;
  }
  if (!owner_end) return false;
  snprintf(module_out, cap, "%.*s", (int)(owner_end - normalized), normalized);
  return true;
}
