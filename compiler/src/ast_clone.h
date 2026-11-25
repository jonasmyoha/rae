/* ast_clone.h - copies of statements and expressions (docs/generic-instances-design.md section 2) */
#ifndef AST_CLONE_H
#define AST_CLONE_H

#include "ast.h"

/* A copy in which every node is new: nothing of it is shared with what it was
 * copied from, and it is not analyzed (no resolved type, no declaration). With
 * `params`, the copy is of a generic function's instance: each type parameter is
 * written out as the type in `args`, in the order of `params`. */
typedef struct {
  CompilerContext* ctx;
  const AstIdentifierPart* params;  /* NULL: nothing is written out */
  const AstTypeRef* args;
} AstCloner;

AstTypeRef* ast_clone_type_ref(const AstCloner* cloner, const AstTypeRef* type);
AstExpr* ast_clone_expr(const AstCloner* cloner, const AstExpr* expr);
AstBlock* ast_clone_block(const AstCloner* cloner, const AstBlock* block);

/* RAE_CHECK_CLONES=1: no node of `copy` is one of `source`, and no type parameter
 * is named in it. Reports what it finds and ends the compiler: a fault of its own. */
void ast_clone_check(const AstCloner* cloner, const AstBlock* source, const AstBlock* copy, Str name);

#endif
