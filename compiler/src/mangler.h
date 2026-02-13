#ifndef MANGLER_H
#define MANGLER_H

#include "ast.h"
#include <stdio.h>

/**
 * Mangles a type reference into a consistent C identifier.
 * Returns an arena-allocated string from ctx->ast_arena.
 */
const char* rae_mangle_type(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* type);

/**
 * Mangles a type reference into a consistent C identifier, performing substitution 
 * of generic parameters with concrete arguments.
 */
const char* rae_mangle_type_specialized(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* concrete_args, const AstTypeRef* type);

/**
 * Mangles a function declaration into a consistent C identifier.
 * Returns an arena-allocated string from ctx->ast_arena.
 */
const char* rae_mangle_function(CompilerContext* ctx, const AstFuncDecl* func);

/**
 * Mangles a specialized function instantiation.
 */
const char* rae_mangle_specialized_function(CompilerContext* ctx, const AstFuncDecl* func, const AstTypeRef* concrete_args);

/**
 * Version of type mangling that allows forcing erasure to Any_.
 */
const char* rae_mangle_type_ext(CompilerContext* ctx, const AstIdentifierPart* generic_params, const AstTypeRef* type, bool force_erase);

/**
 * Checks if a type name refers to a Rae primitive type.
 */
bool is_primitive_type(Str type_name);

/**
 * Checks if a type name refers to a Raylib built-in type.
 */
bool is_raylib_builtin_type(Str type_name);

/**
 * Maps a Rae primitive type name to its C equivalent.
 * Returns NULL if not a primitive.
 */
const char* map_rae_type_to_c(Str type_name);

/**
 * Returns the Raylib C function name for a given Rae name, or NULL if not a Raylib function.
 */
const char* find_raylib_mapping(Str name);

#endif /* MANGLER_H */
