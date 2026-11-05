#include "rae_toolchain.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diag.h"
#include "rae_pack_edit.h"
#include "rae_package_native.h"
#include "rae_version.h"
#include "raepack.h"

/* Where a machine with no local Rae checkout gets it */
#define RAE_DEFAULT_SOURCE "https://github.com/jonasmyoha/rae.git"

#define RAE_MAX_REPO_PACKS 256

void rae_toolchain_store_home(char* out, size_t out_len) {
  const char* env = getenv("RAE_HOME");
  if (env && env[0]) {
    snprintf(out, out_len, "%s", env);
    return;
  }
  const char* user_home = getenv("HOME");
  snprintf(out, out_len, "%s/.rae", user_home ? user_home : ".");
}

static bool path_exists(const char* path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static char* read_whole_file(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* text = malloc((size_t)size + 1);
  if (text) {
    size_t got = fread(text, 1, (size_t)size, file);
    text[got] = '\0';
  }
  fclose(file);
  return text;
}

static bool write_whole_file(const char* path, const char* text) {
  char temporary[PATH_MAX + 16];
  snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  FILE* out = fopen(temporary, "w");
  if (!out) {
    fprintf(stderr, "error: could not write %s\n", temporary);
    return false;
  }
  fputs(text, out);
  fclose(out);
  if (rename(temporary, path) != 0) {
    remove(temporary);
    fprintf(stderr, "error: could not update %s\n", path);
    return false;
  }
  return true;
}

/* Run a shell command, its output captured and trimmed. True when it exits 0. */
static bool capture(const char* command, char* out, size_t out_len) {
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  size_t n = out ? fread(out, 1, out_len - 1, pipe) : 0;
  if (out) {
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
  }
  return pclose(pipe) == 0;
}

/* The source of Rae: $RAE_SOURCE, else this machine's saved checkout, else the remote */
static void store_source(char* out, size_t out_len) {
  const char* env = getenv("RAE_SOURCE");
  if (env && env[0]) {
    snprintf(out, out_len, "%s", env);
    return;
  }
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  char path[PATH_MAX + 16];
  snprintf(path, sizeof(path), "%s/source", home);
  char* saved = read_whole_file(path);
  if (saved) {
    saved[strcspn(saved, "\r\n")] = '\0';
    if (saved[0] && path_exists(saved)) {
      snprintf(out, out_len, "%s", saved);
      free(saved);
      return;
    }
    free(saved);
  }
  snprintf(out, out_len, "%s", RAE_DEFAULT_SOURCE);
}

/* A compiler built in a real checkout, outside the store: new local commits can be pinned */
static void remember_source(const RaeToolchainSelf* self) {
  if (!self->root) return;
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  size_t home_len = strlen(home);
  if (strncmp(self->root, home, home_len) == 0) return;
  char dot_git[PATH_MAX + 8];
  snprintf(dot_git, sizeof(dot_git), "%s/.git", self->root);
  if (!path_exists(dot_git)) return;
  char command[2 * PATH_MAX];
  snprintf(command, sizeof(command), "mkdir -p \"%s\"", home);
  if (system(command) != 0) return;
  char path[PATH_MAX + 16];
  snprintf(path, sizeof(path), "%s/source", home);
  char text[PATH_MAX + 2];
  snprintf(text, sizeof(text), "%s\n", self->root);
  write_whole_file(path, text);
}

static bool toolchain_name_is_safe(const char* text) {
  if (!text[0]) return false;
  for (const char* c = text; *c; ++c) {
    bool ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || *c == '.' || *c == '-';
    if (!ok) return false;
  }
  return true;
}

static void toolchain_name(const char* version, const char* commit, char* out, size_t out_len) {
  if (commit && commit[0]) snprintf(out, out_len, "%s-%s", version, commit);
  else snprintf(out, out_len, "%s", version);
}

/* Run tools/toolchain-prepare.sh of this compiler's checkout. `name` NULL only fetches. */
static bool store_prepare(const RaeToolchainSelf* self, const char* name) {
  if (!self->root) {
    fprintf(stderr, "error: cannot locate this compiler's Rae checkout (set RAE_ROOT)\n");
    return false;
  }
  char home[PATH_MAX];
  char source[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  store_source(source, sizeof(source));
  char command[4 * PATH_MAX];
  snprintf(command, sizeof(command), "sh \"%s/tools/toolchain-prepare.sh\" \"%s\" \"%s\" %s", self->root,
           home, source, name ? name : "");
  return system(command) == 0;
}

/* The project packs of the repo of `pack_path`: its tracked packs with a `rae` block and
 * targets, and `pack_path` itself. Paths are malloc'd; `root` is the repo (or the pack's
 * folder outside git). */
static int repo_project_packs(const char* pack_path, char** out, int cap, char* root, size_t root_len) {
  char dir[PATH_MAX];
  char real_pack[PATH_MAX];
  if (!realpath(pack_path, real_pack)) snprintf(real_pack, sizeof(real_pack), "%s", pack_path);
  snprintf(dir, sizeof(dir), "%s", real_pack);
  char* slash = strrchr(dir, '/');
  if (slash) *slash = '\0'; else strcpy(dir, ".");
  int count = 0;
  out[count++] = strdup(real_pack);
  char command[PATH_MAX + 64];
  snprintf(command, sizeof(command), "git -C \"%s\" rev-parse --show-toplevel 2>/dev/null", dir);
  if (!capture(command, root, root_len) || !root[0]) {
    snprintf(root, root_len, "%s", dir);
    return count;
  }
  snprintf(command, sizeof(command), "git -C \"%s\" ls-files -- '*.raepack' 2>/dev/null", root);
  FILE* pipe = popen(command, "r");
  if (!pipe) return count;
  char line[PATH_MAX];
  while (fgets(line, sizeof(line), pipe) && count < cap) {
    line[strcspn(line, "\r\n")] = '\0';
    char path[2 * PATH_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", root, line);
    char real_path[PATH_MAX];
    if (!realpath(path, real_path) || strcmp(real_path, real_pack) == 0) continue;
    char* text = read_whole_file(real_path);
    if (!text) continue;
    char version[64];
    char commit[64];
    char check[512];
    bool is_project = rae_pack_read_pin(text, version, sizeof(version), commit, sizeof(commit), check,
                                        sizeof(check)) &&
                      rae_pack_has_block(text, "targets");
    free(text);
    if (is_project) out[count++] = strdup(real_path);
  }
  pclose(pipe);
  return count;
}

static void free_packs(char** packs, int count) {
  for (int i = 0; i < count; ++i) free(packs[i]);
}

bool rae_toolchain_repo_agrees(const char* pack_path, const char* version, const char* commit) {
  char* packs[RAE_MAX_REPO_PACKS];
  char root[PATH_MAX];
  int count = repo_project_packs(pack_path, packs, RAE_MAX_REPO_PACKS, root, sizeof(root));
  bool agree = true;
  for (int i = 1; i < count && agree; ++i) {
    char* text = read_whole_file(packs[i]);
    if (!text) continue;
    char other_version[64] = {0};
    char other_commit[64] = {0};
    char check[512];
    bool pinned = rae_pack_read_pin(text, other_version, sizeof(other_version), other_commit,
                                    sizeof(other_commit), check, sizeof(check));
    free(text);
    size_t a = strlen(commit);
    size_t b = strlen(other_commit);
    size_t shorter = a < b ? a : b;
    bool same_commit = (a == 0 && b == 0) || (shorter >= 7 && strncmp(commit, other_commit, shorter) == 0);
    if (pinned && (strcmp(version, other_version) != 0 || !same_commit)) {
      fprintf(stderr,
              "error: the packs of this repo pin different Rae: %s says %s%s%s, and %s says %s%s%s.\n"
              "       One repo builds with one Rae: ./rae toolchain pin changes them all\n",
              packs[0], version, a ? " at " : "", commit, packs[i], other_version, b ? " at " : "", other_commit);
      agree = false;
    }
  }
  free_packs(packs, count);
  return agree;
}

/* Write the pin into one pack, keeping its `check` */
static bool pack_set_pin(const char* path, const char* version, const char* commit) {
  char* text = read_whole_file(path);
  if (!text) {
    fprintf(stderr, "error: could not read %s\n", path);
    return false;
  }
  char old_version[64];
  char old_commit[64];
  char check[512] = {0};
  rae_pack_read_pin(text, old_version, sizeof(old_version), old_commit, sizeof(old_commit), check, sizeof(check));
  char block[1024];
  size_t at = (size_t)snprintf(block, sizeof(block), "  rae: {\n    version: \"%s\"\n", version);
  if (commit[0]) at += (size_t)snprintf(block + at, sizeof(block) - at, "    commit: \"%s\"\n", commit);
  if (check[0]) at += (size_t)snprintf(block + at, sizeof(block) - at, "    check: \"%s\"\n", check);
  snprintf(block + at, sizeof(block) - at, "  }\n");
  const char* const before[] = {"meta", "dependencies", "targets", NULL};
  char* edited = rae_pack_set_block(text, "rae", block, before);
  free(text);
  bool ok = edited && write_whole_file(path, edited);
  free(edited);
  return ok;
}

static const char* const WRAPPER_TEMPLATE =
  "#!/bin/sh\n"
  "# Runs the Rae this project pins (docs/toolchain-and-packages-decision.md in Rae).\n"
  "# Generated by `rae toolchain`: do not edit. RAE_ROOT=<a Rae checkout> uses that one.\n"
  "set -eu\n"
  "tool=\"%s\"\n"
  "url=\"%s\"\n"
  "here=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
  "home=${RAE_HOME:-$HOME/.rae}\n"
  "[ -z \"${RAE_ROOT:-}\" ] || exec \"$RAE_ROOT/compiler/bin/rae\" \"$@\"\n"
  "name=\n"
  "[ \"${1:-}\" = toolchain ] || name=$(find \"$here\" -maxdepth 3 -name '*.raepack' -not -path '*/.rae/*' | sort |\n"
  "  while IFS= read -r pack; do\n"
  "    grep -q 'targets:' \"$pack\" || continue\n"
  "    pin=$(awk '/^[ \\t]*rae:[ \\t]*\\{/ {on=1} on {print} on && /\\}/ {exit}' \"$pack\")\n"
  "    [ -n \"$pin\" ] || continue\n"
  "    commit=$(echo \"$pin\" | sed -n 's/.*commit: *\"\\([^\"]*\\)\".*/\\1/p')\n"
  "    echo \"$pin\" | sed -n 's/.*version: *\"\\([^\"]*\\)\".*/\\1/p' | sed \"s/$/${commit:+-$commit}/\"\n"
  "    break\n"
  "  done)\n"
  "name=${name:-$tool}\n"
  "if [ ! -f \"$home/toolchains/$name/.ready\" ]; then\n"
  "  saved=$(cat \"$home/source\" 2>/dev/null || true)\n"
  "  [ -e \"$saved\" ] || saved=$url\n"
  "  source=${RAE_SOURCE:-$saved}\n"
  "  mkdir -p \"$home\"\n"
  "  n=0; until mkdir \"$home/repo.lock\" 2>/dev/null; do\n"
  "    n=$((n + 1)); [ $n -lt 300 ] || { echo \"rae: remove $home/repo.lock if nothing runs\" >&2; exit 1; }; sleep 1\n"
  "  done\n"
  "  trap 'rmdir \"$home/repo.lock\"' EXIT\n"
  "  [ -d \"$home/repo/.git\" ] || git clone --quiet --no-checkout \"$source\" \"$home/repo\"\n"
  "  git -C \"$home/repo\" fetch --quiet --tags --force \"$source\" '+refs/heads/*:refs/remotes/source/*' || true\n"
  "  rmdir \"$home/repo.lock\"; trap - EXIT\n"
  "  ref=refs/tags/v$tool\n"
  "  case $tool in *-*) ref=${tool#*-} ;; esac\n"
  "  git -C \"$home/repo\" show \"$ref:tools/toolchain-prepare.sh\" | sh -s -- \"$home\" \"$source\" \"$name\"\n"
  "fi\n"
  "grep -Fqx \"$here\" \"$home/projects\" 2>/dev/null || echo \"$here\" >> \"$home/projects\"\n"
  "[ \"${1:-}\" = toolchain ] || [ ! -f \"$home/toolchains/$name/.deps\" ] || . \"$home/toolchains/$name/.deps\"\n"
    "exec \"$home/toolchains/$name/compiler/bin/rae\" \"$@\"\n";

static void self_name(const RaeToolchainSelf* self, char* out, size_t out_len) {
  toolchain_name(self->version, self->is_release && !self->dirty ? "" : self->commit, out, out_len);
}

static void wrapper_text(const char* tool, char* out, size_t out_len) {
  snprintf(out, out_len, WRAPPER_TEMPLATE, tool, RAE_DEFAULT_SOURCE);
}

static bool write_wrapper_tool(const char* tool, const char* dir) {
  char text[4096];
  wrapper_text(tool, text, sizeof(text));
  char path[PATH_MAX + 8];
  snprintf(path, sizeof(path), "%s/rae", dir);
  if (!write_whole_file(path, text)) return false;
  return chmod(path, 0755) == 0;
}

bool rae_toolchain_write_wrapper(const RaeToolchainSelf* self, const char* dir) {
  char tool[128];
  self_name(self, tool, sizeof(tool));
  return write_wrapper_tool(tool, dir);
}

/* The compiler `./rae toolchain` runs: the newer of this one and the pin, since an old pin
 * may not have these commands */
static void wrapper_tool(const RaeToolchainSelf* self, const char* version, const char* commit, char* out,
                         size_t out_len) {
  RaeSemver mine;
  RaeSemver pinned;
  if (rae_semver_parse(self->version, &mine) && rae_version_exact(version, &pinned) &&
      rae_semver_compare(pinned, mine) > 0) {
    toolchain_name(version, commit, out, out_len);
    return;
  }
  self_name(self, out, out_len);
}

/* A pin only this machine can get breaks on the next one: say so */
static void warn_if_only_here(const RaeToolchainSelf* self, const char* version, const char* commit) {
  if (!self->root) return;
  char command[PATH_MAX + 256];
  if (commit[0]) {
    snprintf(command, sizeof(command),
             "git -C \"%s\" for-each-ref --contains %s refs/remotes/origin 2>/dev/null", self->root, commit);
    char found[256] = {0};
    capture(command, found, sizeof(found));
    if (!found[0]) {
      fprintf(stderr, "warning: commit %s is not on the remote, so another machine could not get it: push it in Rae\n",
              commit);
    }
    return;
  }
  snprintf(command, sizeof(command), "git -C \"%s\" ls-remote --exit-code origin refs/tags/v%s >/dev/null 2>&1",
           self->root, version);
  if (system(command) != 0) {
    fprintf(stderr, "warning: the tag v%s is not on the remote, so another machine could not get it: "
                    "git push --follow-tags in Rae\n", version);
  }
}

static bool repo_apply_pin(const char* pack_path, const char* version, const char* commit, const char* tool,
                           char* root, size_t root_len) {
  char* packs[RAE_MAX_REPO_PACKS];
  int count = repo_project_packs(pack_path, packs, RAE_MAX_REPO_PACKS, root, root_len);
  bool ok = true;
  for (int i = 0; i < count && ok; ++i) ok = pack_set_pin(packs[i], version, commit);
  if (ok) {
    for (int i = 0; i < count; ++i) printf("pinned %s to Rae %s%s%s\n", packs[i], version, commit[0] ? " at " : "", commit);
  }
  free_packs(packs, count);
  return ok && write_wrapper_tool(tool, root);
}

int rae_toolchain_pin(const RaeToolchainSelf* self, const char* pack_path, const char* wanted) {
  remember_source(self);
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  RaeSemver exact;
  char version[64] = {0};
  char commit[64] = {0};
  if (rae_version_exact(wanted, &exact)) {
    snprintf(version, sizeof(version), "%s", wanted);
  } else {
    if (!store_prepare(self, NULL)) return 1;
    bool is_main = strcmp(wanted, "main") == 0;
    if (!is_main && !toolchain_name_is_safe(wanted)) {
      fprintf(stderr, "error: pin needs a version like 0.2.340, a commit, or main; not '%s'\n", wanted);
      return 1;
    }
    char command[PATH_MAX + 256];
    snprintf(command, sizeof(command), "git -C \"%s/repo\" rev-parse -q --verify \"%s^{commit}\"", home,
             is_main ? "refs/remotes/source/main" : wanted);
    if (!capture(command, commit, sizeof(commit)) || !commit[0]) {
      fprintf(stderr, "error: '%s' is not a commit of Rae; the pin stays\n", wanted);
      return 1;
    }
    snprintf(command, sizeof(command), "git -C \"%s/repo\" show \"%s:compiler/VERSION\"", home, commit);
    if (!capture(command, version, sizeof(version)) || !rae_version_exact(version, &exact)) {
      fprintf(stderr, "error: commit %s has no usable compiler/VERSION; the pin stays\n", commit);
      return 1;
    }
    /* The commit of a version's own tag is that version, with no commit needed */
    char tagged[64] = {0};
    snprintf(command, sizeof(command), "git -C \"%s/repo\" rev-parse -q --verify \"refs/tags/v%s^{commit}\"", home, version);
    if (capture(command, tagged, sizeof(tagged)) && strcmp(tagged, commit) == 0) commit[0] = '\0';
  }
  char name[128];
  toolchain_name(version, commit, name, sizeof(name));
  if (!store_prepare(self, name)) {
    fprintf(stderr, "error: Rae %s cannot be used (above); the pin stays\n", name);
    return 1;
  }
  char tool[128];
  wrapper_tool(self, version, commit, tool, sizeof(tool));
  char root[PATH_MAX];
  if (!repo_apply_pin(pack_path, version, commit, tool, root, sizeof(root))) return 1;
  warn_if_only_here(self, version, commit);
  printf("wrote %s/rae. Commit it with the pack, and build with ./rae\n", root);
  return 0;
}

bool rae_toolchain_repo_pack(const char* dir, char* out, size_t out_len) {
  char root[PATH_MAX];
  char command[PATH_MAX + 64];
  snprintf(command, sizeof(command), "git -C \"%s\" rev-parse --show-toplevel 2>/dev/null", dir);
  if (!capture(command, root, sizeof(root)) || !root[0]) return false;
  snprintf(command, sizeof(command), "git -C \"%s\" ls-files -- '*.raepack' 2>/dev/null", root);
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  bool found = false;
  char line[PATH_MAX];
  while (!found && fgets(line, sizeof(line), pipe)) {
    line[strcspn(line, "\r\n")] = '\0';
    char path[2 * PATH_MAX + 2];
    snprintf(path, sizeof(path), "%s/%s", root, line);
    char* text = read_whole_file(path);
    if (!text) continue;
    char version[64];
    char commit[64];
    char check[512];
    found = rae_pack_read_pin(text, version, sizeof(version), commit, sizeof(commit), check, sizeof(check)) &&
            rae_pack_has_block(text, "targets");
    free(text);
    if (found) snprintf(out, out_len, "%s", path);
  }
  while (fgets(line, sizeof(line), pipe)) { /* let git finish */ }
  pclose(pipe);
  return found;
}

/* The toolchain the pack pins, prepared if it is missing */
static bool pinned_toolchain(const RaeToolchainSelf* self, const char* pack_path, char* name, size_t name_len) {
  char* text = read_whole_file(pack_path);
  char version[64] = {0};
  char commit[64] = {0};
  char check[512];
  bool pinned = text && rae_pack_read_pin(text, version, sizeof(version), commit, sizeof(commit), check, sizeof(check));
  free(text);
  if (!pinned) {
    fprintf(stderr, "error: %s pins no Rae: rae toolchain pin <version>\n", pack_path);
    return false;
  }
  toolchain_name(version, commit, name, name_len);
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  char ready[2 * PATH_MAX];
  snprintf(ready, sizeof(ready), "%s/toolchains/%s/.ready", home, name);
  return path_exists(ready) || store_prepare(self, name);
}

int rae_toolchain_path(const RaeToolchainSelf* self, const char* pack_path) {
  char name[128];
  if (!pinned_toolchain(self, pack_path, name, sizeof(name))) return 1;
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  printf("%s/toolchains/%s\n", home, name);
  return 0;
}

int rae_toolchain_deps(const RaeToolchainSelf* self, const char* pack_path, int count, char** names) {
  char name[128];
  if (!pinned_toolchain(self, pack_path, name, sizeof(name))) return 1;
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  if (!self->root) {
    fprintf(stderr, "error: cannot locate this compiler's Rae checkout (set RAE_ROOT)\n");
    return 1;
  }
  char command[8 * PATH_MAX];
  size_t at = (size_t)snprintf(command, sizeof(command), "sh \"%s/tools/toolchain-prepare.sh\" --deps \"%s\" \"%s\"",
                               self->root, home, name);
  for (int i = 0; i < count; ++i) {
    at += (size_t)snprintf(command + at, sizeof(command) - at, " \"%s\"", names[i]);
    if (at >= sizeof(command)) return 1;
  }
  return system(command) == 0 ? 0 : 1;
}

/* The newest exact version among the clone's tags */
static bool newest_tagged_version(const char* home, char* out, size_t out_len) {
  char command[PATH_MAX + 64];
  snprintf(command, sizeof(command), "git -C \"%s/repo\" tag --list 'v[0-9]*'", home);
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  bool found = false;
  RaeSemver best = {0, 0, 0};
  char line[128];
  while (fgets(line, sizeof(line), pipe)) {
    line[strcspn(line, "\r\n")] = '\0';
    RaeSemver candidate;
    if (!rae_version_exact(line + 1, &candidate)) continue;
    if (!found || rae_semver_compare(candidate, best) > 0) {
      best = candidate;
      found = true;
      snprintf(out, out_len, "%s", line + 1);
    }
  }
  pclose(pipe);
  return found;
}

/* Does this project still build with `bin`: the pack's `check` command, else every compiled
 * target as far as a C object, which needs none of the app's link libraries */
static bool project_still_builds(const char* bin, char** packs, int count, const char* root) {
  setenv("RAE_FORMAT", "off", 1);
  unsetenv("RAE_ROOT");
  char check[512] = {0};
  for (int i = 0; i < count && !check[0]; ++i) {
    char* text = read_whole_file(packs[i]);
    if (!text) continue;
    char version[64];
    char commit[64];
    rae_pack_read_pin(text, version, sizeof(version), commit, sizeof(commit), check, sizeof(check));
    free(text);
  }
  if (check[0]) {
    setenv("RAE_UPDATE_CHECK", check, 1);
    setenv("RAE_BIN", bin, 1);
    char command[PATH_MAX + 64];
    snprintf(command, sizeof(command), "cd \"%s\" && sh -c \"$RAE_UPDATE_CHECK\"", root);
    printf("running the pack's check: %s\n", check);
    return system(command) == 0;
  }
  char temporary[] = "/tmp/rae-update.XXXXXX";
  if (!mkdtemp(temporary)) return false;
  bool ok = true;
  for (int i = 0; i < count && ok; ++i) {
    RaePack pack;
    diag_set_quiet(true);
    bool parsed = raepack_parse_file(packs[i], &pack, false);
    diag_set_quiet(false);
    if (!parsed) {
      fprintf(stderr, "error: %s does not parse\n", packs[i]);
      ok = false;
      break;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", packs[i]);
    char* slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    for (const RaePackTarget* target = pack.targets; target && ok; target = target->next) {
      bool compiled = target->exports.len == 0;
      bool any = false;
      for (const RaePackSource* source = target->sources; source; source = source->next) {
        if (source->emit == RAEPACK_EMIT_COMPILED) any = true;
      }
      if (!compiled || !any || target->entry.len == 0) continue;
      char* entry = str_to_cstr(target->entry);
      char command[4 * PATH_MAX];
      snprintf(command, sizeof(command),
               "\"%s\" build --target compiled --emit-c --project \"%s\" --entry \"%s/%s\" --out \"%s/app.c\"",
               bin, dir, dir, entry, temporary);
      int status = system(command);
      if (status == 0) {
        snprintf(command, sizeof(command), "cd \"%s\" && cc -std=c11 -w -c app.c -I. -o app.o", temporary);
        status = system(command);
      }
      if (status != 0) {
        fprintf(stderr, "error: %s, target %s, does not build\n", packs[i], entry);
        ok = false;
      }
      free(entry);
    }
    raepack_free(&pack);
  }
  char cleanup[64];
  snprintf(cleanup, sizeof(cleanup), "rm -rf \"%s\"", temporary);
  if (system(cleanup) != 0) fprintf(stderr, "warning: could not remove %s\n", temporary);
  return ok;
}

int rae_toolchain_update(const RaeToolchainSelf* self, const char* pack_path) {
  remember_source(self);
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  if (!store_prepare(self, NULL)) return 1;
  char newest[64] = {0};
  if (!newest_tagged_version(home, newest, sizeof(newest))) {
    fprintf(stderr, "error: Rae has no version tags in %s/repo\n", home);
    return 1;
  }
  char* text = read_whole_file(pack_path);
  char old_version[64] = {0};
  char old_commit[64] = {0};
  char check[512];
  if (text) rae_pack_read_pin(text, old_version, sizeof(old_version), old_commit, sizeof(old_commit), check, sizeof(check));
  free(text);
  if (strcmp(old_version, newest) == 0 && !old_commit[0]) {
    printf("already on the newest Rae, %s\n", newest);
    return 0;
  }
  if (!store_prepare(self, newest)) {
    fprintf(stderr, "error: Rae %s cannot be used (above); the pin stays\n", newest);
    return 1;
  }
  char* packs[RAE_MAX_REPO_PACKS];
  char root[PATH_MAX];
  int count = repo_project_packs(pack_path, packs, RAE_MAX_REPO_PACKS, root, sizeof(root));
  char* saved[RAE_MAX_REPO_PACKS];
  for (int i = 0; i < count; ++i) saved[i] = read_whole_file(packs[i]);
  char wrapper_path[PATH_MAX + 8];
  snprintf(wrapper_path, sizeof(wrapper_path), "%s/rae", root);
  char* saved_wrapper = read_whole_file(wrapper_path);

  char tool[128];
  wrapper_tool(self, newest, "", tool, sizeof(tool));
  char ignored_root[PATH_MAX];
  bool ok = repo_apply_pin(pack_path, newest, "", tool, ignored_root, sizeof(ignored_root));
  if (ok) {
    char bin[PATH_MAX + 64];
    snprintf(bin, sizeof(bin), "%s/toolchains/%s/compiler/bin/rae", home, newest);
    ok = project_still_builds(bin, packs, count, root);
  }
  if (!ok) {
    for (int i = 0; i < count; ++i) {
      if (saved[i]) write_whole_file(packs[i], saved[i]);
    }
    if (saved_wrapper) {
      write_whole_file(wrapper_path, saved_wrapper);
      chmod(wrapper_path, 0755);
    } else {
      remove(wrapper_path);
    }
    fprintf(stderr, "error: this project does not build with Rae %s (above). The pin stays at %s%s%s\n",
            newest, old_version, old_commit[0] ? " at " : "", old_commit);
  } else {
    printf("updated to Rae %s. Commit the packs and ./rae\n", newest);
  }
  for (int i = 0; i < count; ++i) free(saved[i]);
  free(saved_wrapper);
  free_packs(packs, count);
  return ok ? 0 : 1;
}

/* Whether `name` is in the newline-joined list `keep` */
static bool keep_has(const char* keep, const char* name) {
  size_t n = strlen(name);
  for (const char* at = keep; (at = strstr(at, name)) != NULL; at += 1) {
    bool starts = at == keep || at[-1] == '\n';
    if (starts && at[n] == '\n') return true;
  }
  return false;
}

/* Remove the shared builds no kept toolchain's .deps names. The count removed. */
static int gc_deps(const char* home, const char* kept_deps, bool dry_run) {
  char deps_root[PATH_MAX + 16];
  snprintf(deps_root, sizeof(deps_root), "%s/deps", home);
  DIR* names = opendir(deps_root);
  if (!names) return 0;
  int removed = 0;
  struct dirent* dep;
  while ((dep = readdir(names)) != NULL) {
    if (dep->d_name[0] == '.') continue;
    char dep_path[2 * PATH_MAX];
    snprintf(dep_path, sizeof(dep_path), "%s/%s", deps_root, dep->d_name);
    DIR* pins = opendir(dep_path);
    if (!pins) continue;
    struct dirent* pin;
    while ((pin = readdir(pins)) != NULL) {
      size_t n = strlen(pin->d_name);
      if (pin->d_name[0] == '.' || (n > 5 && strcmp(pin->d_name + n - 5, ".lock") == 0)) continue;
      char path[3 * PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", dep_path, pin->d_name);
      char lock[3 * PATH_MAX + 8];
      snprintf(lock, sizeof(lock), "%s.lock", path);
      if (strstr(kept_deps, path) || path_exists(lock)) continue;
      removed += 1;
      printf("%s %s/%s\n", dry_run ? "would remove" : "removing", dep->d_name, pin->d_name);
      if (dry_run) continue;
      char command[4 * PATH_MAX];
      snprintf(command, sizeof(command), "rm -rf \"%s\"", path);
      if (system(command) != 0) fprintf(stderr, "warning: could not remove %s\n", path);
    }
    closedir(pins);
  }
  closedir(names);
  return removed;
}

int rae_toolchain_gc(bool dry_run) {
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  char projects_path[PATH_MAX + 16];
  snprintf(projects_path, sizeof(projects_path), "%s/projects", home);
  char* projects = read_whole_file(projects_path);
  size_t keep_cap = 1024 * 1024;
  char* keep = calloc(1, keep_cap);
  char* still = calloc(1, keep_cap);
  char* package_keep = calloc(1, keep_cap);
  if (!keep || !still || !package_keep) return 1;
  size_t package_keep_len = 0;
  size_t keep_len = 0;
  size_t still_len = 0;
  char* saveptr = NULL;
  for (char* project = projects ? strtok_r(projects, "\n", &saveptr) : NULL; project;
       project = strtok_r(NULL, "\n", &saveptr)) {
    char wrapper[PATH_MAX + 8];
    snprintf(wrapper, sizeof(wrapper), "%s/rae", project);
    if (!path_exists(wrapper)) continue;  /* that project is gone, or no longer uses this */
    if (still_len + PATH_MAX >= keep_cap || keep_len + 4096 >= keep_cap) break;
    still_len += (size_t)snprintf(still + still_len, keep_cap - still_len, "%s\n", project);
    char* wrapper_text_read = read_whole_file(wrapper);
    if (wrapper_text_read) {
      const char* tool = strstr(wrapper_text_read, "tool=\"");
      if (tool) {
        tool += 6;
        const char* end = strchr(tool, '"');
        if (end) keep_len += (size_t)snprintf(keep + keep_len, keep_cap - keep_len, "%.*s\n", (int)(end - tool), tool);
      }
      free(wrapper_text_read);
    }
    char command[PATH_MAX + 128];
    snprintf(command, sizeof(command), "find \"%s\" -maxdepth 3 -name '*.raepack' -not -path '*/.rae/*'", project);
    FILE* pipe = popen(command, "r");
    if (!pipe) continue;
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), pipe)) {
      line[strcspn(line, "\r\n")] = '\0';
      rae_package_native_keep(line, package_keep, keep_cap, &package_keep_len, 0);
      char* text = read_whole_file(line);
      if (!text) continue;
      char version[64];
      char commit[64];
      char check[512];
      if (rae_pack_read_pin(text, version, sizeof(version), commit, sizeof(commit), check, sizeof(check))) {
        char name[128];
        toolchain_name(version, commit, name, sizeof(name));
        if (keep_len + 256 < keep_cap) {
          keep_len += (size_t)snprintf(keep + keep_len, keep_cap - keep_len, "%s\n", name);
        }
      }
      free(text);
    }
    pclose(pipe);
  }
  free(projects);
  char toolchains[PATH_MAX + 16];
  snprintf(toolchains, sizeof(toolchains), "%s/toolchains", home);
  DIR* dir = opendir(toolchains);
  int removed = 0;
  int kept = 0;
  char* kept_deps = calloc(1, keep_cap);
  if (!kept_deps) return 1;
  size_t kept_deps_len = 0;
  if (dir) {
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
      char path[2 * PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", toolchains, entry->d_name);
      struct stat st;
      if (entry->d_name[0] == '.' || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
      size_t n = strlen(entry->d_name);
      if (n > 5 && strcmp(entry->d_name + n - 5, ".lock") == 0) continue;
      char lock[2 * PATH_MAX + 8];
      snprintf(lock, sizeof(lock), "%s.lock", path);
      if (keep_has(keep, entry->d_name) || path_exists(lock)) {
        kept += 1;
        char deps_path[2 * PATH_MAX + 8];
        snprintf(deps_path, sizeof(deps_path), "%s/.deps", path);
        char* deps_text = read_whole_file(deps_path);
        if (deps_text && kept_deps_len + strlen(deps_text) + 1 < keep_cap) {
          kept_deps_len += (size_t)snprintf(kept_deps + kept_deps_len, keep_cap - kept_deps_len, "%s", deps_text);
        }
        free(deps_text);
        continue;
      }
      removed += 1;
      printf("%s %s\n", dry_run ? "would remove" : "removing", entry->d_name);
      if (dry_run) continue;
      char command[4 * PATH_MAX];
      snprintf(command, sizeof(command),
               "git -C \"%s/repo\" worktree remove --force \"%s\" >/dev/null 2>&1 || rm -rf \"%s\"; rm -f \"%s.log\"", home, path,
               path, path);
      if (system(command) != 0) fprintf(stderr, "warning: could not remove %s\n", path);
    }
    closedir(dir);
  }
  removed += gc_deps(home, kept_deps, dry_run);
  removed += rae_package_native_gc(home, package_keep, dry_run);
  free(package_keep);
  free(kept_deps);
  if (!dry_run) {
    char command[PATH_MAX + 64];
    snprintf(command, sizeof(command), "git -C \"%s/repo\" worktree prune >/dev/null 2>&1", home);
    if (system(command) != 0) { /* no clone yet: nothing to prune */ }
    if (path_exists(projects_path)) write_whole_file(projects_path, still);
  }
  printf("%d kept, %d %s. A removed toolchain comes back the next time a project needs it\n", kept, removed,
         dry_run ? "to remove" : "removed");
  free(keep);
  free(still);
  return 0;
}

bool rae_toolchain_wrapper_runs_pin(const char* pack_path, const char* version, const char* commit) {
  char* packs[RAE_MAX_REPO_PACKS];
  char root[PATH_MAX];
  int count = repo_project_packs(pack_path, packs, RAE_MAX_REPO_PACKS, root, sizeof(root));
  free_packs(packs, count);
  char wrapper[PATH_MAX + 8];
  snprintf(wrapper, sizeof(wrapper), "%s/rae", root);
  if (!path_exists(wrapper)) return false;
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  char name[128];
  toolchain_name(version, commit, name, sizeof(name));
  char ready[2 * PATH_MAX];
  snprintf(ready, sizeof(ready), "%s/toolchains/%s/.ready", home, name);
  printf("verdict: ./rae builds with %s/toolchains/%s%s\n", home, name,
         path_exists(ready) ? "" : " (made on its first use)");
  return true;
}

void rae_toolchain_print_status(const char* pack_path) {
  char home[PATH_MAX];
  rae_toolchain_store_home(home, sizeof(home));
  printf("store: %s\n", home);
  if (!pack_path) return;
  char* packs[RAE_MAX_REPO_PACKS];
  char root[PATH_MAX];
  int count = repo_project_packs(pack_path, packs, RAE_MAX_REPO_PACKS, root, sizeof(root));
  free_packs(packs, count);
  char wrapper_path[PATH_MAX + 8];
  snprintf(wrapper_path, sizeof(wrapper_path), "%s/rae", root);
  char* found = read_whole_file(wrapper_path);
  if (!found) {
    printf("./rae: none. ./rae comes with: rae toolchain pin <version>\n");
    return;
  }
  const char* tool = strstr(found, "tool=\"");
  char tool_name[128] = {0};
  if (tool) {
    tool += 6;
    const char* end = strchr(tool, '"');
    if (end) snprintf(tool_name, sizeof(tool_name), "%.*s", (int)(end - tool), tool);
  }
  char current[4096];
  wrapper_text(tool_name, current, sizeof(current));
  printf("./rae: %s\n", strcmp(found, current) == 0 ? "current" : "out of date: rae toolchain pin rewrites it");
  free(found);
}
