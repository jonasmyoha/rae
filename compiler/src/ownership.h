/* Backend-neutral ownership classifiers.
 *
 * These predicates decide which Rae types need cleanup on drop and
 * which need a deep copy on shallow assignment. They are pure
 * structural walks of the AST — no codegen state, no per-backend
 * context — so both the C backend (compiler/src/c_*) and the Live
 * VM emitter (compiler/src/vm_*) can share the same answers.
 *
 * Functions previously lived in `c_stmt.c` and were declared in
 * `c_backend.h` / `c_backend_internal.h`. The bodies are unchanged;
 * this header is a refactor, not a behaviour change.
 */
#ifndef RAE_OWNERSHIP_H
#define RAE_OWNERSHIP_H

#include <stdbool.h>

#include "ast.h"

/* True iff `type` is one of the leaf heap-owning stdlib container
 * shapes the compiler treats as automatically droppable:
 *   List(T), StringMap(V), IntMap(V).
 * View/mod borrows are never drop targets. */
bool is_drop_target_type(const AstTypeRef* type);

/* Strict heap-ownership: true only when `type` transitively owns
 * container heap (List / StringMap / IntMap), NOT when its only
 * heap connection is a String field. Used by the C backend's
 * local auto-drop pass to avoid double-free on the
 * by-value-return-shallow-alias pattern (`let r: JsonValue =
 * jsonRoot(d)` shallow-copies `asString.data` out of the doc's
 * list — freeing it on `r`'s scope exit would double-free with
 * the eventual drop of the doc). */
bool type_owns_heap_storage(CompilerContext* cctx, const AstModule* module,
                            const AstTypeRef* type, int depth);

/* Permissive cascade-drop predicate: true when `type` is a `String`,
 * a List/StringMap/IntMap, or a struct that transitively carries
 * any field of those kinds.
 *
 * Used by Layer 5 struct-drop synthesis, List/Map element-drop
 * synthesis, the C backend's local auto-drop pass (since Phase 3),
 * sema's `rae_ext_rae_buf_set` cascade-element guard, and (Stage 1
 * scope) Live VM drop-helper synthesis. */
bool type_needs_cascade_drop(CompilerContext* cctx, const AstModule* module,
                             const AstTypeRef* type, int depth);

/* True if `let b: T = a` (where `a` is a bare identifier and not
 * `own` / `view`) would shallow-copy heap storage and cause a
 * double-free at scope end. True for `String`, `List(T)`,
 * `StringMap(V)`, `IntMap(V)`, and any user struct with at least
 * one field that needs deep copy. False for primitives, view/mod,
 * `c_struct` (raylib), `Any` / `RaeAny`. */
bool type_needs_deep_copy(CompilerContext* cctx, const AstModule* module,
                          const AstTypeRef* type, int depth);

#endif /* RAE_OWNERSHIP_H */
