#ifndef RAE_TOOLCHAIN_H
#define RAE_TOOLCHAIN_H

/* The toolchain store and a project's pin (docs/toolchain-and-packages-decision.md):
 * `rae toolchain pin`, `update`, `gc`, and the generated ./rae. */

#include <stdbool.h>
#include <stddef.h>

/* This compiler, as the store sees it */
typedef struct {
  const char* version;   /* compiler/VERSION */
  const char* commit;
  bool is_release;       /* built exactly at its tag */
  bool dirty;
  const char* root;      /* its checkout; NULL when unknown */
} RaeToolchainSelf;

int rae_toolchain_pin(const RaeToolchainSelf* self, const char* pack_path, const char* wanted);
int rae_toolchain_update(const RaeToolchainSelf* self, const char* pack_path);
int rae_toolchain_gc(bool dry_run);

/* Print the folder of the toolchain the pack pins, preparing it if it is missing. */
int rae_toolchain_path(const RaeToolchainSelf* self, const char* pack_path);

/* The repo's project pack, for a folder with no pack of its own, like a repo whose packs are
 * all in subfolders. False when the repo has none. */
bool rae_toolchain_repo_pack(const char* dir, char* out, size_t out_len);

/* The shared builds of the pinned toolchain: their state with no names, else build them. */
int rae_toolchain_deps(const RaeToolchainSelf* self, const char* pack_path, int count, char** names);

/* Write ./rae in `dir`, running this compiler for `rae toolchain`. */
bool rae_toolchain_write_wrapper(const RaeToolchainSelf* self, const char* dir);

/* Whether the other project packs in the repo of `pack_path` pin the same Rae as this
 * pin. Says which when they do not. */
bool rae_toolchain_repo_agrees(const char* pack_path, const char* version, const char* commit);

/* What `rae toolchain status` adds: the store, and whether ./rae is current. */
void rae_toolchain_print_status(const char* pack_path);

#endif /* RAE_TOOLCHAIN_H */
