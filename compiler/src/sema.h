#ifndef RAE_SEMA_H
#define RAE_SEMA_H

#include "ast.h"
#include <stdbool.h>

// Main entry point for semantic analysis
// Returns true if analysis succeeded, false if errors were found
bool sema_analyze_module(CompilerContext* ctx, AstModule* module);

// Resolves a type reference (AstTypeRef) to a canonical TypeInfo*
TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref);

// Specialization helpers used by backends
AstTypeRef* substitute_type_ref(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type);
AstTypeRef* infer_generic_args(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* pattern, const AstTypeRef* concrete_type);
Str get_base_type_name(const AstTypeRef* type);
Str get_decl_name(const AstDecl* d);

#endif // RAE_SEMA_H
