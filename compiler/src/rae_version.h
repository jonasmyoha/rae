#ifndef RAE_VERSION_H
#define RAE_VERSION_H

/* Semantic-version parsing and the toolchain-requirement check (#931,
 * docs/versioning-and-toolchain.md §2). Pure: it takes the compiler's own
 * version as data and never reads the generated version header, so it stays
 * unit-testable and out of the build-time git plumbing. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  int major;
  int minor;
  int patch;
} RaeSemver;

/* Parse "X", "X.Y", or "X.Y.Z" (missing components default to 0). Any
 * "-prerelease" / "+build" suffix is ignored — the base version is what the
 * requirement check compares. Returns false on malformed input. */
bool rae_semver_parse(const char* text, RaeSemver* out);

/* Does `compiler` satisfy the requirement string `req`?
 *   - a bare version is a caret requirement: "0.3" -> >=0.3.0 <0.4.0,
 *     "0.3.1" -> >=0.3.1 <0.4.0 (pre-1.0 the MINOR is the breaking axis; at
 *     >=1.0 the caret widens to the next MAJOR);
 *   - an explicit range ">=A <B" (any of >=,>,<=,< in either order).
 * `is_dev` marks a between-tags build; in the default (lenient) mode a dev
 * build's base version counts as released, so 0.3.0-dev.7 satisfies "0.3".
 * `strict` (from --check-toolchain) requires a tagged release: a dev build
 * never satisfies a requirement under strict.
 * On a malformed `req`, sets *bad_req (when non-NULL) and returns false. */
bool rae_toolchain_satisfies(const char* req, RaeSemver compiler, bool is_dev,
                             bool strict, bool* bad_req);

#endif /* RAE_VERSION_H */
