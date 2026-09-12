#ifndef RAE_DEPS_H
#define RAE_DEPS_H

/* Dependency resolution + the `dep` lines of rae.lock (#933,
 * docs/versioning-and-toolchain.md §4, docs/raepack-v2-and-packages.md §12).
 *
 * A dep named `x` makes `import x/Foo` resolve to <depdir>/Foo.rae. Path deps
 * resolve in place; git deps are cloned into <pack dir>/.rae/deps/<name> and
 * pinned to the commit an existing rae.lock names (reproducible from the lock
 * alone), else to the pack's `rev`. Registry or a registry server: not here. */

#include <stdbool.h>
#include <stdio.h>

#include "raepack.h"

/* Resolve every dep of `pack` (its file lives in `pack_dir`), consulting the
 * lockfile at `lock_path` (may not exist) for git pins, and register each one.
 * Diagnoses and returns false on the first unresolvable dep. */
bool rae_deps_resolve(const RaePack* pack, const char* pack_dir, const char* lock_path);

/* Emit the lock's dependency records (one `dep <name> { ... }` line each,
 * after a first `dep lib` line for the stdlib the toolchain resolved). */
void rae_deps_write_lock(FILE* out, const char* stdlib_dir);

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

/* #934: before resolving, mark deps whose lock pin should be IGNORED and whose
 * git remote should be re-fetched — i.e. `rae update`. `only_name` NULL means
 * every dep; otherwise just that one. Off by default (rae_deps_resolve honors
 * the lock, which is what a build and `rae fetch` want). */
void rae_deps_set_update(const char* only_name);

/* #934: print the resolved dependency graph (call after rae_deps_resolve).
 * `root_label` names the project at the root; `stdlib_dir` is shown as the
 * first `lib` edge (the stdlib resolved through the toolchain). */
void rae_deps_print_tree(FILE* out, const char* root_label, const char* stdlib_dir);

#endif /* RAE_DEPS_H */
