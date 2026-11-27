/* c_results.c - the C of a function with several labeled results (sema_results.inc).
 *
 * Its results are one struct, returned by value, with a field per label:
 *
 *   typedef struct { int64_t quotient; int64_t remainder; } rae_Main_divide_..._results;
 *
 * A `ret` fills it, and a destructuring statement reads each field into its local.
 * A function with one result is not touched by any of this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_backend_internal.h"
#include "mangler.h"
#include "ownership.h"

bool c_has_several_results(const AstFuncDecl* func) {
  return func && func->returns && func->returns->next;
}

/* The struct's name: the function's C name and `_results`. An instance of a
 * generic function is a function of its own, with its own name. */
const char* c_results_type_name(CFuncContext* ctx, const AstFuncDecl* func) {
  CompilerContext* compiler = ctx->compiler_ctx;
  const char* mangled = rae_mangle_function(compiler, func);
  size_t length = strlen(mangled) + sizeof "_results";
  char* name = arena_alloc(compiler->ast_arena, length);
  snprintf(name, length, "%s_results", mangled);
  return name;
}

/* The names of the structs written so far, of this compilation */
static const char** g_written;
static size_t g_written_count, g_written_cap;

void c_results_reset(void) {
  g_written_count = 0;
}

/* The struct of `func`'s results, once, before its prototype. `ctx` is the
 * function's own (an instance's type arguments in it). */
void c_emit_results_type(CFuncContext* ctx, const AstFuncDecl* func, FILE* out) {
  if (!c_has_several_results(func) || func->is_extern) return;
  const char* name = c_results_type_name(ctx, func);
  for (size_t i = 0; i < g_written_count; i++)
    if (strcmp(g_written[i], name) == 0) return;
  if (g_written_count == g_written_cap) {
    g_written_cap = g_written_cap ? g_written_cap * 2 : 16;
    g_written = realloc(g_written, g_written_cap * sizeof *g_written);
  }
  g_written[g_written_count++] = name;
  fprintf(out, "typedef struct {");
  for (const AstReturnItem* result = func->returns; result; result = result->next) {
    fprintf(out, " ");
    emit_type_ref_as_c_type(ctx, result->type, out, false);
    fprintf(out, " %.*s;", (int)result->name.len, result->name.data);
  }
  fprintf(out, " } %s;\n", name);
}

/* One per plain function with several results, before the prototypes */
void c_emit_results_types(CompilerContext* ctx, const AstModule* module, FILE* out) {
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* decl = ctx->all_decls[i];
    if (decl->kind != AST_DECL_FUNC) continue;
    const AstFuncDecl* func = &decl->as.func_decl;
    if (func->generic_params || func->specialization_args) continue;
    CFuncContext fctx = { .compiler_ctx = ctx, .module = module, .func_decl = func };
    c_emit_results_type(&fctx, func, out);
  }
}

/* A value a `ret` gives that is a local of this function which owns what it holds:
 * it moves out, as the one value of `ret name` does. (One a `defer` still uses, a
 * parameter that only borrows and an alias of a list's slot are copied.) */
static bool moves_out(CFuncContext* ctx, const AstStmt* ret, const AstExpr* value) {
  if (!value || value->kind != AST_EXPR_IDENT) return false;
  /* (named by another value of this `ret` too: each gets its copy) */
  if (c_stmt_ident_refs(ret, value->as.ident) != 1) return false;
  int local = local_index_by_name(ctx, value->as.ident);
  if (local < 0) return false;
  const AstTypeRef* type = ctx->local_type_refs[local];
  if (!type || type->is_view || type->is_mod) return false;
  bool parameter = ctx->func_first_let_idx != (size_t)-1 && (size_t)local < ctx->func_first_let_idx;
  if (parameter && !type->is_own && !type->is_copy) return false;
  if (c_defer_mentions(ctx, value->as.ident) || c_local_is_slot_alias(ctx, local)) return false;
  return type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0);
}

/* `ret quotient: a / b, text: name`: each value becomes a hidden local of its
 * result's type, written as a `let` is (a place is copied, a call's result and an
 * `own` are taken, an optional is boxed), in the order the `ret` gives them; the
 * struct is made of those, which are then the caller's. False when the function
 * has one result. */
