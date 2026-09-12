#include "rae_deps.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  char name[128];
  char dir[PATH_MAX];
  char source[PATH_MAX + 8];  /* "path:..." or "git:..." as written in the pack */
  char rev[128];
  char commit[64];
  char tree[64];
  bool is_git;
} ResolvedDep;

#define RAE_DEPS_MAX 64
static ResolvedDep s_deps[RAE_DEPS_MAX];
static int s_dep_count = 0;

/* #934 `rae update`: NULL name + s_update_all=false => honor the lock (normal
 * builds, `rae fetch`). s_update_all => re-resolve every dep; else only the
 * one named in s_update_name. */
static bool s_update_all = false;
static char s_update_name[128] = {0};

void rae_deps_set_update(const char* only_name) {
  if (only_name) {
    s_update_all = false;
    snprintf(s_update_name, sizeof(s_update_name), "%s", only_name);
  } else {
    s_update_all = true;
    s_update_name[0] = '\0';
  }
}

static bool dep_is_updating(const char* name) {
  if (s_update_all) return true;
  return s_update_name[0] && strcmp(s_update_name, name) == 0;
}

static bool dir_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void str_copy(char* out, size_t out_len, Str value) {
  size_t n = value.len < out_len - 1 ? value.len : out_len - 1;
  memcpy(out, value.data, n);
  out[n] = '\0';
}

/* Run `git -C <dir> <args>`; capture trimmed stdout. Returns exit success. */
static bool git_in(const char* dir, const char* args, char* out, size_t out_len) {
  char cmd[2 * PATH_MAX + 256];
  snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s 2>/dev/null", dir, args);
  FILE* pipe = popen(cmd, "r");
  if (!pipe) return false;
  size_t n = out ? fread(out, 1, out_len - 1, pipe) : 0;
  if (out) {
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
  }
  return pclose(pipe) == 0;
}

/* Read the pinned commit for git dep `name` out of an existing rae.lock:
 * a line `dep <name> { ... commit: "<hex>" ... }`. False when absent. */
static bool lock_pinned_commit(const char* lock_path, const char* name, char* out,
                               size_t out_len) {
  FILE* file = lock_path ? fopen(lock_path, "r") : NULL;
  if (!file) return false;
  char prefix[160];
  snprintf(prefix, sizeof(prefix), "dep %s ", name);
  char line[4096];
  bool found = false;
  while (fgets(line, sizeof(line), file)) {
    if (strncmp(line, prefix, strlen(prefix)) != 0) continue;
    const char* key = strstr(line, "commit: \"");
    if (!key) break;
    key += strlen("commit: \"");
    const char* end = strchr(key, '"');
    if (!end || (size_t)(end - key) >= out_len) break;
    memcpy(out, key, (size_t)(end - key));
    out[end - key] = '\0';
    found = out[0] != '\0';
    break;
  }
  fclose(file);
  return found;
}

static bool resolve_path_dep(ResolvedDep* dep, const RaePackDep* spec, const char* pack_dir) {
  char rel[PATH_MAX];
  str_copy(rel, sizeof(rel), spec->path);
  char joined[PATH_MAX];
  if (rel[0] == '/') snprintf(joined, sizeof(joined), "%s", rel);
  else snprintf(joined, sizeof(joined), "%s/%s", pack_dir, rel);
  if (!realpath(joined, dep->dir) || !dir_exists(dep->dir)) {
    fprintf(stderr, "error: dependency '%s': path \"%s\" (%s) is not a directory\n",
            dep->name, rel, joined);
    return false;
  }
  snprintf(dep->source, sizeof(dep->source), "path:%s", rel);
  return true;
}

