#ifndef RAE_DEPS_H
#define RAE_DEPS_H

/* Dependency resolution (#933, docs/toolchain-and-packages-decision.md).
 *
 * A dep named `x` makes `import x/Foo` resolve to <depdir>/Foo.rae. Path deps
 * resolve in place. Git deps are cloned into <pack dir>/.rae/deps/<name> and checked
 * out at the pack's exact version (its tag) or commit, and their files are checked
 * against the pack's hash. A build only reads the pack: `rae fetch` and `rae update`
 * write it. Registry or a registry server: not here. */

#include <stdbool.h>
#include <stdio.h>

#include "raepack.h"

/* Resolve every dep of `pack` (its file lives in `pack_dir`) and register each one.
 * Diagnoses and returns false on the first unresolvable dep. */
bool rae_deps_resolve(const RaePack* pack, const char* pack_dir);

/* The version the project builds with: a dependency's `rae` is checked against it. */
void rae_deps_set_compiler_version(const char* version);

/* What resolving a git dep found, for writing it into the pack: the version in force,
 * its commit ("" for the version's tag) and the hash of its files. */
typedef struct {
  const char* version;
  const char* commit;
  const char* hash;
} RaeDepResolved;
bool rae_deps_resolved(const char* name, RaeDepResolved* out);

/* Say which git deps have no hash in their pack. A build only reads the pack, so only
 * `rae fetch` or `rae update` can write one. */
void rae_deps_note_unpinned(FILE* out);

/* Number of deps registered by rae_deps_resolve. */
int rae_deps_count(void);

/* `normalized` is a `/`-separated import path. When its first segment names a
 * registered dep, return a malloc'd path to <depdir>/<rest>.rae if that file
 * exists (NULL otherwise). */
char* rae_deps_resolve_module_file(const char* normalized);

/* Folder-package form: the dep dir itself for `import x`, or <depdir>/<rest>
 * for a sub-package, when that directory exists. malloc'd or NULL. */
char* rae_deps_resolve_folder(const char* normalized);

/* True when `file_path` lies inside a registered dep directory — such files
 * are vendored, so the build's format preflight must not rewrite them. */
bool rae_deps_owns_path(const char* file_path);

/* #934: before resolving, mark deps that move to their newest version, whose git
 * remote is fetched again: `rae update`. `only_name` NULL means every dep; otherwise
 * just that one. Off by default (a build and `rae fetch` hold to the pack). */
void rae_deps_set_update(const char* only_name);

/* #934: print the resolved dependency graph (call after rae_deps_resolve).
 * `root_label` names the project at the root; `stdlib_dir` is shown as the
 * first `lib` edge (the stdlib resolved through the toolchain). */
void rae_deps_print_tree(FILE* out, const char* root_label, const char* stdlib_dir);

/* #64888955 package boundaries: the dep that owns `file_path` (its index), or
 * -1 for a file of the root project (or the stdlib, which callers check
 * first). A package reaches itself, and a dep it declares in its own pack (the
 * root: in the project's pack); never the project that depends on it. */
int rae_deps_owner_of(const char* file_path);
bool rae_deps_package_reaches(int from, int to);
/* The name of dep `index` (NULL for -1) */
const char* rae_deps_name(int index);

#endif /* RAE_DEPS_H */
