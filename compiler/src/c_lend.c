/* Spawns that lend a view to their task (docs/lightweight-spawn-design.md
 * §14.2). Inside a taskScope, sema lets a spawn pass a `view` of non-scalar
 * data when the place outlives the scope, and freezes it until the scope
 * ends (sema_spawn_lends). Here:
 *
 *   - each lent-to function gets an argument pack whose fields have its
 *     parameters' own C types (a view is the pointer a normal call passes),
 *     made by `__raelend_pack_F(...)`, which takes exactly the arguments the
 *     normal call does: the spawn site emits the normal call's text with the
 *     function swapped for the pack, so every argument is lowered (borrowed,
 *     copied, moved) as for a call;
 *   - the taskScope keeps a task group (RaeTaskGroup) that counts its lending
 *     tasks and waits for them before the scope ends, and before a `ret`,
 *     `break` or `continue` leaves it. So the borrow ends before the lent
 *     place can change or be freed, wherever the task's handle went.
 *
 * A lending task runs on the scheduler's pool when it can (it waits only on
 * other tasks), and on a thread of its own otherwise. */
#include "c_backend_internal.h"
#include "may_wait.h"
#include "mangler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----- the functions lent to --------------------------------------------- */

typedef struct {
  const AstDecl** decls;
  size_t count;
  size_t cap;
} LendTargets;

static void add_target(LendTargets* targets, const AstDecl* decl) {
  for (size_t i = 0; i < targets->count; i++) {
    if (targets->decls[i] == decl) return;
  }
  if (targets->count == targets->cap) {
    targets->cap = targets->cap ? targets->cap * 2 : 8;
    targets->decls = realloc(targets->decls, targets->cap * sizeof(AstDecl*));
  }
  targets->decls[targets->count++] = decl;
}

static void walk_block(const AstBlock* block, LendTargets* targets);

static void walk_expr(const AstExpr* expr, LendTargets* targets) {
  if (!expr) return;
  switch (expr->kind) {
    case AST_EXPR_UNARY: {
      if (expr->as.unary.op == AST_UNARY_SPAWN && expr->as.unary.spawn_lends) {
        const AstExpr* call = expr->as.unary.operand;
        if (call && call->kind == AST_EXPR_CALL && call->decl_link && call->decl_link->kind == AST_DECL_FUNC) {
          add_target(targets, call->decl_link);
        }
      }
      walk_expr(expr->as.unary.operand, targets);
      return;
    }
    case AST_EXPR_OWN: case AST_EXPR_BOX: case AST_EXPR_UNBOX:
      walk_expr(expr->as.unary.operand, targets);
      return;
    case AST_EXPR_BINARY:
      walk_expr(expr->as.binary.lhs, targets);
      walk_expr(expr->as.binary.rhs, targets);
      return;
    case AST_EXPR_CAST:
      walk_expr(expr->as.cast.operand, targets);
      return;
    case AST_EXPR_CALL:
      for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) walk_expr(arg->value, targets);
      return;
    case AST_EXPR_METHOD_CALL:
      walk_expr(expr->as.method_call.object, targets);
      for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) walk_expr(arg->value, targets);
      return;
    case AST_EXPR_MEMBER:
      walk_expr(expr->as.member.object, targets);
      return;
    case AST_EXPR_INDEX:
      walk_expr(expr->as.index.target, targets);
      walk_expr(expr->as.index.index, targets);
      return;
    case AST_EXPR_INTERP:
      for (const AstInterpPart* part = expr->as.interp.parts; part; part = part->next) walk_expr(part->value, targets);
      return;
    case AST_EXPR_OBJECT:
      for (const AstObjectField* field = expr->as.object_literal.fields; field; field = field->next) {
        walk_expr(field->value, targets);
      }
      return;
    case AST_EXPR_LIST:
      for (const AstExprList* item = expr->as.list; item; item = item->next) walk_expr(item->value, targets);
      return;
    case AST_EXPR_COLLECTION_LITERAL:
      for (const AstCollectionElement* element = expr->as.collection.elements; element; element = element->next) {
        walk_expr(element->value, targets);
      }
      return;
    case AST_EXPR_MATCH:
      walk_expr(expr->as.match_expr.subject, targets);
      for (const AstMatchArm* arm = expr->as.match_expr.arms; arm; arm = arm->next) walk_expr(arm->value, targets);
      return;
    default:
      return;
  }
}