bool c_emit_ret_results(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  const AstFuncDecl* func = ctx->func_decl;
  if (!c_has_several_results(func)) return false;
  Arena* arena = ctx->compiler_ctx->ast_arena;
  /* `ret socketRead(...)`, whose results are this function's own: the call's struct,
   * field by field into this function's (each is a C type of its own) */
  const AstReturnArg* only = stmt->as.ret_stmt.values;
  if (only && !only->next && !only->has_label && only->value
      && (only->value->kind == AST_EXPR_CALL || only->value->kind == AST_EXPR_METHOD_CALL)) {
    int id = ctx->temp_counter++;
    bool had_expected = ctx->has_expected_type;
    ctx->has_expected_type = false;
    fprintf(out, "    __auto_type __rae_passed%d = ", id);
    emit_expr(ctx, only->value, out, PREC_LOWEST, false, false);
    fprintf(out, ";\n");
    ctx->has_expected_type = had_expected;
    fprintf(out, "    %s __ret_val = {", c_results_type_name(ctx, func));
    for (const AstReturnItem* result = func->returns; result; result = result->next)
      fprintf(out, " .%.*s = __rae_passed%d.%.*s,", (int)result->name.len, result->name.data, id,
              (int)result->name.len, result->name.data);
    fprintf(out, " };\n");
    return true;
  }
  for (const AstReturnArg* given = stmt->as.ret_stmt.values; given; given = given->next) {
    const AstReturnItem* result = NULL;
    for (const AstReturnItem* r = func->returns; r && !result; r = r->next)
      if (given->has_label && str_eq(given->label, r->name)) result = r;
    if (!result || !given->value) continue;  /* (sema reported it) */
    AstExpr* value = given->value;
    if (moves_out(ctx, stmt, value)) {
      AstExpr* moved = arena_alloc(arena, sizeof(AstExpr));
      *moved = (AstExpr){ .kind = AST_EXPR_OWN, .resolved_type = value->resolved_type, .line = value->line,
                          .column = value->column };
      moved->as.unary.operand = value;
      moved->as.unary.moves_local = true;
      value = moved;
    }
    size_t length = result->name.len + sizeof "__rae_result_";
    char* name = arena_alloc(arena, length);
    snprintf(name, length, "__rae_result_%.*s", (int)result->name.len, result->name.data);
    AstStmt* hidden = arena_alloc(arena, sizeof(AstStmt));
    *hidden = (AstStmt){ .kind = AST_STMT_LET, .line = stmt->line, .column = stmt->column, .is_synthetic = true };
    hidden->as.let_stmt.name = str_from_cstr(name);
    hidden->as.let_stmt.type = result->type;
    hidden->as.let_stmt.value = value;
    emit_stmt(ctx, hidden, out);
    /* the struct takes it: this function does not drop it */
    int local = local_index_by_name(ctx, hidden->as.let_stmt.name);
    if (local >= 0) ctx->local_moved[local] = true;
  }
  fprintf(out, "    %s __ret_val = {", c_results_type_name(ctx, func));
  for (const AstReturnItem* result = func->returns; result; result = result->next)
    fprintf(out, " .%.*s = __rae_result_%.*s,", (int)result->name.len, result->name.data,
            (int)result->name.len, result->name.data);
  fprintf(out, " };\n");
  return true;
}

/* The locals of a destructuring statement, as the function's other locals are
 * known. With `out` (the statement is being emitted) each owns what it holds, as a
 * `let` from a call does, and gets its live flag when a later statement moves it on
 * some paths only. */
void c_register_destructured_locals(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
    if (ctx->local_count >= 256 || !b->type) continue;
    size_t slot = ctx->local_count++;
    ctx->locals[slot] = b->local_name;
    ctx->local_type_refs[slot] = b->type;
    ctx->local_types[slot] = str_from_cstr(
        rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, b->type));
    ctx->local_is_ptr[slot] = false;
    ctx->local_is_mod[slot] = false;
    ctx->local_drop_flag[slot] = false;
    ctx->local_moved[slot] = false;
    ctx->local_struct_owns_heap[slot] = true;
    ctx->local_slot_alias[slot] = NULL;
    if (out) c_declare_live_flag(ctx, b->local_name, stmt->next, out);
  }
}

/* `let quotient: Int = quotient, var rest: Int = remainder = divide(a: 7, b: 2)`:
 * the call once, into the struct, then each local from its field */
void c_emit_destructure(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  const AstExpr* call = stmt->as.destruct_stmt.call;
  if (!call) return;
  /* (the struct's type is the call's own: of a generic function it is the
   * instance's, which only the call's emission names) */
  int id = ctx->temp_counter++;
  bool had_expected = ctx->has_expected_type;
  ctx->has_expected_type = false;
  if (ctx->stmt_may_pool) fprintf(out, "  int __rae_spm_results%d = rae_string_pool_mark();\n", id);
  fprintf(out, "  __auto_type __rae_results%d = ", id);
  emit_expr(ctx, call, out, PREC_LOWEST, false, false);
  fprintf(out, ";\n");
  if (ctx->stmt_may_pool) fprintf(out, "  rae_string_pool_flush(__rae_spm_results%d);\n", id);
  ctx->has_expected_type = had_expected;
  for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
    fprintf(out, "  ");
    emit_type_ref_as_c_type(ctx, b->type, out, false);
    fprintf(out, " %.*s = __rae_results%d.%.*s;\n", (int)b->local_name.len, b->local_name.data, id,
            (int)b->return_label.len, b->return_label.data);
  }
  c_register_destructured_locals(ctx, stmt, out);
}
