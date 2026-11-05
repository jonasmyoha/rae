#include "rae_package_native.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag.h"
#include "rae_deps.h"
#include "rae_toolchain.h"
#include "raepack.h"

/* How long to wait for another process's build of the same package */
#define RAE_PACKAGE_LOCK_TRIES 720
#define RAE_PACKAGE_LOCK_WAIT_SEC 5

static bool path_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* A git dep's pin is its `git-tree` hash. A path dep has no commit to hash, and changes as it is
 * worked on, so its pin is a hash of its files. */
static bool package_pin(const char* dir, const char* hash, bool is_git, char* out, size_t out_len) {
  const char* tree = hash ? strchr(hash, ':') : NULL;
  if (is_git && tree && tree[1]) {
    snprintf(out, out_len, "%.12s", tree + 1);
    return true;
  }
  char command[PATH_MAX + 256];
  snprintf(command, sizeof(command),
           "cd \"%s\" 2>/dev/null && find . -type f -not -path './.rae/*' | LC_ALL=C sort | xargs shasum | shasum",
           dir);
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  size_t got = fread(out, 1, out_len - 1, pipe);
  out[got] = '\0';
  bool ok = pclose(pipe) == 0 && got >= 12;
  if (ok) out[12] = '\0';
  return ok;
}

static bool run_build(const RaeDepNative* native, const char* folder, const char* install) {
  char log[PATH_MAX + 16];
  snprintf(log, sizeof(log), "%s.log", folder);
  fprintf(stderr, "@@RAE_BUILD_PROGRESS@@ building package %s in %s (the first time only)\n", native->name, folder);
  setenv("RAE_PACKAGE_CACHE", folder, 1);
  setenv("RAE_PACKAGE_INSTALL", install, 1);
  char command[3 * PATH_MAX];
  snprintf(command, sizeof(command), "cd \"%s\" && bash \"%s\" > \"%s\" 2>&1", native->dir, native->build, log);
  bool ok = system(command) == 0;
  unsetenv("RAE_PACKAGE_CACHE");
  unsetenv("RAE_PACKAGE_INSTALL");
  if (!ok) {
    snprintf(command, sizeof(command), "tail -20 \"%s\" >&2", log);
    if (system(command) != 0) { /* the log is only a help */ }
    fprintf(stderr, "error: the build of package %s failed (log: %s)\n", native->name, log);
  }
  return ok;
}

bool rae_package_native_install(int index, char* out, size_t out_len) {
  RaeDepNative native;
  if (!rae_deps_native_at(index, &native)) return false;
  char pin[128];
  if (!package_pin(native.dir, native.hash, native.is_git, pin, sizeof(pin))) {
    fprintf(stderr, "error: could not hash package %s in %s\n", native.name, native.dir);
    return false;
  }
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  char folder[2 * PATH_MAX];
  snprintf(folder, sizeof(folder), "%s/packages/%s/%s", home, native.name, pin);
  snprintf(out, out_len, "%s/install", folder);
  char ready[2 * PATH_MAX + 16];
  snprintf(ready, sizeof(ready), "%s/.ready", folder);
  if (path_exists(ready)) return true;
  char command[2 * PATH_MAX + 16];
  snprintf(command, sizeof(command), "mkdir -p \"%s\"", folder);
  if (system(command) != 0) {
    fprintf(stderr, "error: could not make %s\n", folder);
    return false;
  }
  /* One build at a time: two builds of a project may start together */
  char lock[2 * PATH_MAX + 16];
  snprintf(lock, sizeof(lock), "%s.lock", folder);
  for (int tries = 0; mkdir(lock, 0777) != 0; ++tries) {
    if (tries >= RAE_PACKAGE_LOCK_TRIES) {
      fprintf(stderr, "error: %s is still held; remove it if nothing is running\n", lock);
      return false;
    }
    sleep(RAE_PACKAGE_LOCK_WAIT_SEC);
    if (path_exists(ready)) return true;
  }
  bool ok = path_exists(ready) || run_build(&native, folder, out);
  if (ok) {
    FILE* marker = fopen(ready, "w");
    if (marker) fclose(marker);
    ok = marker != NULL;
  }
  rmdir(lock);
  return ok;
}

bool rae_package_native_prepare_all(void) {
  for (int i = 0; i < rae_deps_count(); ++i) {
    RaeDepNative native;
    if (!rae_deps_native_at(i, &native)) continue;
    char install[3 * PATH_MAX];
    if (!rae_package_native_install(i, install, sizeof(install))) return false;
  }
  return true;
}

bool rae_package_native_print(void) {
  for (int i = 0; i < rae_deps_count(); ++i) {
    RaeDepNative native;
    if (!rae_deps_native_at(i, &native)) continue;
    char install[3 * PATH_MAX];
    if (!rae_package_native_install(i, install, sizeof(install))) return false;
    printf("%s\t%s\t%s\t%s\t%s\n", native.name, native.header, install, native.sources_device, native.defines_device);
  }
  return true;
}