static void walk_stmt(const AstStmt* stmt, LendTargets* targets) {
  switch (stmt->kind) {
    case AST_STMT_LET: walk_expr(stmt->as.let_stmt.value, targets); return;
    case AST_STMT_DESTRUCT: walk_expr(stmt->as.destruct_stmt.call, targets); return;
    case AST_STMT_EXPR: walk_expr(stmt->as.expr_stmt, targets); return;
    case AST_STMT_RET:
      for (const AstReturnArg* arg = stmt->as.ret_stmt.values; arg; arg = arg->next) walk_expr(arg->value, targets);
      return;
    case AST_STMT_ASSIGN:
      walk_expr(stmt->as.assign_stmt.target, targets);
      walk_expr(stmt->as.assign_stmt.value, targets);
      return;
    case AST_STMT_IF:
      if (stmt->as.if_stmt.binding) walk_stmt(stmt->as.if_stmt.binding, targets);
      walk_expr(stmt->as.if_stmt.condition, targets);
      walk_block(stmt->as.if_stmt.then_block, targets);
      walk_block(stmt->as.if_stmt.else_block, targets);
      return;
    case AST_STMT_LOOP:
      if (stmt->as.loop_stmt.init) walk_stmt(stmt->as.loop_stmt.init, targets);
      walk_expr(stmt->as.loop_stmt.condition, targets);
      walk_expr(stmt->as.loop_stmt.increment, targets);
      walk_block(stmt->as.loop_stmt.body, targets);
      return;
    case AST_STMT_MATCH:
      if (stmt->as.match_stmt.binding) walk_stmt(stmt->as.match_stmt.binding, targets);
      walk_expr(stmt->as.match_stmt.subject, targets);
      for (const AstMatchCase* match_case = stmt->as.match_stmt.cases; match_case; match_case = match_case->next) {
        walk_block(match_case->block, targets);
      }
      return;
    case AST_STMT_DEFER: walk_block(stmt->as.defer_stmt.block, targets); return;
    case AST_STMT_UNSAFE: walk_block(stmt->as.unsafe_stmt.block, targets); return;
    default: return;
  }
}

static void walk_block(const AstBlock* block, LendTargets* targets) {
  if (!block) return;
  for (const AstStmt* stmt = block->first; stmt; stmt = stmt->next) walk_stmt(stmt, targets);
}

/* The pack and the thunk of every function a spawn lends to */
void c_lend_emit_helpers(CompilerContext* ctx, const AstModule* module, FILE* out) {
  LendTargets targets = { NULL, 0, 0 };
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* decl = ctx->all_decls[i];
    if (decl->kind == AST_DECL_FUNC && decl->as.func_decl.body) walk_block(decl->as.func_decl.body, &targets);
  }
  for (size_t i = 0; i < targets.count; i++) {
    const AstFuncDecl* func = &targets.decls[i]->as.func_decl;
    CFuncContext tctx = { .compiler_ctx = ctx, .module = module, .func_decl = func };
    const char* mangled = rae_mangle_function(ctx, func);
    const char* returns = c_return_type(&tctx, func);
    bool is_void = strcmp(returns, "void") == 0;
    int count = 0;
    fprintf(out, "typedef struct { ");
    for (const AstParam* param = func->params; param; param = param->next, count++) {
      emit_param_c_type(&tctx, param, out, false);
      fprintf(out, " f%d; ", count);
    }
    fprintf(out, "RaeTask* __task; RaeTaskGroup* __group; } __raelend_args_%s;\n", mangled);
    fprintf(out, "RAE_UNUSED static __raelend_args_%s* __raelend_pack_%s(", mangled, mangled);
    emit_param_list(&tctx, func->params, out, false);
    fprintf(out, ") {\n  __raelend_args_%s* __a = (__raelend_args_%s*)malloc(sizeof(__raelend_args_%s));\n", mangled,
            mangled, mangled);
    count = 0;
    for (const AstParam* param = func->params; param; param = param->next, count++) {
      fprintf(out, "  memcpy((void*)&__a->f%d, (const void*)&%.*s, sizeof(__a->f%d));\n", count, (int)param->name.len,
              param->name.data, count);
    }
    fprintf(out, "  return __a;\n}\n");
    fprintf(out, "RAE_UNUSED static void* __raelend_thunk_%s(void* __vp) {\n", mangled);
    fprintf(out, "  rae_thread_install_altstack();\n");
    fprintf(out, "  __raelend_args_%s* __a = (__raelend_args_%s*)__vp;\n  ", mangled, mangled);
    if (!is_void) fprintf(out, "*(%s*)__a->__task->result = ", returns);
    fprintf(out, "%s(", mangled);
    for (int k = 0; k < count; k++) fprintf(out, "%s__a->f%d", k ? ", " : "", k);
    fprintf(out, ");\n  RaeTaskGroup* __g = __a->__group;\n  rae_task_complete(__a->__task);\n  free(__a);\n"
                 "  rae_group_done(__g);\n  return ((void*)0);\n}\n");
  }
  free(targets.decls);
}

