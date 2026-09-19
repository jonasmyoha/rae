/* array_methods.h - the List-shaped API of Array(T, cap: N), synthesized per cap.
 *
 * `Array(T, cap: N)` is a compiler builtin (a struct wrapping T[N]), so its
 * methods cannot live in lib/core as ordinary Rae source: no Rae signature
 * can range over the cap. The next best thing is Rae source anyway — one
 * small generic module per DISTINCT cap the program uses, generated as text
 * and parsed by the real parser, so `copyAt`/`viewAt`/`modAt`/
 * `copyAtFallback`/`set`/`length` on an Array are ordinary generic functions
 * (`T: type, this: view Array(T, cap: 16)`) that the method resolver, the
 * specializer and the C backend treat exactly like List's. The bodies are
 * the only Rae code allowed to index an Array with `[]` (inside `unsafe`),
 * the same way List's wrappers are the only callers of the buffer intrinsics.
 * See docs/collections.md and docs/value-aggregates-and-ownership.md §1.7. */
#ifndef ARRAY_METHODS_H
#define ARRAY_METHODS_H

#include <stdint.h>
#include "arena.h"
#include "ast.h"

/* Parse the Array method module for `cap`. Returns the decl list (NULL on a
 * parse failure, which is a compiler bug, not a user error). Every decl is
 * stamped with module_name "core/Array" and no origin file, so it is visible
 * from every file like the prelude. */
AstDecl* array_methods_synthesize(Arena* arena, int64_t cap);

#endif /* ARRAY_METHODS_H */