static bool resolve_git_dep(ResolvedDep* dep, const RaePackDep* spec, const char* pack_dir,
                            const char* lock_path) {
  char url[PATH_MAX];
  str_copy(url, sizeof(url), spec->git);
  str_copy(dep->rev, sizeof(dep->rev), spec->rev);
  snprintf(dep->source, sizeof(dep->source), "git:%s", url);
  snprintf(dep->dir, sizeof(dep->dir), "%s/.rae/deps/%s", pack_dir, dep->name);

  bool updating = dep_is_updating(dep->name);
  char pinned[64] = {0};
  bool have_pin = !updating && lock_pinned_commit(lock_path, dep->name, pinned, sizeof(pinned));

  if (!dir_exists(dep->dir)) {
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/.rae/deps", pack_dir);
    char mk[PATH_MAX + 32];
    snprintf(mk, sizeof(mk), "mkdir -p \"%s\"", parent);
    (void)system(mk);
    char clone[3 * PATH_MAX];
    snprintf(clone, sizeof(clone), "git clone --quiet \"%s\" \"%s\" 2>&1", url, dep->dir);
    if (system(clone) != 0) {
      fprintf(stderr, "error: dependency '%s': could not clone %s\n", dep->name, url);
      return false;
    }
  }

  if (updating) {
    /* Re-resolve within the pack's requirement: pull the remote, then move to
     * the pack's rev (or the default branch tip when no rev is pinned). */
    git_in(dep->dir, "fetch --tags --quiet", NULL, 0);
    git_in(dep->dir, "fetch --quiet", NULL, 0);
    if (!dep->rev[0]) {
      char def[128] = {0};
      if (git_in(dep->dir, "rev-parse --abbrev-ref origin/HEAD", def, sizeof(def)) && def[0]) {
        char args[192];
        snprintf(args, sizeof(args), "checkout --quiet \"%s\"", def);
        git_in(dep->dir, args, NULL, 0);
      }
    }
  }

  /* The lock's commit wins (reproducible from the lockfile alone); otherwise
   * the pack's rev; otherwise whatever the clone checked out. */
  const char* target = have_pin ? pinned : (dep->rev[0] ? dep->rev : NULL);
  if (target) {
    char head[64] = {0};
    git_in(dep->dir, "rev-parse HEAD", head, sizeof(head));
    bool already = have_pin && strcmp(head, pinned) == 0;
    if (!already) {
      char args[256];
      snprintf(args, sizeof(args), "checkout --quiet \"%s\"", target);
      if (!git_in(dep->dir, args, NULL, 0)) {
        /* A rev that only exists upstream (new tag) needs a fetch first. */
        git_in(dep->dir, "fetch --tags --quiet", NULL, 0);
        if (!git_in(dep->dir, args, NULL, 0)) {
          fprintf(stderr, "error: dependency '%s': revision \"%s\" not found in %s\n",
                  dep->name, target, dep->dir);
          return false;
        }
      }
    }
  }
  if (!git_in(dep->dir, "rev-parse HEAD", dep->commit, sizeof(dep->commit)) ||
      !git_in(dep->dir, "rev-parse 'HEAD^{tree}'", dep->tree, sizeof(dep->tree))) {
    fprintf(stderr, "error: dependency '%s': %s is not a git checkout\n", dep->name, dep->dir);
    return false;
  }
  return true;
}

bool rae_deps_resolve(const RaePack* pack, const char* pack_dir_in, const char* lock_path) {
  /* Absolute dep dirs: the module loader reads a leading `.` in a file path
   * (`./.rae/deps/x/Foo.rae`) as a relative import spec, and the lock should
   * not depend on the cwd the build was started from. */
  char pack_dir[PATH_MAX];
  if (!realpath(pack_dir_in, pack_dir)) snprintf(pack_dir, sizeof(pack_dir), "%s", pack_dir_in);
  for (const RaePackDep* spec = pack ? pack->deps : NULL; spec; spec = spec->next) {
    if (s_dep_count >= RAE_DEPS_MAX) {
      fprintf(stderr, "error: too many dependencies (max %d)\n", RAE_DEPS_MAX);
      return false;
    }
    ResolvedDep* dep = &s_deps[s_dep_count];
    memset(dep, 0, sizeof(*dep));
    str_copy(dep->name, sizeof(dep->name), spec->name);
    dep->is_git = spec->git.len > 0;
    bool ok = dep->is_git ? resolve_git_dep(dep, spec, pack_dir, lock_path)
                          : resolve_path_dep(dep, spec, pack_dir);
    if (!ok) return false;
    s_dep_count += 1;
  }
  return true;
}

