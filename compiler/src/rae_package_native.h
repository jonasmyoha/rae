#ifndef RAE_PACKAGE_NATIVE_H
#define RAE_PACKAGE_NATIVE_H

/* A package's C: the `native` block of its pack (docs/toolchain-and-packages-decision.md,
 * section 6). No compiler code names a library: the block says how to build it, which header
 * means a program uses it, and what to link. */

#include <stdbool.h>
#include <stddef.h>

/* The folder of package `index`'s build, made if it is missing: <store>/packages/<name>/<pin>/
 * install, the pin being the hash of the package's files. False, with the error said, when its
 * build failed. A package with no native block is not asked for. */
bool rae_package_native_install(int index, char* out, size_t out_len);

/* Build every native package of the resolved dependencies. False when one failed. */
bool rae_package_native_prepare_all(void);

/* For `rae tree --native` and the device scripts: one line per native package, tab separated:
 * name, header, install folder, device sources, device defines file. */
bool rae_package_native_print(void);

/* For `rae toolchain gc`: add each package the pack at `pack_path` pins, and those its packages pin,
 * to `keep`, one "/<name>/<pin>" per line. */
void rae_package_native_keep(const char* pack_path, char* keep, size_t keep_cap, size_t* keep_len, int depth);

/* Remove each package build under <home>/packages that `keep` does not name, except one being
 * built. It comes back when a program next needs it. The count removed. */
int rae_package_native_gc(const char* home, const char* keep, bool dry_run);

#endif /* RAE_PACKAGE_NATIVE_H */
