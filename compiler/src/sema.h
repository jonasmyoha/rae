#ifndef RAE_SEMA_H
#define RAE_SEMA_H

#include "ast.h"
#include <stdbool.h>

// Main entry point for semantic analysis
// Returns true if analysis succeeded, false if errors were found
bool sema_analyze_module(CompilerContext* ctx, AstModule* module);

// Per-file import/open scope (docs/module-namespacing.md). Call reset once, then
// register each source file's import directives (from the module graph) before
// sema, so name resolution can honour per-file aliases and import/open.
void sema_reset_file_scopes(void);
void sema_register_file_imports(Arena* arena, const char* file, AstImport* imports);
// Register a prelude package (auto-loaded => opened for every file).
void sema_register_global_open(Arena* arena, const char* module);

// Resolves a type reference (AstTypeRef) to a canonical TypeInfo*
TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref);

// Specialization helpers used by backends
AstTypeRef* substitute_type_ref(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type);

// #773 compile-time field reflection through a generic world parameter. When a
// generic function's body contains a `loop ... in fields(world)`, the field set
// depends on the concrete instantiation, so a backend emitting one concrete
// specialization must expand the loop AFTER substitution. Returns a CLONE of
// `template_body` with every fields() loop unrolled for the given concrete
// generic args, or NULL if the body has no fields() loop (emit the body as-is).
AstBlock* reflect_instantiate_body(CompilerContext* ctx, const AstModule* module,
        const AstBlock* template_body, const AstParam* params,
        const AstIdentifierPart* gparams, const AstTypeRef* concrete_args);
AstTypeRef* infer_generic_args(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* pattern, const AstTypeRef* concrete_type);
AstTypeRef* infer_generic_args_multi(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef** patterns, const AstTypeRef** concretes, size_t pair_count);
Str get_base_type_name(const AstTypeRef* type);
Str get_decl_name(const AstDecl* d);

#endif // RAE_SEMA_H
