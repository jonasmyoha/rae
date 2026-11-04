#include "rae_package_native.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rae_deps.h"
#include "rae_toolchain.h"

/* How long to wait for another process's build of the same package */
#define RAE_PACKAGE_LOCK_TRIES 720
#define RAE_PACKAGE_LOCK_WAIT_SEC 5

static bool path_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* A git dep's pin is its `git-tree` hash. A path dep has no commit to hash, and changes as it is
 * worked on, so its pin is a hash of its files. */
static bool package_pin(const RaeDepNative* native, char* out, size_t out_len) {
  const char* tree = strchr(native->hash, ':');
  if (native->is_git && tree && tree[1]) {
    snprintf(out, out_len, "%.12s", tree + 1);
    return true;
  }
  char command[PATH_MAX + 256];
  snprintf(command, sizeof(command),
           "cd \"%s\" && find . -type f -not -path './.rae/*' | LC_ALL=C sort | xargs shasum | shasum",
           native->dir);
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
  if (!package_pin(&native, pin, sizeof(pin))) {
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
