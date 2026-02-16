#ifndef RAE_SEMA_H
#define RAE_SEMA_H

#include "ast.h"
#include <stdbool.h>

// Main entry point for semantic analysis
// Returns true if analysis succeeded, false if errors were found
bool sema_analyze_module(CompilerContext* ctx, AstModule* module);

// Resolves a type reference (AstTypeRef) to a canonical TypeInfo*
TypeInfo* sema_resolve_type(CompilerContext* ctx, AstTypeRef* type_ref);

#endif // RAE_SEMA_H
