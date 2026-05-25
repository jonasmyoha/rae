#ifndef C_BACKEND_H
#define C_BACKEND_H

#include <stdbool.h>

#include "ast.h"

struct VmRegistry;

bool c_backend_emit_module(CompilerContext* ctx, const AstModule* module, const char* out_path, struct VmRegistry* registry, bool* out_uses_raylib);
void register_generic_type(CompilerContext* ctx, const AstTypeRef* type);
void register_function_specialization(CompilerContext* ctx, const AstFuncDecl* decl, const AstTypeRef* concrete_args);

// Cascade-drop classifier — true if `type` is `String`, or a struct
// (transitively) carrying a String field. Used by the C backend to
// decide when to emit per-field drops, and by sema to forbid raw
// `rae_ext_rae_buf_set` of cascade-drop element types in user code.
bool type_needs_cascade_drop(CompilerContext* cctx, const AstModule* module,
                             const AstTypeRef* type, int depth);

// Deep-copy classifier — true if `let b: T = a` (where `a` is a bare
// identifier and not `own`/`view`) would shallow-copy heap storage and
// trigger a double-free at scope end. True for String, List(T),
// StringMap(V), IntMap(V), and any user struct with at least one field
// that needs deep copy. False for primitives, view/mod types, c_struct
// (raylib structs), Any. Recurses through struct fields.
bool type_needs_deep_copy(CompilerContext* cctx, const AstModule* module,
                          const AstTypeRef* type, int depth);

#endif /* C_BACKEND_H */
