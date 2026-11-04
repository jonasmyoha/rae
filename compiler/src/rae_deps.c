#include "rae_deps.h"
#include "rae_version.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

typedef struct {
  char name[128];
  char dir[PATH_MAX];
  char source[PATH_MAX + 8];  /* "path:..." or "git:..." as written in the pack */
  char subdir[PATH_MAX];
  char version[64];     /* the exact version in force (rae update moves it) */
  char pin_commit[64];  /* the pack's optional commit; "" for the version's tag */
  char pack_hash[128];  /* the hash the pack holds; "" when it holds none */
  char hash[128];       /* the hash of the files found */
  char commit[64];
  bool is_git;
  bool from_root;       /* declared by the project: its version wins a diamond */
  char declared_by[128];
  /* the deps THIS package's own pack declares (by name): what its files may
   * reach besides themselves and the stdlib */
  char uses[16][128];
  int use_count;
} ResolvedDep;

#define RAE_DEPS_MAX 64
/* The compiler the project builds with: a dependency's `rae` is checked against it */
static char s_compiler_version[64] = {0};
static ResolvedDep s_deps[RAE_DEPS_MAX];
static int s_dep_count = 0;
/* the deps the ROOT project's pack declares (by name) */
static char s_root_uses[RAE_DEPS_MAX][128];
static int s_root_use_count = 0;

/* #934 `rae update`: NULL name + s_update_all=false => hold to the pack (normal
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
  str_copy(dep->version, sizeof(dep->version), spec->version);
  return true;
}

/* The tag of a version: v1.2.0, or with a subdir its last folder first: one/v1.2.0 */
static void version_tag(const ResolvedDep* dep, const char* version, char* out, size_t out_len) {
  const char* folder = strrchr(dep->subdir, '/');
  folder = folder ? folder + 1 : dep->subdir;
  if (folder[0]) snprintf(out, out_len, "%s/v%s", folder, version);
  else snprintf(out, out_len, "v%s", version);
}

/* Is a dep's newest released version newer than the one in force: moves it, for
 * `rae update`. Tags that are not this package's exact versions do not count. */
