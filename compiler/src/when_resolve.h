/* Compile-time `when` (docs/platform-conditional-code.md): keep only the
 * selected branch of every `when`, before sema. */
#ifndef RAE_WHEN_RESOLVE_H
#define RAE_WHEN_RESOLVE_H

#include <stdbool.h>

#include "arena.h"
#include "ast.h"

/* Resolve every declaration- and statement-level `when` of the merged
 * program. False (errors reported) when a condition is not a compile-time
 * Bool. */
bool when_resolve_module(Arena* arena, AstModule* merged);

#endif
