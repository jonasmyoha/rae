/* c_results.c - the C of a function with several labeled results (sema_results.inc).
 *
 * Its results are one struct, returned by value, with a field per label:
 *
 *   typedef struct { int64_t quotient; int64_t remainder; } rae_Main_divide_..._results;
 *
 * A `ret` fills it, and a destructuring statement reads each field into its local.
 * A function with one result is not touched by any of this. */
#include <stdio.h>
#include <string.h>

#include "c_backend_internal.h"
#include "mangler.h"

bool c_has_several_results(const AstFuncDecl* func) {
  return func && func->returns && func->returns->next;
}

const char* c_results_type_name(CompilerContext* ctx, const AstFuncDecl* func) {
  const char* mangled = rae_mangle_function(ctx, func);
  size_t length = strlen(mangled) + sizeof "_results";
  char* name = arena_alloc(ctx->ast_arena, length);
  snprintf(name, length, "%s_results", mangled);
  return name;
}

/* One typedef per plain function with several results, before the prototypes */
void c_emit_results_types(CompilerContext* ctx, const AstModule* module, FILE* out) {
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* decl = ctx->all_decls[i];
    if (decl->kind != AST_DECL_FUNC) continue;
    const AstFuncDecl* func = &decl->as.func_decl;
    if (!c_has_several_results(func) || func->generic_params || func->specialization_args || func->is_extern) continue;
    CFuncContext fctx = { .compiler_ctx = ctx, .module = module, .func_decl = func };
    fprintf(out, "typedef struct {");
    for (const AstReturnItem* result = func->returns; result; result = result->next) {
      fprintf(out, " ");
      emit_type_ref_as_c_type(&fctx, result->type, out, false);
      fprintf(out, " %.*s;", (int)result->name.len, result->name.data);
    }
    fprintf(out, " } %s;\n", c_results_type_name(ctx, func));
  }
}

/* `ret quotient: a / b, remainder: a % b`: the struct, each field from the value
 * given for its label. False when the function has one result. */
bool c_emit_ret_results(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  const AstFuncDecl* func = ctx->func_decl;
  if (!c_has_several_results(func)) return false;
  bool had_expected = ctx->has_expected_type;
  AstTypeRef saved_expected = ctx->expected_type;
  fprintf(out, "    %s __ret_val = {", c_results_type_name(ctx->compiler_ctx, func));
  for (const AstReturnItem* result = func->returns; result; result = result->next) {
    const AstReturnArg* given = NULL;
    for (const AstReturnArg* arg = stmt->as.ret_stmt.values; arg && !given; arg = arg->next)
      if (arg->has_label && str_eq(arg->label, result->name)) given = arg;
    if (!given || !given->value) continue;  /* (sema reported it) */
    fprintf(out, " .%.*s = ", (int)result->name.len, result->name.data);
    ctx->expected_type = *result->type;
    ctx->expected_type.next = NULL;
    ctx->has_expected_type = true;
    emit_expr(ctx, given->value, out, PREC_LOWEST, false, false);
    fprintf(out, ",");
  }
  fprintf(out, " };\n");
  ctx->has_expected_type = had_expected;
  ctx->expected_type = saved_expected;
  return true;
}

/* The locals of a destructuring statement, as the function's other locals are known */
void c_register_destructured_locals(CFuncContext* ctx, const AstStmt* stmt) {
  for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
    if (ctx->local_count >= 256 || !b->type) continue;
    size_t slot = ctx->local_count++;
    ctx->locals[slot] = b->local_name;
    ctx->local_type_refs[slot] = b->type;
    ctx->local_types[slot] = str_from_cstr(
        rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, b->type));
    ctx->local_is_ptr[slot] = false;
    ctx->local_is_mod[slot] = false;
    ctx->local_drop_flag[slot] = false;
    ctx->local_moved[slot] = false;
    ctx->local_struct_owns_heap[slot] = false;
  }
}

/* `let quotient: Int = quotient, var rest: Int = remainder = divide(a: 7, b: 2)`:
 * the call once, into the struct, then each local from its field */
void c_emit_destructure(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  const AstExpr* call = stmt->as.destruct_stmt.call;
  const AstFuncDecl* callee = call && call->decl_link && call->decl_link->kind == AST_DECL_FUNC
      ? &call->decl_link->as.func_decl : NULL;
  if (!c_has_several_results(callee)) return;  /* (sema reported it) */
  int id = ctx->temp_counter++;
  fprintf(out, "  %s __rae_results%d;\n", c_results_type_name(ctx->compiler_ctx, callee), id);
  bool had_expected = ctx->has_expected_type;
  ctx->has_expected_type = false;
  if (ctx->stmt_may_pool) fprintf(out, "  { int __rae_spm = rae_string_pool_mark(); __rae_results%d = ", id);
  else fprintf(out, "  __rae_results%d = ", id);
  emit_expr(ctx, call, out, PREC_LOWEST, false, false);
  fprintf(out, ctx->stmt_may_pool ? "; rae_string_pool_flush(__rae_spm); }\n" : ";\n");
  ctx->has_expected_type = had_expected;
  for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
    fprintf(out, "  ");
    emit_type_ref_as_c_type(ctx, b->type, out, false);
    fprintf(out, " %.*s = __rae_results%d.%.*s;\n", (int)b->local_name.len, b->local_name.data, id,
            (int)b->return_label.len, b->return_label.data);
  }
  c_register_destructured_locals(ctx, stmt);
}