static void move_to_newest_version(ResolvedDep* dep, const char* clone) {
  char prefix[192];
  version_tag(dep, "", prefix, sizeof(prefix));
  char tags[65536] = {0};
  char args[256];
  snprintf(args, sizeof(args), "tag --list \"%s*\"", prefix);
  if (!git_in(clone, args, tags, sizeof(tags))) return;
  RaeSemver best = {0, 0, 0};
  bool found = false;
  char best_text[64] = {0};
  char* saveptr = NULL;
  for (char* line = strtok_r(tags, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
    RaeSemver candidate;
    const char* text = line + strlen(prefix);
    if (!rae_version_exact(text, &candidate)) continue;
    if (!found || rae_semver_compare(candidate, best) > 0) {
      best = candidate;
      found = true;
      snprintf(best_text, sizeof(best_text), "%s", text);
    }
  }
  if (found) {
    snprintf(dep->version, sizeof(dep->version), "%s", best_text);
    dep->pin_commit[0] = '\0';
  }
}

static bool resolve_git_dep(ResolvedDep* dep, const RaePackDep* spec, const char* pack_dir) {
  char url[PATH_MAX];
  str_copy(url, sizeof(url), spec->git);
  str_copy(dep->subdir, sizeof(dep->subdir), spec->subdir);
  str_copy(dep->version, sizeof(dep->version), spec->version);
  str_copy(dep->pin_commit, sizeof(dep->pin_commit), spec->commit);
  str_copy(dep->pack_hash, sizeof(dep->pack_hash), spec->hash);
  snprintf(dep->source, sizeof(dep->source), "git:%s", url);
  if (spec->rev.len > 0) {
    fprintf(stderr, "error: dependency '%s': `rev` is gone. Write its exact version, "
                    "version: \"1.2.0\", which is the tag v1.2.0, and add commit: \"...\" "
                    "to name one commit\n", dep->name);
    return false;
  }
  RaeSemver exact;
  if (!rae_version_exact(dep->version, &exact)) {
    fprintf(stderr, "error: dependency '%s' needs an exact version, like version: \"1.2.0\"%s%s%s\n",
            dep->name, dep->version[0] ? ", not \"" : "", dep->version, dep->version[0] ? "\"" : "");
    return false;
  }
  char clone[PATH_MAX];
  snprintf(clone, sizeof(clone), "%s/.rae/deps/%s", pack_dir, dep->name);
  if (dep->subdir[0]) snprintf(dep->dir, sizeof(dep->dir), "%s/%s", clone, dep->subdir);
  else snprintf(dep->dir, sizeof(dep->dir), "%s", clone);

  /* A relative URL is relative to its pack, like a `path`, and not to where the build started */
  if (url[0] == '.') {
    char relative[PATH_MAX];
    snprintf(relative, sizeof(relative), "%s", url);
    snprintf(url, sizeof(url), "%s/%s", pack_dir, relative);
  }

  bool updating = dep_is_updating(dep->name);
  if (!dir_exists(clone)) {
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/.rae/deps", pack_dir);
    char mk[PATH_MAX + 32];
    snprintf(mk, sizeof(mk), "mkdir -p \"%s\"", parent);
    (void)system(mk);
    char command[3 * PATH_MAX];
    /* Of a repo with several packages, check out only this package's folder */
    if (dep->subdir[0]) {
      snprintf(command, sizeof(command),
               "git clone --quiet --no-checkout \"%s\" \"%s\" 2>&1 && "
               "git -C \"%s\" sparse-checkout set --cone -- \"%s\" 2>&1 && "
               "git -C \"%s\" checkout --quiet 2>&1",
               url, clone, clone, dep->subdir, clone);
    } else {
      snprintf(command, sizeof(command), "git clone --quiet \"%s\" \"%s\" 2>&1", url, clone);
    }
    if (system(command) != 0) {
      fprintf(stderr, "error: dependency '%s': could not clone %s\n", dep->name, url);
      return false;
    }
  }
  char changes[256] = {0};
  if (git_in(clone, "status --porcelain --untracked-files=no", changes, sizeof(changes)) && changes[0]) {
    fprintf(stderr, "error: dependency '%s': %s has local changes, so its files are not the "
                    "pinned ones. Delete that folder to fetch it again\n", dep->name, clone);
    return false;
  }
  if (updating) {
    git_in(clone, "fetch --tags --force --quiet", NULL, 0);
    move_to_newest_version(dep, clone);
  }

  /* The pack's commit, else the version's tag */
  char tag[192];
  version_tag(dep, dep->version, tag, sizeof(tag));
  char target[256];
  if (dep->pin_commit[0]) snprintf(target, sizeof(target), "%s", dep->pin_commit);
  else snprintf(target, sizeof(target), "refs/tags/%s", tag);
  char verify[320];
  snprintf(verify, sizeof(verify), "rev-parse -q --verify \"%s^{commit}\"", target);
  char wanted[64] = {0};
  if (!git_in(clone, verify, wanted, sizeof(wanted)) || !wanted[0]) {
    git_in(clone, "fetch --tags --quiet", NULL, 0);
    if (!git_in(clone, verify, wanted, sizeof(wanted)) || !wanted[0]) {
      if (dep->pin_commit[0]) {
        fprintf(stderr, "error: dependency '%s': commit %s is not in %s. It may never have been "
                        "pushed, or history was rewritten. To go back to version %s, remove its "
                        "commit line\n", dep->name, dep->pin_commit, url, dep->version);
      } else {
        fprintf(stderr, "error: dependency '%s': version %s has no tag %s in %s\n",
                dep->name, dep->version, tag, url);
      }
      return false;
    }
  }
  char head[64] = {0};
  git_in(clone, "rev-parse HEAD", head, sizeof(head));
  if (strcmp(head, wanted) != 0) {
    char args[128];
    snprintf(args, sizeof(args), "checkout --quiet --detach %s", wanted);
    if (!git_in(clone, args, NULL, 0)) {
      fprintf(stderr, "error: dependency '%s': could not check out %s in %s\n", dep->name, wanted, clone);
      return false;
    }
  }
  snprintf(dep->commit, sizeof(dep->commit), "%s", wanted);
  /* git-tree: the content-addressed tree object id of the package's folder, a hash
   * of its files computed by git rather than a second hasher here */
  char tree[64] = {0};
  char args[PATH_MAX + 64];
  if (dep->subdir[0]) snprintf(args, sizeof(args), "rev-parse \"HEAD:%s\"", dep->subdir);
  else snprintf(args, sizeof(args), "rev-parse \"HEAD^{tree}\"");
  if (!git_in(clone, args, tree, sizeof(tree)) || !tree[0]) {
    fprintf(stderr, "error: dependency '%s': no folder \"%s\" at %s in %s\n",
            dep->name, dep->subdir, target, url);
    return false;
  }
  snprintf(dep->hash, sizeof(dep->hash), "git-tree:%s", tree);
  if (!updating && dep->pack_hash[0] && strcmp(dep->pack_hash, dep->hash) != 0) {
    fprintf(stderr, "error: dependency '%s': the files of version %s do not match the hash in the "
                    "pack (%s, found %s). The tag may have been moved. If the new files are "
                    "trusted: rae update %s\n",
            dep->name, dep->version, dep->pack_hash, dep->hash, dep->name);
    return false;
  }
  return true;
}

static int dep_index_named(const char* name) {
  for (int i = 0; i < s_dep_count; ++i) {
    if (strcmp(s_deps[i].name, name) == 0) return i;
  }
  return -1;
}

/* The first `*.raepack` file in `dir` (a dependency's own pack), into `out` */
static bool find_pack_file(const char* dir, char* out, size_t out_len) {
  DIR* d = opendir(dir);
  if (!d) return false;
  bool found = false;
  struct dirent* entry;
  while ((entry = readdir(d)) != NULL) {
    size_t n = strlen(entry->d_name);
    if (n > 8 && strcmp(entry->d_name + n - 8, ".raepack") == 0) {
      snprintf(out, out_len, "%s/%s", dir, entry->d_name);
      found = true;
      break;
    }
  }
  closedir(d);
  return found;
}

static void record_use(int owner, const char* name) {
  if (owner < 0) {
    if (s_root_use_count < RAE_DEPS_MAX) {
      snprintf(s_root_uses[s_root_use_count], sizeof(s_root_uses[0]), "%s", name);
      s_root_use_count += 1;
    }
    return;
  }
  ResolvedDep* dep = &s_deps[owner];
  if (dep->use_count < 16) {
    snprintf(dep->uses[dep->use_count], sizeof(dep->uses[0]), "%s", name);
    dep->use_count += 1;
  }
}

/* What a package's own pack promises and asks, against what declared it: its
 * meta.version must be the pinned version when one was given, and its `rae` is the
 * lowest compiler it needs, checked against the project's. */
static bool check_own_pack(const ResolvedDep* dep, const RaePack* own, const char* own_path) {
  if (dep->version[0] && own->meta_version.len > 0) {
    char meta[64];
    str_copy(meta, sizeof(meta), own->meta_version);
    if (strcmp(meta, dep->version) != 0) {
      fprintf(stderr, "error: dependency '%s': pinned at version %s, but the meta.version in %s is %s\n",
              dep->name, dep->version, own_path, meta);
      return false;
    }
  }
  if (own->rae_version.len > 0 && s_compiler_version[0]) {
    char needs[64];
    str_copy(needs, sizeof(needs), own->rae_version);
    RaeSemver needed;
    RaeSemver have;
    if (!rae_version_exact(needs, &needed)) {
      fprintf(stderr, "error: dependency '%s': its rae version in %s must be exact, like \"0.2.340\", "
                      "not \"%s\"\n", dep->name, own_path, needs);
      return false;
    }
    if (rae_semver_parse(s_compiler_version, &have) && rae_semver_compare(have, needed) < 0) {
      fprintf(stderr, "error: dependency '%s' needs at least Rae %s (%s); this project uses %s\n",
              dep->name, needs, own_path, s_compiler_version);
      return false;
    }
  }
  return true;
}

/* A name already registered, declared again: the same dependency, or a diamond */
static bool check_same_dep(const ResolvedDep* existing, const RaePackDep* spec, int owner) {
  if (existing->from_root) return true;  /* the project chose */
  char version[64];
  char commit[64];
  str_copy(version, sizeof(version), spec->version);
  str_copy(commit, sizeof(commit), spec->commit);
  if (strcmp(version, existing->version) == 0 && strcmp(commit, existing->pin_commit) == 0) return true;
  fprintf(stderr, "error: two packages want different versions of '%s': %s wants %s%s%s, and %s wants "
                  "%s%s%s. Declare '%s' in the project's own pack: its version wins\n",
          existing->name, existing->declared_by, existing->version[0] ? existing->version : "any version",
          existing->pin_commit[0] ? " at " : "", existing->pin_commit,
          owner >= 0 ? s_deps[owner].name : "the project", version[0] ? version : "any version",
          commit[0] ? " at " : "", commit, existing->name);
  return false;
}

/* Resolve the deps `pack` declares (its file in `pack_dir`) for the package
 * `owner` (-1: the root project), then each new dep's own pack's deps, so a
 * dependency's dependencies are loaded and every package knows which others
 * it may reach (#64888955: a package sees only its own files, the stdlib and
 * its own dependencies). All of one pack's deps come first, so the project's
 * are known before any package asks for another version of one. */
static bool resolve_pack_deps(const RaePack* pack, const char* pack_dir_in, int owner, int depth) {
  if (depth > 16) {
    fprintf(stderr, "error: dependencies nest more than 16 deep (a cycle?)\n");
    return false;
  }
  /* Absolute dep dirs: the module loader reads a leading `.` in a file path
   * (`./.rae/deps/x/Foo.rae`) as a relative import spec. */
  char pack_dir[PATH_MAX];
  if (!realpath(pack_dir_in, pack_dir)) snprintf(pack_dir, sizeof(pack_dir), "%s", pack_dir_in);
  int first_new = s_dep_count;
  for (const RaePackDep* spec = pack ? pack->deps : NULL; spec; spec = spec->next) {
    char name[128];
    str_copy(name, sizeof(name), spec->name);
    record_use(owner, name);
    int known = dep_index_named(name);
    if (known >= 0) {
      if (!check_same_dep(&s_deps[known], spec, owner)) return false;
      continue;
    }
    if (s_dep_count >= RAE_DEPS_MAX) {
      fprintf(stderr, "error: too many dependencies (max %d)\n", RAE_DEPS_MAX);
      return false;
    }
    int index = s_dep_count;
    ResolvedDep* dep = &s_deps[index];
    memset(dep, 0, sizeof(*dep));
    snprintf(dep->name, sizeof(dep->name), "%s", name);
    dep->is_git = spec->git.len > 0;
    dep->from_root = owner < 0;
    snprintf(dep->declared_by, sizeof(dep->declared_by), "%s", owner >= 0 ? s_deps[owner].name : "the project");
    bool ok = dep->is_git ? resolve_git_dep(dep, spec, pack_dir) : resolve_path_dep(dep, spec, pack_dir);
    if (!ok) return false;
    s_dep_count += 1;
  }
  /* Their own packs: their dependencies */
  int end_new = s_dep_count;
  for (int index = first_new; index < end_new; ++index) {
    char own_pack[PATH_MAX];
    if (!find_pack_file(s_deps[index].dir, own_pack, sizeof(own_pack))) continue;
    RaePack nested;
    memset(&nested, 0, sizeof(nested));
    if (raepack_parse_library_file(own_pack, &nested)) {
      bool nested_ok = check_own_pack(&s_deps[index], &nested, own_pack) &&
                       resolve_pack_deps(&nested, s_deps[index].dir, index, depth + 1);
      raepack_free(&nested);
      if (!nested_ok) return false;
    } else {
      raepack_free(&nested);
    }
  }
  return true;
}

bool rae_deps_resolve(const RaePack* pack, const char* pack_dir_in) {
  return resolve_pack_deps(pack, pack_dir_in, -1, 0);
}

void rae_deps_set_compiler_version(const char* version) {
  snprintf(s_compiler_version, sizeof(s_compiler_version), "%s", version ? version : "");
}

bool rae_deps_resolved(const char* name, RaeDepResolved* out) {
  int index = dep_index_named(name);
  if (index < 0) return false;
  out->version = s_deps[index].version;
  out->commit = s_deps[index].pin_commit;
  out->hash = s_deps[index].hash;
  return true;
}

int rae_deps_owner_of(const char* file_path) {
  if (!file_path || s_dep_count == 0) return -1;
  for (int i = 0; i < s_dep_count; ++i) {
    size_t n = strlen(s_deps[i].dir);
    if (strncmp(file_path, s_deps[i].dir, n) == 0 && file_path[n] == '/') return i;
  }
  return -1;
}

bool rae_deps_package_reaches(int from, int to) {
  if (from == to) return true;
  if (to < 0) return false;  /* no dependency reaches the project that depends on it */
  const char* name = s_deps[to].name;
  if (from < 0) {
    for (int i = 0; i < s_root_use_count; ++i) if (strcmp(s_root_uses[i], name) == 0) return true;
    return false;
  }
  for (int i = 0; i < s_deps[from].use_count; ++i) if (strcmp(s_deps[from].uses[i], name) == 0) return true;
  return false;
}

const char* rae_deps_name(int index) {
  if (index < 0 || index >= s_dep_count) return NULL;
  return s_deps[index].name;
}

void rae_deps_note_unpinned(FILE* out) {
  for (int i = 0; i < s_dep_count; ++i) {
    if (s_deps[i].is_git && !s_deps[i].pack_hash[0]) {
      fprintf(out, "note: dependency '%s' has no hash in its pack, so nothing checks that its "
                   "files stay the same: run 'rae fetch' to write it\n", s_deps[i].name);
    }
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
      fprintf(out, "%s %s  %s @ %s (%s)\n", branch, dep->name, dep->source, dep->version, shortc);
    }
  }
}
