#ifndef RAE_VERSION_H
#define RAE_VERSION_H

/* Semantic-version parsing, for pins (docs/toolchain-and-packages-decision.md). Pure: it
 * never reads the generated version header. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  int major;
  int minor;
  int patch;
} RaeSemver;

/* Parse "X", "X.Y", or "X.Y.Z" (missing components default to 0). Any
 * "-prerelease" / "+build" suffix is ignored. Returns false on malformed input. */
bool rae_semver_parse(const char* text, RaeSemver* out);

/* A pin is one exact version: "X.Y.Z" and nothing else, so "0.2" and ranges are
 * not pins. False for anything else. */
bool rae_version_exact(const char* text, RaeSemver* out);

/* <0, 0 or >0, as `left` is older than, equal to or newer than `right`. */
int rae_semver_compare(RaeSemver left, RaeSemver right);

#endif /* RAE_VERSION_H */