/* ----- the spawn site and the group ------------------------------------------ */

bool c_lend_emit_spawn(CFuncContext* ctx, const AstExpr* spawn, FILE* out) {
  if (ctx->lend_group_count == 0) return false;
  const AstExpr* call = spawn->as.unary.operand;
  const AstDecl* decl = call->decl_link;
  const AstFuncDecl* func = &decl->as.func_decl;
  const char* mangled = rae_mangle_function(ctx->compiler_ctx, func);
  /* The normal call's text, its function swapped for the pack */
  char* text = NULL;
  size_t length = 0;
  FILE* text_out = open_memstream(&text, &length);
  emit_expr(ctx, call, text_out, PREC_LOWEST, false, false);
  fclose(text_out);
  char head[512];
  snprintf(head, sizeof head, "%s(", mangled);
  const char* found = text ? strstr(text, head) : NULL;
  /* A temporary the call takes the address of lives only in the statement */
  if (!found || strstr(text, "__rae_pw_")) {
    free(text);
    return false;
  }
  int group = ctx->lend_group_ids[ctx->lend_group_count - 1];
  TypeInfo* result = spawn->resolved_type && spawn->resolved_type->kind == TYPE_TASK ? spawn->resolved_type->as.task.base : NULL;
  bool is_void = !result || result->kind == TYPE_VOID;
  fprintf(out, "({ __raelend_args_%s* __s = ", mangled);
  fwrite(text, 1, (size_t)(found - text), out);
  fprintf(out, "__raelend_pack_%s(%s; ", mangled, found + strlen(head));
  free(text);
  fprintf(out, "RaeTask* __t = rae_task_new(");
  if (is_void) {
    fprintf(out, "0");
  } else {
    fprintf(out, "sizeof(");
    emit_type_info_as_c_type(ctx, result, out);
    fprintf(out, ")");
  }
  fprintf(out, "); __s->__task = __t; __s->__group = &__rae_group%d; rae_group_add(&__rae_group%d); ", group, group);
  CompilerContext* compiler = ctx->compiler_ctx;
  const char* submit = core_function_mangled(compiler, "schedulerSubmit");
  const char* forced = getenv("RAE_SPAWN_THREADS");
  bool on_pool = submit && compiler->wait_graph && !(forced && forced[0] && strcmp(forced, "0") != 0) &&
      may_wait_spawn_on_pool(compiler->wait_graph, decl, false);
  if (on_pool) {
    fprintf(out, "rae_task_prepare(__t, __raelend_thunk_%s, __s); %s((int64_t)(intptr_t)__t); __t; })", mangled, submit);
  } else {
    fprintf(out, "rae_task_start(__t, __raelend_thunk_%s, __s); __t; })", mangled);
  }
  return true;
}

void c_lend_open_group(CFuncContext* ctx, FILE* out) {
  int id = (int)ctx->temp_counter++;
  fprintf(out, "  RaeTaskGroup __rae_group%d = {0};\n", id);
  if (ctx->lend_group_count < 8) {
    ctx->lend_group_ids[ctx->lend_group_count] = id;
    ctx->lend_group_loop_depth[ctx->lend_group_count] = ctx->loop_depth;
    ctx->lend_group_count++;
  }
  /* A resumable twin cannot suspend while tasks borrow its C locals */
  ctx->twin_no_suspend++;
}

void c_lend_close_group(CFuncContext* ctx, FILE* out) {
  if (ctx->lend_group_count == 0) return;
  ctx->lend_group_count--;
  fprintf(out, "  rae_group_wait(&__rae_group%d);\n", ctx->lend_group_ids[ctx->lend_group_count]);
  ctx->twin_no_suspend--;
}

/* Before a `ret`: every open group; before a `break` / `continue`: the
 * groups opened inside the loop it leaves */
void c_lend_wait_groups(CFuncContext* ctx, FILE* out, bool loop_exit) {
  for (int i = ctx->lend_group_count - 1; i >= 0; i--) {
    if (loop_exit && ctx->lend_group_loop_depth[i] < ctx->loop_depth) continue;
    fprintf(out, "  rae_group_wait(&__rae_group%d);\n", ctx->lend_group_ids[i]);
  }
}