void rae_deps_write_lock(FILE* out, const char* stdlib_dir) {
  /* The standard library is the first dependency: resolved through the
   * toolchain the lock's `toolchain` block names, not a project-local copy. */
  fprintf(out, "dep lib { source: \"toolchain:%s\" }\n", stdlib_dir ? stdlib_dir : "");
  for (int i = 0; i < s_dep_count; ++i) {
    const ResolvedDep* dep = &s_deps[i];
    if (!dep->is_git) {
      fprintf(out, "dep %s { source: \"%s\" }\n", dep->name, dep->source);
      continue;
    }
    fprintf(out, "dep %s { source: \"%s\"", dep->name, dep->source);
    if (dep->rev[0]) fprintf(out, " rev: \"%s\"", dep->rev);
    /* git-tree: the content-addressed tree object id — a content hash of the
     * checked-out files, computed by git rather than a second hasher here. */
    fprintf(out, " commit: \"%s\" hash: \"git-tree:%s\" }\n", dep->commit, dep->tree);
  }
}

int rae_deps_count(void) { return s_dep_count; }

static const ResolvedDep* lookup(const char* normalized, const char** rest_out) {
  const char* slash = strchr(normalized, '/');
  size_t head_len = slash ? (size_t)(slash - normalized) : strlen(normalized);
  for (int i = 0; i < s_dep_count; ++i) {
    if (strlen(s_deps[i].name) == head_len && strncmp(s_deps[i].name, normalized, head_len) == 0) {
      *rest_out = slash ? slash + 1 : "";
      return &s_deps[i];
    }
  }
  return NULL;
}

char* rae_deps_resolve_module_file(const char* normalized) {
  const char* rest = NULL;
  const ResolvedDep* dep = lookup(normalized, &rest);
  if (!dep || rest[0] == '\0') return NULL;
  size_t total = strlen(dep->dir) + strlen(rest) + 6;
  char* buffer = malloc(total);
  if (!buffer) return NULL;
  snprintf(buffer, total, "%s/%s.rae", dep->dir, rest);
  struct stat st;
  if (stat(buffer, &st) == 0 && S_ISREG(st.st_mode)) return buffer;
  free(buffer);
  return NULL;
}

char* rae_deps_resolve_folder(const char* normalized) {
  const char* rest = NULL;
  const ResolvedDep* dep = lookup(normalized, &rest);
  if (!dep) return NULL;
  size_t total = strlen(dep->dir) + strlen(rest) + 2;
  char* buffer = malloc(total);
  if (!buffer) return NULL;
  if (rest[0]) snprintf(buffer, total, "%s/%s", dep->dir, rest);
  else snprintf(buffer, total, "%s", dep->dir);
  if (dir_exists(buffer)) return buffer;
  free(buffer);
  return NULL;
}

bool rae_deps_owns_path(const char* file_path) {
  if (!file_path || s_dep_count == 0) return false;
  char real[PATH_MAX];
  const char* probe = realpath(file_path, real) ? real : file_path;
  for (int i = 0; i < s_dep_count; ++i) {
    size_t n = strlen(s_deps[i].dir);
    if (strncmp(probe, s_deps[i].dir, n) == 0 && (probe[n] == '/' || probe[n] == '\0')) return true;
  }
  return false;
}

void rae_deps_print_tree(FILE* out, const char* root_label, const char* stdlib_dir) {
  fprintf(out, "%s\n", root_label && root_label[0] ? root_label : "(project)");
  int total = s_dep_count + 1;  /* +1 for the implicit stdlib edge */
  fprintf(out, "%s lib  (stdlib via toolchain: %s)\n",
          total == 1 ? "\xe2\x94\x94\xe2\x94\x80" : "\xe2\x94\x9c\xe2\x94\x80",
          stdlib_dir && stdlib_dir[0] ? stdlib_dir : "(unresolved)");
  for (int i = 0; i < s_dep_count; ++i) {
    const ResolvedDep* dep = &s_deps[i];
    const char* branch = (i == s_dep_count - 1) ? "\xe2\x94\x94\xe2\x94\x80"
                                                : "\xe2\x94\x9c\xe2\x94\x80";
    if (!dep->is_git) {
      fprintf(out, "%s %s  %s\n", branch, dep->name, dep->source);
    } else {
      char shortc[13] = {0};
      snprintf(shortc, sizeof(shortc), "%.12s", dep->commit);
      if (dep->rev[0]) {
        fprintf(out, "%s %s  %s @ %s (%s)\n", branch, dep->name, dep->source, dep->rev, shortc);
      } else {
        fprintf(out, "%s %s  %s (%s)\n", branch, dep->name, dep->source, shortc);
      }
    }
  }
}