/* The first pack in `dir`, as a package has one at its root */
static bool pack_in_folder(const char* dir, char* out, size_t out_len) {
  DIR* folder = opendir(dir);
  if (!folder) return false;
  bool found = false;
  struct dirent* entry;
  while (!found && (entry = readdir(folder)) != NULL) {
    size_t n = strlen(entry->d_name);
    if (n > 8 && strcmp(entry->d_name + n - 8, ".raepack") == 0) {
      snprintf(out, out_len, "%s/%s", dir, entry->d_name);
      found = true;
    }
  }
  closedir(folder);
  return found;
}

void rae_package_native_keep(const char* pack_path, char* keep, size_t keep_cap, size_t* keep_len, int depth) {
  if (depth > 16) return;
  RaePack pack;
  memset(&pack, 0, sizeof(pack));
  diag_set_quiet(true);
  bool parsed = raepack_parse_library_file(pack_path, &pack);
  diag_set_quiet(false);
  if (!parsed) {
    raepack_free(&pack);
    return;
  }
  char pack_dir[PATH_MAX];
  snprintf(pack_dir, sizeof(pack_dir), "%s", pack_path);
  char* slash = strrchr(pack_dir, '/');
  if (slash) *slash = '\0';
  else snprintf(pack_dir, sizeof(pack_dir), ".");
  for (const RaePackDep* dep = pack.deps; dep; dep = dep->next) {
    bool is_git = dep->git.len > 0;
    char dir[2 * PATH_MAX];
    if (is_git) {
      snprintf(dir, sizeof(dir), "%s/.rae/deps/%.*s%s%.*s", pack_dir, (int)dep->name.len, dep->name.data,
               dep->subdir.len ? "/" : "", (int)dep->subdir.len, dep->subdir.data);
    } else if (dep->path.len > 0 && dep->path.data[0] == '/') {
      snprintf(dir, sizeof(dir), "%.*s", (int)dep->path.len, dep->path.data);
    } else {
      snprintf(dir, sizeof(dir), "%s/%.*s", pack_dir, (int)dep->path.len, dep->path.data);
    }
    char hash[128];
    snprintf(hash, sizeof(hash), "%.*s", (int)dep->hash.len, dep->hash.data);
    char pin[128];
    if (package_pin(dir, hash, is_git, pin, sizeof(pin)) && *keep_len + 256 < keep_cap) {
      *keep_len += (size_t)snprintf(keep + *keep_len, keep_cap - *keep_len, "/%.*s/%s\n", (int)dep->name.len,
                                    dep->name.data, pin);
    }
    char own_pack[3 * PATH_MAX];
    if (pack_in_folder(dir, own_pack, sizeof(own_pack))) {
      rae_package_native_keep(own_pack, keep, keep_cap, keep_len, depth + 1);
    }
  }
  raepack_free(&pack);
}

int rae_package_native_gc(const char* home, const char* keep, bool dry_run) {
  char root[PATH_MAX + 16];
  snprintf(root, sizeof(root), "%s/packages", home);
  DIR* names = opendir(root);
  if (!names) return 0;
  int removed = 0;
  struct dirent* name;
  while ((name = readdir(names)) != NULL) {
    if (name->d_name[0] == '.') continue;
    char name_dir[2 * PATH_MAX];
    snprintf(name_dir, sizeof(name_dir), "%s/%s", root, name->d_name);
    DIR* pins = opendir(name_dir);
    if (!pins) continue;
    struct dirent* pin;
    while ((pin = readdir(pins)) != NULL) {
      size_t n = strlen(pin->d_name);
      if (pin->d_name[0] == '.' || (n > 4 && strcmp(pin->d_name + n - 4, ".log") == 0) ||
          (n > 5 && strcmp(pin->d_name + n - 5, ".lock") == 0)) {
        continue;
      }
      char wanted[512];
      snprintf(wanted, sizeof(wanted), "/%s/%s\n", name->d_name, pin->d_name);
      char folder[3 * PATH_MAX];
      snprintf(folder, sizeof(folder), "%s/%s", name_dir, pin->d_name);
      char lock[3 * PATH_MAX + 8];
      snprintf(lock, sizeof(lock), "%s.lock", folder);
      if (strstr(keep, wanted) || path_exists(lock)) continue;
      removed += 1;
      printf("%s package %s/%s\n", dry_run ? "would remove" : "removing", name->d_name, pin->d_name);
      if (dry_run) continue;
      char command[7 * PATH_MAX];
      snprintf(command, sizeof(command), "rm -rf \"%s\" \"%s.log\"", folder, folder);
      if (system(command) != 0) fprintf(stderr, "warning: could not remove %s\n", folder);
    }
    closedir(pins);
    if (!dry_run) rmdir(name_dir); /* only an empty one goes */
  }
  closedir(names);
  return removed;
}
