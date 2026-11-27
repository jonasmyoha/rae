/* A call written as a statement (`words(count: 1)`): nobody keeps its result,
 * and one that owns memory (a List, a struct or an optional that holds some, a
 * Task) is dropped at the end of the statement. A String result is in the
 * statement's pool already. */
#include <stdio.h>
#include <string.h>

#include "c_backend_internal.h"
#include "c_binary.h"
#include "c_decl_index.h"
#include "ownership.h"
#include "sema.h"

/* The statement temporary that takes the result of a call of `fd` (with the type
 * arguments `concrete`, NULL for none), or -1: the call answers nothing, a
 * reference, an alias of what someone else owns, or what owns nothing. */
int c_discard_temp(CFuncContext* ctx, const AstFuncDecl* fd, const AstTypeRef* concrete) {
  if (!fd || !fd->returns || fd->returns->next || !fd->returns->type || !ctx->stmt_temps) return -1;
  const AstFuncDecl* body = fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC
      ? &fd->generic_template->as.func_decl : fd;
  const AstTypeRef* type = fd->returns->type;
  if (body->generic_params && concrete)
    type = substitute_type_ref(ctx->compiler_ctx, body->generic_params, concrete, body->returns->type);
  if (!type || type->is_view || type->is_mod) return -1;
  if (str_eq_cstr(get_base_type_name(type), "String") && !type->is_opt) return -1;
  /* C hands over handles and plain structs, whose owner is the code around it */
  if (fd->is_extern) return -1;
  /* (an optional made from an alias holds a copy) */
  if (!type->is_opt && rae_func_returns_alias(ctx->compiler_ctx, body)) return -1;
  if (!type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0)) return -1;
  AstTypeRef* result = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
  *result = *type;
  result->is_own = false;
  result->is_copy = false;
  result->next = NULL;
  return register_stmt_temp(ctx, result, true);
}

/* The same for a call of the language's own, which no function is behind:
 * `task.get()` and `tryGet()` (they take the result), `enumMembers(E)`,
 * `T.fromJson(...)`, `value.toBinary()` and `T.fromBinary(...)`. */
int c_discard_builtin_temp(CFuncContext* ctx, const AstExpr* expr) {
  if (!expr || !ctx->stmt_temps) return -1;
  const AstTypeRef* type = NULL;
  if (expr->kind == AST_EXPR_CALL && c_call_enum_members_type(expr)) {
    type = infer_expr_type_ref(ctx, expr);
  } else if (expr->kind == AST_EXPR_METHOD_CALL) {
    Str name = expr->as.method_call.method_name;
    const TypeInfo* receiver = expr->as.method_call.object ? expr->as.method_call.object->resolved_type : NULL;
    while (receiver && receiver->kind == TYPE_REF) receiver = receiver->as.ref.base;
    bool is_from = false;
    if (receiver && receiver->kind == TYPE_TASK && receiver->as.task.base
        && (str_eq_cstr(name, "get") || str_eq_cstr(name, "tryGet"))) {
      AstTypeRef* result = sema_type_ref_of(ctx->compiler_ctx, receiver->as.task.base);
      if (str_eq_cstr(name, "tryGet")) {
        result->is_opt = true;
        result->resolved_type = expr->resolved_type;
      }
      type = result;
    } else if (c_binary_call_type(ctx, expr, &is_from)) {
      type = infer_expr_type_ref(ctx, expr);
    } else if (str_eq_cstr(name, "fromJson") && !expr->decl_link
               && expr->as.method_call.object->kind == AST_EXPR_IDENT) {
      AstTypeRef* named = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
      memset(named, 0, sizeof(*named));
      named->parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart));
      memset(named->parts, 0, sizeof(*named->parts));
      named->parts->text = expr->as.method_call.object->as.ident;
      type = named;
      const AstDecl* decl = find_type_decl(ctx, ctx->module, get_base_type_name(type));
      if (!decl || decl->kind != AST_DECL_TYPE || (decl->as.type_decl.generic_params && !type->generic_args)) return -1;
    }
  }
  if (!type || type->is_view || type->is_mod) return -1;
  if (str_eq_cstr(get_base_type_name(type), "String") && !type->is_opt) return -1;
  if (!type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0)) return -1;
  return register_stmt_temp(ctx, type, true);
}

/* Is the result of the method call `call` a value of its own (`map.values()`)?
 * Of the functions it could mean (those of its name that take as many values),
 * none answers a reference or an alias. */
bool c_method_makes_its_result(CFuncContext* ctx, const AstExpr* call) {
  size_t values = 1;
  for (const AstCallArg* a = call->as.method_call.args; a; a = a->next) values++;
  size_t count = 0, found = 0;
  const size_t* named = decl_index_functions(ctx->compiler_ctx, call->as.method_call.method_name, &count);
  for (size_t k = 0; k < count; k++) {
    const AstFuncDecl* fd = &ctx->compiler_ctx->all_decls[named[k]]->as.func_decl;
    if (fd->specialization_args) continue;
    size_t params = 0;
    for (const AstParam* p = fd->params; p; p = p->next) params++;
    if (params != values) continue;
    if (!fd->body || !fd->returns || !fd->returns->type || fd->returns->type->is_view || fd->returns->type->is_mod
        || rae_func_returns_alias(ctx->compiler_ctx, fd)) return false;
    found++;
  }
  return found > 0;
}
