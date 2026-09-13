#ifndef C_BACKEND_H
#define C_BACKEND_H

#include <stdbool.h>

#include "ast.h"
#include "ownership.h"

bool c_backend_emit_module(CompilerContext* ctx, const AstModule* module, const char* out_path);
void register_generic_type(CompilerContext* ctx, const AstTypeRef* type);
void register_function_specialization(CompilerContext* ctx, const AstFuncDecl* decl, const AstTypeRef* concrete_args);

#endif /* C_BACKEND_H */
