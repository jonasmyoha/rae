// c_stmt.c — Statement emission for the C backend.
//
// `emit_stmt` is the per-AST-node switch for Rae statements. Helper emitters
// for `if`, `for`-style loops, and `match` live here too; defer-stack
// bookkeeping (used by ret/scope-exit) is also here since it's purely
// statement-scoped state.

#include "c_backend.h"
#include "c_backend_internal.h"
#include "diag.h"
#include "mangler.h"
#include "ownership.h"
#include "sema.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// File-local helpers.
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
void emit_optional_boxed_expr(CFuncContext* ctx, const AstTypeRef* opt_type,
                              const AstExpr* value, FILE* out);

/* `if let element ... list.at/viewAt/modAt(index:)` is the hot-path spelling
 * of checked indexing. Lower it directly instead of calling the generic Rae
 * helper, whose ordinary function prologue carries temp-pool bookkeeping and
 * whose owned optional representation is RaeAny. The language semantics stay
 * optional; this only removes representation work that the branch makes
 * unnecessary. */
static bool emit_list_if_let(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  const AstStmt* binding = stmt->as.if_stmt.binding;
  if (!binding || binding->kind != AST_STMT_LET || !binding->as.let_stmt.value
      || binding->as.let_stmt.value->kind != AST_EXPR_METHOD_CALL
      || !binding->as.let_stmt.type) return false;
  const AstExpr* call = binding->as.let_stmt.value;
  Str method = call->as.method_call.method_name;
  bool owned = str_eq_cstr(method, "copyAt");
  bool viewed = str_eq_cstr(method, "viewAt");
  bool modified = str_eq_cstr(method, "modAt");
  if (!owned && !viewed && !modified) return false;
  const AstTypeRef* list_type = infer_expr_type_ref(ctx, call->as.method_call.object);
  if (!list_type || !str_eq_cstr(get_base_type_name(list_type), "List")) return false;
  const AstCallArg* index_arg = call->as.method_call.args;
  while (index_arg && !str_eq_cstr(index_arg->name, "index")) index_arg = index_arg->next;
  if (!index_arg || !index_arg->value) return false;

  int fast_id = ctx->temp_counter++;
  bool list_is_ref = list_type->is_view || list_type->is_mod;
  fprintf(out, "  {\n    __auto_type __rae_list%d = ", fast_id);
  if (!list_is_ref) fprintf(out, "&(");
  emit_expr(ctx, call->as.method_call.object, out, PREC_LOWEST, false, true);
  if (!list_is_ref) fprintf(out, ")");
  fprintf(out, ";\n    int64_t __rae_index%d = ", fast_id);
  emit_expr(ctx, index_arg->value, out, PREC_LOWEST, false, false);
  fprintf(out, ";\n    if ((uint64_t)__rae_index%d < (uint64_t)__rae_list%d->length) {\n      ",
          fast_id, fast_id);

  const AstTypeRef* element_type = binding->as.let_stmt.type;
  AstTypeRef value_type = *element_type;
  value_type.is_opt = false;
  value_type.is_view = false;
  value_type.is_mod = false;
  value_type.resolved_type = NULL;
  bool is_ref = element_type->is_view || element_type->is_mod;
  bool primitive_ref = is_ref && is_primitive_type(get_base_type_name(element_type));
  bool string_value = !is_ref && str_eq_cstr(get_base_type_name(element_type), "String");
  bool deep_value = !is_ref && !string_value
      && type_needs_deep_copy(ctx->compiler_ctx, ctx->module, &value_type, 0);

  emit_type_ref_as_c_type(ctx, element_type, out, false);
  fprintf(out, " %.*s", (int)binding->as.let_stmt.name.len,
          binding->as.let_stmt.name.data);
  if (deep_value) {
    const char* type_name = rae_mangle_type_specialized(
        ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &value_type);
    fprintf(out, ";\n      rae_deep_copy_%s(&%.*s, &__rae_list%d->data[__rae_index%d])",
            type_name, (int)binding->as.let_stmt.name.len,
            binding->as.let_stmt.name.data, fast_id, fast_id);
  } else {
    fprintf(out, " = ");
    if (primitive_ref) fprintf(out, "{ .ptr = &");
    else if (is_ref) fprintf(out, "&");
    else if (string_value) fprintf(out, "rae_string_copy(");
    fprintf(out, "__rae_list%d->data[__rae_index%d]", fast_id, fast_id);
    if (primitive_ref) fprintf(out, " }");
    else if (string_value) fprintf(out, ")");
  }
  fprintf(out, ";\n");

  size_t saved_locals = ctx->local_count;
  if (ctx->local_count < 256) {
    size_t local_index = ctx->local_count++;
    ctx->locals[local_index] = binding->as.let_stmt.name;
    ctx->local_type_refs[local_index] = element_type;
    ctx->local_is_ptr[local_index] = is_ref;
    ctx->local_is_mod[local_index] = element_type->is_mod;
    ctx->local_moved[local_index] = false;
    ctx->local_struct_owns_heap[local_index] = !is_ref;
  }
  if (stmt->as.if_stmt.then_block) {
    for (const AstStmt* body_stmt = stmt->as.if_stmt.then_block->first;
         body_stmt; body_stmt = body_stmt->next) emit_stmt(ctx, body_stmt, out);
  }
  emit_implicit_drops_for_body(ctx, out, saved_locals);
  ctx->local_count = saved_locals;
  fprintf(out, "    }");
  if (stmt->as.if_stmt.else_block) {
    fprintf(out, " else {\n");
    size_t saved_else = ctx->local_count;
    for (const AstStmt* else_stmt = stmt->as.if_stmt.else_block->first;
         else_stmt; else_stmt = else_stmt->next) emit_stmt(ctx, else_stmt, out);
    emit_implicit_drops_for_body(ctx, out, saved_else);
    ctx->local_count = saved_else;
    fprintf(out, "    }");
  }
  fprintf(out, "\n  }\n");
  return true;
}

// Ownership classifiers (is_drop_target_type, type_owns_heap_storage,
// type_needs_cascade_drop, type_needs_deep_copy) moved to
// `ownership.{c,h}` so the Live VM emitter can share them with the C
// backend. Behaviour preserved exactly — see Stage 1 plan.
const AstFuncDecl* find_drop_overload_for(CFuncContext* ctx, Str container_base);

// Locate the `drop` generic-function overload whose receiver type's
// base name matches `container_base` ("List" / "StringMap" / "IntMap").
// Returns NULL if no overload exists — callers silently skip the drop
// emission, which is the Stage 1 fallback (no double-free, just a leak).
const AstFuncDecl* find_drop_overload_for(
    CFuncContext* ctx, Str container_base) {
  if (!ctx || !ctx->compiler_ctx) return NULL;
  for (size_t j = 0; j < ctx->compiler_ctx->all_decl_count; j++) {
    const AstDecl* d = ctx->compiler_ctx->all_decls[j];
    if (d->kind != AST_DECL_FUNC) continue;
    if (!str_eq_cstr(d->as.func_decl.name, "drop")) continue;
    if (!d->as.func_decl.generic_params) continue;
    const AstParam* first = d->as.func_decl.params;
    if (!first || !first->type) continue;
    Str dp_base = get_base_type_name(first->type);
    if (str_eq(dp_base, container_base)) return &d->as.func_decl;
  }
  return NULL;
}

static bool c_type_is_plain_string(const AstTypeRef* type) {
  if (!type || type->is_opt || type->is_view || type->is_mod) return false;
  return str_eq_cstr(get_base_type_name(type), "String");
}

static bool c_expr_can_move_owned_payload(const AstExpr* expr) {
  if (!expr) return false;
  switch (expr->kind) {
    case AST_EXPR_CALL:
    case AST_EXPR_METHOD_CALL:
    case AST_EXPR_OBJECT:
    case AST_EXPR_INTERP:
    case AST_EXPR_BINARY:
    case AST_EXPR_OWN:
      return true;
    default:
      return false;
  }
}

static bool c_expr_is_extern_opt_string_call(const AstExpr* expr) {
  if (!expr || (expr->kind != AST_EXPR_CALL && expr->kind != AST_EXPR_METHOD_CALL)
      || !expr->decl_link) return false;
  if (expr->decl_link->kind != AST_DECL_FUNC) return false;
  const AstFuncDecl* fd = &expr->decl_link->as.func_decl;
  if (!fd->is_extern || !fd->returns || !fd->returns->type) return false;
  if (!fd->returns->type->is_opt) return false;
  return str_eq_cstr(get_base_type_name(fd->returns->type), "String");
}

static bool c_expr_is_nonextern_opt_call(const AstExpr* expr) {
  if (!expr || (expr->kind != AST_EXPR_CALL && expr->kind != AST_EXPR_METHOD_CALL)
      || !expr->decl_link) return false;
  if (expr->decl_link->kind != AST_DECL_FUNC) return false;
  const AstFuncDecl* fd = &expr->decl_link->as.func_decl;
  return !fd->is_extern && fd->returns && fd->returns->type
      && fd->returns->type->is_opt;
}

static const char* c_optional_payload_drop_fn(CFuncContext* ctx,
                                              const AstTypeRef* payload) {
  if (!ctx || !payload) return NULL;
  if (!type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, payload, 0)) {
    return NULL;
  }
  if (is_drop_target_type(payload)) {
    const AstTypeRef* elem_type = payload->generic_args;
    if (!elem_type) return NULL;
    Str loc_base = get_base_type_name(payload);
    const AstFuncDecl* drop_fd = find_drop_overload_for(ctx, loc_base);
    if (!drop_fd) return NULL;
    register_function_specialization(ctx->compiler_ctx, drop_fd, elem_type);
    return rae_mangle_specialized_function(ctx->compiler_ctx, drop_fd, elem_type);
  }
  return rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params,
                                     ctx->generic_args, payload);
}

static bool c_optional_payload_is_boxed_pointer(CFuncContext* ctx,
                                                const AstTypeRef* payload) {
  if (!payload || c_type_is_plain_string(payload)) return false;
  Str base = get_base_type_name(payload);
  if (base.len == 0) return false;
  if (is_primitive_type(base)) return false;
  if (str_eq_cstr(base, "Any") || str_eq_cstr(base, "RaeAny")) return false;
  (void)ctx;
  return true;
}

void emit_optional_boxed_expr(CFuncContext* ctx, const AstTypeRef* opt_type,
                                     const AstExpr* value, FILE* out) {
  if (!value || value->kind == AST_EXPR_NONE) {
    if (opt_type && !(opt_type->is_view || opt_type->is_mod)
        && rae_opt_is_struct_rep(ctx, opt_type))
      fprintf(out, "(%s){0}", rae_opt_type_name(ctx, opt_type));
    else
      fprintf(out, "rae_any_none()");
    return;
  }
  if (value->kind == AST_EXPR_OWN) {
    const AstTypeRef* owned_tr = infer_expr_type_ref(ctx, value->as.unary.operand);
    if (owned_tr && owned_tr->is_opt) {
      emit_expr(ctx, value, out, PREC_LOWEST, false, false);
      return;
    }
  }
  /* Pass-throughs: the value is ALREADY an optional (a RaeAny box), so the
   * box transfers as-is. Suppress the call emitter's auto-unbox — without
   * this, an opt-returning call in opt-field position emitted
   * `call().as.s`, assigning the raw payload into the RaeAny field, which
   * does not compile (test 531). */
  if (c_expr_is_nonextern_opt_call(value)) {
    bool saved_unbox = ctx->suppress_opt_unbox;
    ctx->suppress_opt_unbox = true;
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    ctx->suppress_opt_unbox = saved_unbox;
    return;
  }
  if (value->kind == AST_EXPR_METHOD_CALL
      && (str_eq_cstr(value->as.method_call.method_name, "copyAt")
          || str_eq_cstr(value->as.method_call.method_name, "viewAt")
          || str_eq_cstr(value->as.method_call.method_name, "modAt"))) {
    bool saved_unbox = ctx->suppress_opt_unbox;
    ctx->suppress_opt_unbox = true;
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    ctx->suppress_opt_unbox = saved_unbox;
    return;
  }
  if (value->kind == AST_EXPR_CALL && value->as.call.callee
      && value->as.call.callee->kind == AST_EXPR_IDENT
      && str_eq_cstr(value->as.call.callee->as.ident, "copyAt")) {
    bool saved_unbox = ctx->suppress_opt_unbox;
    ctx->suppress_opt_unbox = true;
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    ctx->suppress_opt_unbox = saved_unbox;
    return;
  }
  // Sema may wrap a struct-rep opt-returning call in an UNBOX even where the
  // surrounding context wants the optional itself (e.g. `ret ps.at(i)` where
  // `at` returns `opt Particle`). Unwrap so the underlying `rae_opt_<T>` value
  // is passed through rather than re-wrapped as a payload into a new opt.
  if (value->kind == AST_EXPR_UNBOX) {
    const AstExpr* inner = value->as.unary.operand;
    const AstTypeRef* inner_tr = infer_expr_type_ref(ctx, inner);
    if (inner_tr && inner_tr->is_opt && !(inner_tr->is_view || inner_tr->is_mod)
        && rae_opt_is_struct_rep(ctx, inner_tr)) {
      bool saved_unbox = ctx->suppress_opt_unbox;
      ctx->suppress_opt_unbox = true;
      emit_expr(ctx, inner, out, PREC_LOWEST, false, false);
      ctx->suppress_opt_unbox = saved_unbox;
      return;
    }
  }
  const AstTypeRef* val_tr = infer_expr_type_ref(ctx, value);
  if (val_tr && val_tr->is_opt && !c_expr_is_extern_opt_string_call(value)) {
    bool saved_unbox = ctx->suppress_opt_unbox;
    ctx->suppress_opt_unbox = true;
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    ctx->suppress_opt_unbox = saved_unbox;
    return;
  }
  if (val_tr) {
    Str val_base = get_base_type_name(val_tr);
    if (str_eq_cstr(val_base, "Any") || str_eq_cstr(val_base, "RaeAny")) {
      emit_expr(ctx, value, out, PREC_LOWEST, false, false);
      return;
    }
  }

  AstTypeRef* substituted_opt = opt_type
      ? substitute_type_ref(ctx->compiler_ctx, ctx->generic_params,
                            ctx->generic_args, (AstTypeRef*)opt_type)
      : NULL;
  AstTypeRef payload = substituted_opt ? *substituted_opt
      : opt_type ? *opt_type : (AstTypeRef){0};
  payload.is_opt = false;
  payload.is_view = false;
  payload.is_mod = false;
  payload.resolved_type = NULL;

  // #651: every non-`Any` payload — scalars, String, enums (as Int) and
  // aggregates — builds the monomorphized `struct rae_opt_<T>` value in place
  // (no malloc, no RaeAny box). String copies its payload; aggregates deep-copy
  // when needed; scalars assign directly.
  AstTypeRef opt_for_name = payload; opt_for_name.is_opt = true;
  if (rae_opt_is_struct_rep(ctx, &opt_for_name)) {
    const char* optm = rae_opt_type_name(ctx, &opt_for_name);
    int optn = (int)ctx->temp_counter++;
    fprintf(out, "(__extension__ ({ %s __opt%d = {0}; __opt%d.has = 1; ",
            optm, optn, optn);
    if (c_type_is_plain_string(&payload)) {
      /* opt String owns its boxed payload independently — always copy the
       * source String so list/map aliases and string-pool temporaries cannot
       * outlive or double-own it. */
      fprintf(out, "__opt%d.value = rae_string_copy(", optn);
      emit_expr(ctx, value, out, PREC_LOWEST, false, false);
      fprintf(out, "); ");
    } else {
      bool needs_deep_agg = type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                                 &payload, 0);
      if (needs_deep_agg && !c_expr_can_move_owned_payload(value)) {
        const char* copy_name = rae_mangle_type_specialized(
            ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &payload);
        fprintf(out, "rae_deep_copy_%s(&__opt%d.value, &(", copy_name, optn);
        emit_expr(ctx, value, out, PREC_LOWEST, false, false);
        fprintf(out, ")); ");
      } else {
        fprintf(out, "__opt%d.value = ", optn);
        emit_expr(ctx, value, out, PREC_LOWEST, false, false);
        fprintf(out, "; ");
      }
    }
    fprintf(out, "__opt%d; }))", optn);
    return;
  }

  // Non-struct-rep payload (`Any`): keep the inline RaeAny box.
  if (c_type_is_plain_string(&payload)) {
    fprintf(out, "rae_any((rae_string_copy(");
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    fprintf(out, ")))");
    return;
  }

  if (!c_optional_payload_is_boxed_pointer(ctx, &payload)) {
    fprintf(out, "rae_any((");
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    fprintf(out, "))");
    return;
  }

  int tmpn = (int)ctx->temp_counter++;
  fprintf(out, "(__extension__ ({ ");
  emit_type_ref_as_c_type(ctx, &payload, out, false);
  fprintf(out, " __optv%d; ", tmpn);
  emit_type_ref_as_c_type(ctx, &payload, out, false);
  fprintf(out, "* __optp%d = (", tmpn);
  emit_type_ref_as_c_type(ctx, &payload, out, false);
  fprintf(out, "*)malloc(sizeof(");
  emit_type_ref_as_c_type(ctx, &payload, out, false);
  fprintf(out, ")); ");

  bool needs_deep = type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                         &payload, 0);
  if (needs_deep && !c_expr_can_move_owned_payload(value)) {
    const char* copy_name = rae_mangle_type_specialized(
        ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &payload);
    fprintf(out, "rae_deep_copy_%s(__optp%d, &(", copy_name, tmpn);
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    fprintf(out, ")); ");
  } else {
    fprintf(out, "__optv%d = ", tmpn);
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    fprintf(out, "; *__optp%d = __optv%d; ", tmpn, tmpn);
  }

  const char* drop_name = c_optional_payload_drop_fn(ctx, &payload);
  if (drop_name) {
    fprintf(out, "rae_any_owned_ptr(__optp%d, (RaeAnyDropFn)", tmpn);
    if (is_drop_target_type(&payload)) {
      fprintf(out, "%s", drop_name);
    } else {
      fprintf(out, "rae_drop_struct_%s", drop_name);
    }
    fprintf(out, "); }))");
  } else {
    fprintf(out, "rae_any_owned_ptr(__optp%d, NULL); }))", tmpn);
  }
}

// Classifies a function as alias-returning by walking its body for
// any return path that yields a buf_get-flavoured alias. Used at
// let-stmt time so `let v = call()` knows whether to mark v as
// owning (full cascade drop) or aliasing (skip String drops).
//
// A function returns an alias when ANY of these patterns reach a ret:
//
//   ret <ident>            where ident is bound by `let` to one of
//                          the aliasing forms.
//   ret <aliasing_call>    a direct call whose callee is itself
//                          alias-returning (transitive) or a
//                          buf_get intrinsic.
//
// Aliasing let-init forms:
//   let x = buf_get(...) / __buf_get(...) / rae_ext_rae_buf_get(...)
//   let x = <call to alias-returning user function>
//   let x = <other ident> (shallow copy — pure pass-through)
//
// Recursion has a depth cap + visited-set so mutually-recursive
// helpers don't infinite-loop. The result is conservative — a false
// "owning" leaks (no crash), a false "alias" causes the Phase 2
// String-copy in struct literals to leak (also no crash).
#define RAE_ALIAS_VISIT_MAX 32
typedef struct { const AstFuncDecl* fns[RAE_ALIAS_VISIT_MAX]; int n; } AliasVisit;
static bool visit_seen(AliasVisit* v, const AstFuncDecl* fd) {
  for (int i = 0; i < v->n; i++) if (v->fns[i] == fd) return true;
  return false;
}
static bool visit_push(AliasVisit* v, const AstFuncDecl* fd) {
  if (v->n >= RAE_ALIAS_VISIT_MAX) return false;
  v->fns[v->n++] = fd; return true;
}
static bool call_is_aliasing(CompilerContext* cctx, const AstExpr* call, AliasVisit* v);
static bool stmt_block_returns_alias_v(CompilerContext* cctx, const AstStmt* first, AliasVisit* v);
static bool func_returns_alias_v(CompilerContext* cctx, const AstFuncDecl* fd, AliasVisit* v);
static const AstStmt* find_let_for_ident(const AstStmt* scope_start, Str name) {
  for (const AstStmt* s = scope_start; s; s = s->next) {
    if (s->kind == AST_STMT_LET && str_eq(s->as.let_stmt.name, name)) return s;
  }
  return NULL;
}
static bool let_init_is_aliasing(CompilerContext* cctx, const AstStmt* let_s, AliasVisit* v) {
  if (!let_s) return false;
  const AstExpr* val = let_s->as.let_stmt.value;
  if (!val) return false;
  if (val->kind == AST_EXPR_IDENT) return true; // bare-ident copy → alias
  if (val->kind == AST_EXPR_CALL) return call_is_aliasing(cctx, val, v);
  return false;
}
static bool call_is_aliasing(CompilerContext* cctx, const AstExpr* call, AliasVisit* v) {
  const AstExpr* callee = call->as.call.callee;
  if (!callee || callee->kind != AST_EXPR_IDENT) return false;
  Str cn = callee->as.ident;
  if (str_eq_cstr(cn, "rae_ext_rae_buf_get") ||
      str_eq_cstr(cn, "__buf_get") ||
      str_eq_cstr(cn, "rae_ext___buf_get")) return true;
  // User function: recurse on its body.
  for (size_t k = 0; k < cctx->all_decl_count; k++) {
    const AstDecl* d = cctx->all_decls[k];
    if (d->kind != AST_DECL_FUNC) continue;
    if (!str_eq(d->as.func_decl.name, cn)) continue;
    return func_returns_alias_v(cctx, &d->as.func_decl, v);
  }
  return false;
}
static bool stmt_block_returns_alias_v(CompilerContext* cctx, const AstStmt* first, AliasVisit* v) {
  for (const AstStmt* s = first; s; s = s->next) {
    if (s->kind == AST_STMT_RET) {
      const AstReturnArg* vs = s->as.ret_stmt.values;
      if (vs && vs->value) {
        const AstExpr* rv = vs->value;
        if (rv->kind == AST_EXPR_IDENT) {
          const AstStmt* ls = find_let_for_ident(first, rv->as.ident);
          if (let_init_is_aliasing(cctx, ls, v)) return true;
        } else if (rv->kind == AST_EXPR_CALL) {
          if (call_is_aliasing(cctx, rv, v)) return true;
        }
        // NOTE: `ret <ident>.<member>` (and `ret <expr>[i]`) is NOT an
        // alias return. The ret codegen ALWAYS deep-copies member/index
        // String returns (see wrap_ret_string_copy in this file — a
        // member access could alias container storage the callee's
        // scope-exit drop would free, so it's copied unconditionally).
        // The result is therefore an OWNED, independent heap the CALLER
        // must drop. Classifying it as an alias (is_owned=0) orphaned the
        // copy and leaked it — the dominant #282 `copy`-site leak
        // (pageOf's `ret r.id`, jsonObjectKeyAt's `ret f.key`, ...). Only
        // a raw `ret <buf_get-call>` or `ret <non-view buf_get local>`
        // (handled above) genuinely passes an alias through without a copy.
      }
    } else if (s->kind == AST_STMT_IF) {
      if (s->as.if_stmt.then_block &&
          stmt_block_returns_alias_v(cctx, s->as.if_stmt.then_block->first, v)) return true;
      if (s->as.if_stmt.else_block &&
          stmt_block_returns_alias_v(cctx, s->as.if_stmt.else_block->first, v)) return true;
    } else if (s->kind == AST_STMT_LOOP) {
      if (s->as.loop_stmt.body &&
          stmt_block_returns_alias_v(cctx, s->as.loop_stmt.body->first, v)) return true;
    }
  }
  return false;
}
static bool func_returns_alias_v(CompilerContext* cctx, const AstFuncDecl* fd, AliasVisit* v) {
  if (!fd || !fd->body) return false;
  if (visit_seen(v, fd)) return false; // recursive call — assume owning to avoid infinite alias
  if (!visit_push(v, fd)) return false;
  return stmt_block_returns_alias_v(cctx, fd->body->first, v);
}
bool rae_func_returns_alias(CompilerContext* cctx, const AstFuncDecl* fd) {
  AliasVisit v = {0};
  return func_returns_alias_v(cctx, fd, &v);
}

// Count identifier references to `name` in an expression subtree.
// Used by Phase 2 deep-copy to decide whether a parameter source can
// be moved into a struct field (count==1, this is the only use) or
// must be deep-copied (count>=2, the param is read again later).
static int count_ident_refs_expr(const AstExpr* e, Str name);
static int count_ident_refs_stmt(const AstStmt* s, Str name);
static int count_ident_refs_block(const AstBlock* b, Str name) {
  if (!b) return 0;
  int total = 0;
  for (const AstStmt* s = b->first; s; s = s->next) {
    total += count_ident_refs_stmt(s, name);
  }
  return total;
}
static int count_ident_refs_args(const AstCallArg* a, Str name) {
  int total = 0;
  for (; a; a = a->next) total += count_ident_refs_expr(a->value, name);
  return total;
}
static int count_ident_refs_expr(const AstExpr* e, Str name) {
  if (!e) return 0;
  switch (e->kind) {
    case AST_EXPR_IDENT:
      return str_eq(e->as.ident, name) ? 1 : 0;
    case AST_EXPR_BINARY:
      return count_ident_refs_expr(e->as.binary.lhs, name) +
             count_ident_refs_expr(e->as.binary.rhs, name);
    case AST_EXPR_UNARY:
      return count_ident_refs_expr(e->as.unary.operand, name);
    case AST_EXPR_CAST:
      return count_ident_refs_expr(e->as.cast.operand, name);
    case AST_EXPR_CALL:
      return count_ident_refs_expr(e->as.call.callee, name) +
             count_ident_refs_args(e->as.call.args, name);
    case AST_EXPR_METHOD_CALL:
      return count_ident_refs_expr(e->as.method_call.object, name) +
             count_ident_refs_args(e->as.method_call.args, name);
    case AST_EXPR_MEMBER:
      return count_ident_refs_expr(e->as.member.object, name);
    case AST_EXPR_OBJECT: {
      int total = 0;
      for (const AstObjectField* f = e->as.object_literal.fields; f; f = f->next) {
        total += count_ident_refs_expr(f->value, name);
      }
      return total;
    }
    case AST_EXPR_LIST: {
      int total = 0;
      for (const AstExprList* l = e->as.list; l; l = l->next) {
        total += count_ident_refs_expr(l->value, name);
      }
      return total;
    }
    case AST_EXPR_INDEX:
      return count_ident_refs_expr(e->as.index.target, name) +
             count_ident_refs_expr(e->as.index.index, name);
    case AST_EXPR_COLLECTION_LITERAL: {
      int total = 0;
      for (const AstCollectionElement* el = e->as.collection.elements; el; el = el->next) {
        total += count_ident_refs_expr(el->value, name);
      }
      return total;
    }
    case AST_EXPR_INTERP: {
      int total = 0;
      for (const AstInterpPart* p = e->as.interp.parts; p; p = p->next) {
        total += count_ident_refs_expr(p->value, name);
      }
      return total;
    }
    case AST_EXPR_MATCH: {
      int total = count_ident_refs_expr(e->as.match_expr.subject, name);
      for (const AstMatchArm* a = e->as.match_expr.arms; a; a = a->next) {
        total += count_ident_refs_expr(a->pattern, name);
        total += count_ident_refs_expr(a->value, name);
      }
      return total;
    }
    case AST_EXPR_BOX:
    case AST_EXPR_UNBOX:
    case AST_EXPR_OWN:
      return count_ident_refs_expr(e->as.unary.operand, name);
    default:
      return 0;
  }
}
static int count_ident_refs_stmt(const AstStmt* s, Str name) {
  if (!s) return 0;
  switch (s->kind) {
    case AST_STMT_LET:
      return count_ident_refs_expr(s->as.let_stmt.value, name);
    case AST_STMT_DESTRUCT:
      return count_ident_refs_expr(s->as.destruct_stmt.call, name);
    case AST_STMT_EXPR:
      return count_ident_refs_expr(s->as.expr_stmt, name);
    case AST_STMT_RET: {
      int total = 0;
      for (const AstReturnArg* r = s->as.ret_stmt.values; r; r = r->next) {
        total += count_ident_refs_expr(r->value, name);
      }
      return total;
    }
    case AST_STMT_IF:
      return count_ident_refs_expr(s->as.if_stmt.condition, name) +
             count_ident_refs_block(s->as.if_stmt.then_block, name) +
             count_ident_refs_block(s->as.if_stmt.else_block, name);
    case AST_STMT_LOOP:
      return count_ident_refs_stmt(s->as.loop_stmt.init, name) +
             count_ident_refs_expr(s->as.loop_stmt.condition, name) +
             count_ident_refs_expr(s->as.loop_stmt.increment, name) +
             count_ident_refs_block(s->as.loop_stmt.body, name);
    case AST_STMT_MATCH: {
      int total = count_ident_refs_expr(s->as.match_stmt.subject, name);
      for (const AstMatchCase* c = s->as.match_stmt.cases; c; c = c->next) {
        total += count_ident_refs_expr(c->pattern, name);
        for (const AstCasePattern* op = c->or_patterns; op; op = op->next)
          total += count_ident_refs_expr(op->expr, name);
        total += count_ident_refs_block(c->block, name);
      }
      return total;
    }
    case AST_STMT_ASSIGN:
      return count_ident_refs_expr(s->as.assign_stmt.target, name) +
             count_ident_refs_expr(s->as.assign_stmt.value, name);
    case AST_STMT_DEFER:
      return count_ident_refs_block(s->as.defer_stmt.block, name);
    case AST_STMT_BREAK:
    case AST_STMT_CONTINUE:
      return 0;  // no operands
  }
  return 0;
}
int rae_func_count_param_refs(const AstFuncDecl* fd, Str name) {
  if (!fd || !fd->body) return 0;
  return count_ident_refs_block(fd->body, name);
}

// Emit `drop(local);` calls for every heap-owning binding declared
// from `first_let_index` (inclusive) onward. Anything before
// `first_let_index` is a function parameter — those are owned by the
// caller and must NOT be dropped here. Walk in reverse declaration
// order so a drop never reads a local that's already been dropped
// (LIFO matches how `defer` emits user-written cleanup).
//
// Stage 2 limitation: only called at end-of-body fallthrough, not at
// every `ret`. Functions that early-return therefore leak whatever
// heap-owning lets were live at the ret. Move-detection at ret paths
// lands in Stage 3 — see docs/scope-exit-dealloc.md.
// Stage 3 move tracking (docs/ownership-model.md). Find the local
// named `name` and flip its moved bit so emit_implicit_drops_for_body
// skips it. LIFO scan matches Rae's shadowing rule (latest binding
// wins on name collisions).
void mark_local_moved_by_name(CFuncContext* ctx, Str name) {
  if (!ctx) return;
  for (int i = (int)ctx->local_count - 1; i >= 0; i--) {
    if (str_eq(ctx->locals[i], name)) {
      ctx->local_moved[i] = true;
      return;
    }
  }
}

// Convenience wrapper: if `expr` is a bare identifier referring to a
// local, mark it moved. Anything else (call, member access, literal,
// compound expression) is a no-op — only direct local references
// are owned by a binding the caller is tracking.
void mark_expr_moved_if_local(CFuncContext* ctx, const AstExpr* expr) {
  if (!ctx || !expr) return;
  if (expr->kind == AST_EXPR_IDENT) {
    mark_local_moved_by_name(ctx, expr->as.ident);
  } else if (expr->kind == AST_EXPR_OWN) {
    // Explicit `own x` always tries to move whatever's inside.
    mark_expr_moved_if_local(ctx, expr->as.unary.operand);
  }
}

// Stage C (docs/ownership-model.md): emit cascade drops for `own T`
// parameters at end of scope. Called alongside emit_implicit_drops_-
// for_body — the former handles let-locals, this handles params.
// Move-tracking (local_moved[]) skips drops for params that were
// returned or transferred onward.
bool emit_implicit_drops_for_own_params(CFuncContext* ctx, FILE* out,
                                        size_t first_let_index) {
  if (!ctx || !out) return false;
  if (first_let_index == (size_t)-1) return true;
  for (size_t i = first_let_index; i > 0; i--) {
    size_t idx = i - 1;
    const AstTypeRef* type = ctx->local_type_refs[idx];
    if (!type) continue;
    // Stage C drops `own T` params; Stage 3 also drops `copy T`
    // params — the callee owns the deep copy the caller paid for.
    if (!(type->is_own || type->is_copy)) continue;
    if (ctx->local_moved[idx]) continue;
    if (!type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0)) {
      continue;
    }
    Str name = ctx->locals[idx];
    if (type->is_opt) {
      if (!(type->is_view || type->is_mod) && rae_opt_is_struct_rep(ctx, type))
        fprintf(out, "  rae_drop_%s(&%.*s);\n",
                rae_opt_type_name(ctx, type), (int)name.len, name.data);
      else
        fprintf(out, "  rae_any_drop(&%.*s);\n",
                (int)name.len, name.data);
      continue;
    }
    Str tbase = get_base_type_name(type);
    if (str_eq_cstr(tbase, "String")) {
      fprintf(out, "  rae_string_drop(&%.*s);\n",
              (int)name.len, name.data);
      continue;
    }
    if (is_drop_target_type(type)) {
      const AstTypeRef* elem_type = type->generic_args;
      if (!elem_type) continue;
      Str loc_base = get_base_type_name(type);
      const AstFuncDecl* drop_fd = find_drop_overload_for(ctx, loc_base);
      if (!drop_fd) continue;
      register_function_specialization(ctx->compiler_ctx, drop_fd, elem_type);
      const char* drop_name =
          rae_mangle_specialized_function(ctx->compiler_ctx, drop_fd, elem_type);
      fprintf(out, "  %s(&%.*s);\n", drop_name,
              (int)name.len, name.data);
      continue;
    }
    if (type->generic_args) continue;
    const char* struct_mangled = rae_mangle_type_specialized(
        ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
    fprintf(out, "  rae_drop_struct_%s(&%.*s);\n", struct_mangled,
            (int)name.len, name.data);
  }
  return true;
}

bool emit_implicit_drops_for_body(CFuncContext* ctx, FILE* out,
                                  size_t first_let_index) {
  if (!ctx || !out) return false;
  for (size_t i = ctx->local_count; i > first_let_index; i--) {
    size_t idx = i - 1;
    const AstTypeRef* type = ctx->local_type_refs[idx];
    if (!type) continue;
    if (type->is_view || type->is_mod) continue;
    if (ctx->local_moved[idx]) continue;
    // Task(T): join-on-drop. A Task is a RaeTask* (not a cascade-drop
    // struct), so it'd be skipped below — handle it here. rae_task_drop
    // joins (no-op if already get()'d) then frees, so a worker thread
    // can't outlive its scope / be killed at process teardown.
    if (str_eq_cstr(get_base_type_name(type), "Task")) {
      fprintf(out, "  rae_task_drop(%.*s);\n",
              (int)ctx->locals[idx].len, ctx->locals[idx].data);
      continue;
    }
    // Skip cheap value types — they own no heap and don't need a
    // drop call. Permissive predicate so String-only owning structs
    // are eligible too — alias safety is gated by local_struct_owns_heap
    // below.
    if (!type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0)) {
      continue;
    }
    Str name = ctx->locals[idx];
    if (type->is_opt) {
      if (!(type->is_view || type->is_mod) && rae_opt_is_struct_rep(ctx, type))
        fprintf(out, "  rae_drop_%s(&%.*s);\n",
                rae_opt_type_name(ctx, type), (int)name.len, name.data);
      else
        fprintf(out, "  rae_any_drop(&%.*s);\n",
                (int)name.len, name.data);
      continue;
    }
    Str tbase = get_base_type_name(type);
    if (str_eq_cstr(tbase, "String")) {
      // String locals don't have a synthesised rae_drop_struct_ —
      // call the runtime helper directly. Only drop when the local
      // uniquely owns its heap (auto-init or struct-literal copy);
      // String-typed call results may alias the callee's storage.
      if (ctx->local_struct_owns_heap[idx]) {
        fprintf(out, "  rae_string_drop(&%.*s);\n",
                (int)name.len, name.data);
      }
      continue;
    }
    if (is_drop_target_type(type)) {
      // Stdlib container (List / StringMap / IntMap) — call the
      // user-defined generic `drop(T)` from lib/core.rae.
      const AstTypeRef* elem_type = type->generic_args;
      if (!elem_type) continue;
      Str loc_base = get_base_type_name(type);
      const AstFuncDecl* drop_fd = find_drop_overload_for(ctx, loc_base);
      if (!drop_fd) continue;
      register_function_specialization(ctx->compiler_ctx, drop_fd, elem_type);
      const char* drop_name =
          rae_mangle_specialized_function(ctx->compiler_ctx, drop_fd, elem_type);
      fprintf(out, "  %s(&%.*s);\n", drop_name,
              (int)name.len, name.data);
    } else {
      // Layer 5 + Phase 3 — user struct that transitively needs
      // cascade drop. Two variants are synthesised in c_backend.c:
      //   rae_drop_struct_<T>       — full cascade (drops String fields).
      //   rae_drop_struct_<T>_alias — strict cascade (skips Strings).
      // Pick by local ownership: struct-literal/auto-init locals
      // uniquely own (full); call-result and bare-ident-copy locals
      // may alias the source (alias variant).
      //
      // Stage 1 closure: spec-typed locals (Wrapper(String) etc.)
      // also reach this branch. Pass A' in c_backend.c collects
      // their `ctx->generic_types[]` entries into `drop_entries[]`
      // and emits the synthesised helpers under the spec-mangled
      // name. Generic user-defined containers (List/StringMap/
      // IntMap) still go through the `is_drop_target_type` branch
      // above because they have their own `drop(T)` overload.
      const char* struct_mangled = rae_mangle_type_specialized(
          ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
      const char* suffix = ctx->local_struct_owns_heap[idx] ? "" : "_alias";
      fprintf(out, "  rae_drop_struct_%s%s(&%.*s);\n", struct_mangled, suffix,
              (int)name.len, name.data);
    }
  }
  return true;
}

static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    if (emit_list_if_let(ctx, stmt, out)) return true;
    // `if let` (spec 4.2): the binding is emitted before the condition, inside
    // a C block so the name cannot outlive the construct. The user-visible
    // scope rule -- in the if-branch only -- is enforced by restoring
    // local_count below, exactly as the branches already do.
    size_t saved_locals_bind = ctx->local_count;
    bool has_binding = stmt->as.if_stmt.binding != NULL;
    const AstStmt* bind = stmt->as.if_stmt.binding;
    // OWNED narrowing (spec 4.2): `if let track: Track = getTrack()`. The
    // optional VALUE is materialised for the statement, tested, and on the
    // present path its payload MOVES into the binding — memcpy out of the
    // box, free the box's shell without running its drop (the heap now
    // belongs to the binding, which the branch's normal drop pass releases).
    // The parser leaves the condition NULL for this form; the test on the
    // box is emitted here, where the box has a name.
    bool owned_bind = has_binding && bind->as.let_stmt.type
                      && !bind->as.let_stmt.type->is_view
                      && !bind->as.let_stmt.type->is_mod;
    int ifopt_id = -1;
    if (owned_bind) {
        const AstExpr* src = bind->as.let_stmt.value;
        if (src && src->kind == AST_EXPR_UNBOX) src = src->as.unary.operand;
        ifopt_id = ctx->temp_counter++;
        const AstTypeRef* src_tr = infer_expr_type_ref(ctx, src);
        // #651: name the `rae_opt_<T>` temp from the BINDING's declared payload
        // type — that is the concrete `T` the user narrows to. The source
        // expression's inferred type can be an unresolved generic template
        // return (`opt T` for `copyAt` inside `join`) that would mangle to
        // `rae_opt_rae_T`. Fall back to the inferred type only if the binding
        // type is somehow not struct-rep.
        bool src_opt_struct = false;
        AstTypeRef synth_opt_tr = {0};
        const AstTypeRef* opt_name_tr = NULL;
        if (bind->as.let_stmt.type) {
            synth_opt_tr = *bind->as.let_stmt.type;
            synth_opt_tr.is_opt = true; synth_opt_tr.is_view = false;
            synth_opt_tr.is_mod = false; synth_opt_tr.next = NULL;
            if (rae_opt_is_struct_rep(ctx, &synth_opt_tr)) {
                src_opt_struct = true; opt_name_tr = &synth_opt_tr;
            }
        }
        if (!src_opt_struct && src_tr && src_tr->is_opt
            && !(src_tr->is_view || src_tr->is_mod)
            && rae_opt_is_struct_rep(ctx, src_tr)) {
            src_opt_struct = true; opt_name_tr = src_tr;
        }
        if (src_opt_struct) {
            // Struct-rep optional: `rae_opt_<T> t = src; if (t.has) { T x = t.value; }`.
            // No malloc, no free — the payload moves out of the temp by value.
            const char* optm = rae_opt_type_name(ctx, opt_name_tr);
            fprintf(out, "  { %s __rae_ifopt%d = ", optm, ifopt_id);
            emit_expr(ctx, src, out, PREC_LOWEST, false, false);
            fprintf(out, ";\n  if (__rae_ifopt%d.has) {\n", ifopt_id);
            fprintf(out, "    ");
            emit_type_ref_as_c_type(ctx, bind->as.let_stmt.type, out, false);
            fprintf(out, " %.*s = __rae_ifopt%d.value;\n",
                    (int)bind->as.let_stmt.name.len, bind->as.let_stmt.name.data, ifopt_id);
        } else {
        fprintf(out, "  { RaeAny __rae_ifopt%d = ", ifopt_id);
        emit_expr(ctx, src, out, PREC_LOWEST, false, false);
        fprintf(out, ";\n  if (!rae_any_is_none(__rae_ifopt%d)) {\n", ifopt_id);
        Str obase = get_base_type_name(bind->as.let_stmt.type);
        fprintf(out, "    ");
        emit_type_ref_as_c_type(ctx, bind->as.let_stmt.type, out, false);
        fprintf(out, " %.*s = ", (int)bind->as.let_stmt.name.len, bind->as.let_stmt.name.data);
        if (str_eq_cstr(obase, "Int") || str_eq_cstr(obase, "Int64")
            || str_eq_cstr(obase, "Char") || str_eq_cstr(obase, "Char32")) {
            fprintf(out, "__rae_ifopt%d.as.i;\n", ifopt_id);
        } else if (str_eq_cstr(obase, "Float") || str_eq_cstr(obase, "Float32")
                   || str_eq_cstr(obase, "Float64")) {
            fprintf(out, "__rae_ifopt%d.as.f;\n", ifopt_id);
        } else if (str_eq_cstr(obase, "Bool")) {
            fprintf(out, "__rae_ifopt%d.as.b;\n", ifopt_id);
        } else if (str_eq_cstr(obase, "String")) {
            // The box carried the string's heap; the binding owns it now.
            fprintf(out, "__rae_ifopt%d.as.s;\n", ifopt_id);
        } else {
            fprintf(out, "*(");
            emit_type_ref_as_c_type(ctx, bind->as.let_stmt.type, out, false);
            fprintf(out, "*)__rae_ifopt%d.as.ptr;\n", ifopt_id);
            fprintf(out, "    free(__rae_ifopt%d.as.ptr);\n", ifopt_id);
        }
        }
        // Register the payload as an owning local so the branch's drop pass
        // (and any early `ret` inside it) releases its heap. It uniquely
        // owns: the heap moved out of the box, whose shell was freed above
        // without running its drop.
        if (ctx->local_count < 256) {
            size_t local_index = ctx->local_count;
            ctx->locals[local_index] = bind->as.let_stmt.name;
            ctx->local_type_refs[local_index] = bind->as.let_stmt.type;
            ctx->local_is_ptr[local_index] = false;
            ctx->local_is_mod[local_index] = false;
            ctx->local_moved[local_index] = false;
            ctx->local_struct_owns_heap[local_index] = true;
            ctx->local_count++;
        }
    } else {
        if (has_binding) {
            fprintf(out, "  {\n");
            emit_stmt(ctx, stmt->as.if_stmt.binding, out);
        }
        fprintf(out, "  if (");
        emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST, false, false);
        fprintf(out, ") {\n");
    }
    // Stage 2 scope tracking: save/restore local_count around each
    // branch so lets declared inside the block don't pollute the
    // outer scope's `ctx->locals` view. Without this, the end-of-body
    // drop pass would try to drop names that the C compiler can't
    // see (out-of-scope C identifiers).
    // For owned narrowing the payload local belongs to the THEN branch: it
    // is included in the branch's drop range (and unregistered with it), so
    // its heap is released exactly where its scope ends.
    size_t saved_locals_then = ctx->local_count - (owned_bind ? 1 : 0);
    if (stmt->as.if_stmt.then_block) {
        for (const AstStmt* s = stmt->as.if_stmt.then_block->first; s; s = s->next) emit_stmt(ctx, s, out);
    }
    emit_implicit_drops_for_body(ctx, out, saved_locals_then);
    ctx->local_count = saved_locals_then;
    fprintf(out, "  }");
    if (stmt->as.if_stmt.else_block) {
        fprintf(out, " else {\n");
        size_t saved_locals_else = ctx->local_count;
        for (const AstStmt* s = stmt->as.if_stmt.else_block->first; s; s = s->next) emit_stmt(ctx, s, out);
        emit_implicit_drops_for_body(ctx, out, saved_locals_else);
        ctx->local_count = saved_locals_else;
        fprintf(out, "  }\n");
    } else {
        fprintf(out, "\n");
    }
    if (has_binding) {
        fprintf(out, "  }\n");
        ctx->local_count = saved_locals_bind;
    }
    return true;
}

static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    if (stmt->as.loop_stmt.is_range) {
        const AstStmt* binding = stmt->as.loop_stmt.init;
        const AstTypeRef* binding_type = binding ? binding->as.let_stmt.type : NULL;
        const AstTypeRef* collection_type = infer_expr_type_ref(
            ctx, stmt->as.loop_stmt.condition);
        if (!binding || binding->kind != AST_STMT_LET || !binding_type
            || !collection_type) {
            fprintf(out, "  /* invalid collection loop; rejected by sema */\n");
            return true;
        }

        int loop_id = ctx->temp_counter++;
        AstTypeRef collection_value_type = *collection_type;
        collection_value_type.is_view = false;
        collection_value_type.is_mod = false;
        collection_value_type.is_opt = false;

        /* Snapshot the List header once. This aliases its backing storage but
         * does not own or drop it. The body iterates directly over `data`, so
         * there is no optional construction and no per-element bounds check. */
        fprintf(out, "  {\n    ");
        emit_type_ref_as_c_type(ctx, &collection_value_type, out, false);
        fprintf(out, " __rae_collection%d = ", loop_id);
        bool collection_is_ref = collection_type->is_view || collection_type->is_mod;
        if (collection_is_ref) fprintf(out, "*(");
        emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST, false, false);
        if (collection_is_ref) fprintf(out, ")");
        fprintf(out, ";\n    int64_t __rae_collection_length%d = __rae_collection%d.length;\n",
                loop_id, loop_id);
        fprintf(out, "    for (int64_t __rae_collection_index%d = 0; "
                     "__rae_collection_index%d < __rae_collection_length%d; "
                     "__rae_collection_index%d++) {\n",
                loop_id, loop_id, loop_id, loop_id);

        size_t saved_locals = ctx->local_count;
        fprintf(out, "      ");
        emit_type_ref_as_c_type(ctx, binding_type, out, false);
        fprintf(out, " %.*s", (int)binding->as.let_stmt.name.len,
                binding->as.let_stmt.name.data);
        bool binding_is_ref = binding_type->is_view || binding_type->is_mod;
        bool copy_string = !binding_is_ref
            && str_eq_cstr(get_base_type_name(binding_type), "String");
        AstTypeRef binding_value_type = *binding_type;
        binding_value_type.is_view = false;
        binding_value_type.is_mod = false;
        binding_value_type.is_opt = false;
        bool deep_value = !binding_is_ref && !copy_string
            && type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                    &binding_value_type, 0);
        if (deep_value) {
            const char* type_name = rae_mangle_type_specialized(
                ctx->compiler_ctx, ctx->generic_params, ctx->generic_args,
                &binding_value_type);
            fprintf(out, ";\n      rae_deep_copy_%s(&%.*s, "
                         "&__rae_collection%d.data[__rae_collection_index%d])",
                    type_name, (int)binding->as.let_stmt.name.len,
                    binding->as.let_stmt.name.data, loop_id, loop_id);
        } else {
            fprintf(out, " = ");
            if (copy_string) fprintf(out, "rae_string_copy(");
            bool primitive_ref = binding_is_ref
                && is_primitive_type(get_base_type_name(binding_type));
            if (primitive_ref) fprintf(out, "{ .ptr = &");
            else if (binding_is_ref) fprintf(out, "&");
            fprintf(out, "__rae_collection%d.data[__rae_collection_index%d]",
                    loop_id, loop_id);
            if (primitive_ref) fprintf(out, " }");
            if (copy_string) fprintf(out, ")");
        }
        fprintf(out, ";\n");

        if (ctx->local_count < 256) {
            size_t local_index = ctx->local_count++;
            ctx->locals[local_index] = binding->as.let_stmt.name;
            ctx->local_type_refs[local_index] = binding_type;
            ctx->local_is_ptr[local_index] = binding_is_ref;
            ctx->local_is_mod[local_index] = binding_type->is_mod;
            ctx->local_moved[local_index] = false;
            ctx->local_struct_owns_heap[local_index] = !binding_is_ref;
        }
        // break/continue drop back to here (before the element binding), so a
        // non-local exit drops the current element too, matching the per-
        // iteration drop below.
        if (ctx->loop_depth < 32) ctx->loop_body_local_start[ctx->loop_depth] = saved_locals;
        ctx->loop_depth++;
        if (stmt->as.loop_stmt.body) {
            for (const AstStmt* body_stmt = stmt->as.loop_stmt.body->first;
                 body_stmt; body_stmt = body_stmt->next) {
                emit_stmt(ctx, body_stmt, out);
            }
        }
        ctx->loop_depth--;
        emit_implicit_drops_for_body(ctx, out, saved_locals);
        ctx->local_count = saved_locals;
        fprintf(out, "    }\n  }\n");
        return true;
    }

    // A declaration initializer needs the normal let/var lowering so the
    // backend can resolve method calls on the counter inside all three loop
    // clauses and the body. Keep it in an enclosing C scope so owned values
    // can also be dropped after the loop rather than leaking.
    size_t saved_locals = ctx->local_count;
    bool has_decl_init = stmt->as.loop_stmt.init
        && stmt->as.loop_stmt.init->kind == AST_STMT_LET;
    if (has_decl_init) {
        fprintf(out, "  {\n");
        emit_stmt(ctx, stmt->as.loop_stmt.init, out);
        fprintf(out, "  for (; ");
    } else {
        fprintf(out, "  for (");
        if (stmt->as.loop_stmt.init) {
            emit_expr(ctx, stmt->as.loop_stmt.init->as.expr_stmt, out,
                      PREC_LOWEST, false, false);
        }
        fprintf(out, "; ");
    }
    if (stmt->as.loop_stmt.condition) emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST, false, false);
    fprintf(out, "; ");
    if (stmt->as.loop_stmt.increment) emit_expr(ctx, stmt->as.loop_stmt.increment, out, PREC_LOWEST, false, false);
    fprintf(out, ") {\n");
    // The declaration initializer belongs to the enclosing loop scope.
    // break/continue only drop locals introduced by the current body.
    size_t body_locals = ctx->local_count;
    if (ctx->loop_depth < 32) ctx->loop_body_local_start[ctx->loop_depth] = body_locals;
    ctx->loop_depth++;
    if (stmt->as.loop_stmt.body) {
        for (const AstStmt* s = stmt->as.loop_stmt.body->first; s; s = s->next) emit_stmt(ctx, s, out);
    }
    ctx->loop_depth--;
    emit_implicit_drops_for_body(ctx, out, body_locals);
    ctx->local_count = body_locals;
    fprintf(out, "  }\n");
    if (has_decl_init) {
        emit_implicit_drops_for_body(ctx, out, saved_locals);
        fprintf(out, "  }\n");
    }
    ctx->local_count = saved_locals;
    return true;
}

// Does this expression already yield a reference (a call whose declared return
// type is `view T` / `mod T`)? Such a value needs no materialising -- it is
// already a pointer to storage someone else owns.
static bool ref_bind_value_returns_ref(CFuncContext* ctx, const AstExpr* val) {
    if (!val) return false;
    if (val->kind != AST_EXPR_CALL && val->kind != AST_EXPR_METHOD_CALL) return false;
    const AstFuncDecl* vfd = val->decl_link ? &val->decl_link->as.func_decl : NULL;
    if (vfd && vfd->returns && vfd->returns->type
        && (vfd->returns->type->is_view || vfd->returns->type->is_mod)) return true;
    // Sema does not always populate decl_link on call sites; c_call.c
    // re-resolves by name, so mirror that lookup here. Method calls need it
    // too: `tracks.viewAt(index: 0)` arrives as a METHOD_CALL with no
    // decl_link, and without this the binding would take the address of a
    // pointer the callee already returned.
    Str callee = (Str){0};
    if (val->kind == AST_EXPR_CALL && val->as.call.callee
        && val->as.call.callee->kind == AST_EXPR_IDENT) {
        callee = val->as.call.callee->as.ident;
    } else if (val->kind == AST_EXPR_METHOD_CALL) {
        callee = val->as.method_call.method_name;
    }
    if (callee.len > 0 && ctx && ctx->compiler_ctx) {
        for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
            const AstDecl* d = ctx->compiler_ctx->all_decls[i];
            if (d->kind != AST_DECL_FUNC) continue;
            if (!str_eq(d->as.func_decl.name, callee)) continue;
            const AstFuncDecl* cfd = &d->as.func_decl;
            if (cfd->returns && cfd->returns->type
                && (cfd->returns->type->is_view || cfd->returns->type->is_mod)) return true;
        }
    }
    return false;
}

/* The backend twin of sema's spec-4.2 check, for the UFCS calls sema cannot
 * resolve: does this call (by name) return an OWNED optional? A view/mod
 * narrowing of one is rejected — before this, `if let t: view Track =>
 * tracks.get(index: 0)` materialised the box, read garbage, and aborted. */
static bool bind_value_returns_owned_opt(CFuncContext* ctx, const AstExpr* val) {
    if (!val || !ctx || !ctx->compiler_ctx) return false;
    Str mname = (Str){0};
    if (val->kind == AST_EXPR_METHOD_CALL) {
        mname = val->as.method_call.method_name;
    } else if (val->kind == AST_EXPR_CALL && val->as.call.callee
               && val->as.call.callee->kind == AST_EXPR_IDENT) {
        mname = val->as.call.callee->as.ident;
    }
    if (!mname.len) return false;
    for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
        const AstDecl* d = ctx->compiler_ctx->all_decls[i];
        if (d->kind != AST_DECL_FUNC) continue;
        if (!str_eq(d->as.func_decl.name, mname)) continue;
        const AstTypeRef* rt = d->as.func_decl.returns ? d->as.func_decl.returns->type : NULL;
        return rt && rt->is_opt && !rt->is_view && !rt->is_mod;
    }
    return false;
}

// Emit one pattern test for a match case: `rae_any_is_none(subj)` for the
// `none` pattern, `rae_ext_rae_str_eq(subj, pattern)` for a String subject
// (rae_String is a struct — `==` is invalid), else `subj == pattern`.
// Or-patterns chain several of these with `||` (C's `==` and calls bind
// tighter than `||`, so no extra parens needed).
static void emit_match_case_test(CFuncContext* ctx, const AstExpr* subject,
                                 const AstExpr* pattern, bool subject_is_string,
                                 FILE* out) {
    if (pattern->kind == AST_EXPR_NONE) {
        bool saved = ctx->suppress_opt_unbox;
        ctx->suppress_opt_unbox = true;
        const AstTypeRef* subj_tr = infer_expr_type_ref(ctx, subject);
        if (subj_tr && subj_tr->is_opt && !(subj_tr->is_view || subj_tr->is_mod)
            && rae_opt_is_struct_rep(ctx, subj_tr)) {
            fprintf(out, "(!(");
            emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
            fprintf(out, ").has)");
        } else {
            fprintf(out, "rae_any_is_none(");
            emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
            fprintf(out, ")");
        }
        ctx->suppress_opt_unbox = saved;
    } else if (subject_is_string) {
        fprintf(out, "rae_ext_rae_str_eq(");
        emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
        fprintf(out, ", ");
        emit_expr(ctx, pattern, out, PREC_LOWEST, false, false);
        fprintf(out, ")");
    } else {
        emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
        fprintf(out, " == ");
        emit_expr(ctx, pattern, out, PREC_LOWEST, false, false);
    }
}

bool emit_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    if (!stmt) return true;
    switch (stmt->kind) {
        case AST_STMT_EXPR: {
            // Stage 4: wrap with string-pool mark/flush. `rae_ext_rae_str_interp`
            // registers each interp result; flush at the end of this expression
            // statement cleans up any temps the expression created (the common
            // case: `log("iter {i}")` where the interp result is consumed by log
            // and never bound). Bindings (let/assign/ret) detach captured
            // results via `rae_string_pool_take` so this flush doesn't free them.
            fprintf(out, "  { int __rae_spm = rae_string_pool_mark(); ");
            emit_expr(ctx, stmt->as.expr_stmt, out, PREC_LOWEST, false, false);
            fprintf(out, "; rae_string_pool_flush(__rae_spm); }\n");
            break;
        }
        case AST_STMT_LET: {
            // Tracks whether the RHS was wrapped in rae_string_copy /
            // rae_deep_copy_<T>. The post-init ownership classifier
            // below uses this to mark the local as owning (since the
            // copy gave it private heap), overriding the default
            // bare-ident-IDENT-aliases-source classification.
            bool let_did_deep_copy = false;
            bool is_ref_bind = stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type &&
                               (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod);

            // A `=>` onto a produced value. Two very different cases meet
            // here (spec 2.3.1):
            //
            //   - A NON-OPT reference binding to a produced owned value is an
            //     ERROR: a fresh value needs an owner and a view/mod binding
            //     refuses ownership. This used to be materialised into a
            //     hidden temporary (#443); that rule was withdrawn (#454/#455).
            //     sema rejects the shapes it can resolve; this is the
            //     catch-all for method calls it cannot.
            //
            //   - An OPT binding (an `if let` narrowing, or an optional-
            //     reference local) still materialises: the optional VALUE the
            //     construct consumes needs statement storage regardless of
            //     what the binding form is. Its ownership story is #457.
            //
            // The temporary is a plain local emitted just before, so its
            // lifetime is the enclosing scope. `__auto_type` avoids having to
            // reconstruct the expression's C type here.
            int materialised_id = -1;
            if (is_ref_bind && stmt->as.let_stmt.value
                && !is_primitive_type(get_base_type_name(stmt->as.let_stmt.type))
                && !ref_bind_value_returns_ref(ctx, stmt->as.let_stmt.value)) {
                AstExprKind vk = stmt->as.let_stmt.value->kind;
                bool is_opt_bind = stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_opt;
                if (!is_opt_bind
                    && (vk == AST_EXPR_CALL || vk == AST_EXPR_METHOD_CALL
                        || vk == AST_EXPR_OBJECT)) {
                    diag_error(ctx->module ? ctx->module->file_path : NULL,
                               (int)stmt->line, (int)stmt->column,
                               "cannot bind a reference to a call that returns ownership; "
                               "write 'let name: T = ...' to take the value, or use a reference-returning "
                               "accessor (viewAt, modAt, viewGet, modGet)");
                } else
                if (is_opt_bind
                    && (vk == AST_EXPR_CALL || vk == AST_EXPR_METHOD_CALL)
                    && bind_value_returns_owned_opt(ctx, stmt->as.let_stmt.value)) {
                    diag_error(ctx->module ? ctx->module->file_path : NULL,
                               (int)stmt->line, (int)stmt->column,
                               "this call returns an owned optional; a view/mod binding refuses ownership "
                               "and nothing else would own the value — take it with "
                               "'if let <name>: T = ...' (spec 4.2)");
                } else
                if (vk == AST_EXPR_CALL || vk == AST_EXPR_METHOD_CALL
                    || vk == AST_EXPR_OBJECT || vk == AST_EXPR_INDEX) {
                    materialised_id = ctx->temp_counter++;
                    fprintf(out, "  __auto_type __rae_bind%d = ", materialised_id);
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    fprintf(out, ";\n");
                }
            }

            fprintf(out, "  ");
            emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
            fprintf(out, " %.*s = ", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
            if (materialised_id >= 0) {
                fprintf(out, "&__rae_bind%d;\n", materialised_id);
                if (ctx->local_count < 256) {
                    size_t local_index = ctx->local_count;
                    ctx->locals[local_index] = stmt->as.let_stmt.name;
                    ctx->local_type_refs[local_index] = stmt->as.let_stmt.type;
                    ctx->local_is_ptr[local_index] = false;
                    ctx->local_is_mod[local_index] = false;
                    ctx->local_count++;
                }
                break;
            }
            if (is_ref_bind) {
                Str base = get_base_type_name(stmt->as.let_stmt.type);
                // Does the source ident ALREADY lower to a reference — a
                // view/mod param or an earlier alias binding? Then it is a
                // pointer (struct) or a .ptr handle (primitive), and `&` on
                // it aliases the stack slot holding the pointer instead of
                // the referent: `rae_Track* y = &x` with x already
                // rae_Track* wrote 42 into the pointer cell and the caller
                // never saw it (#461).
                bool src_is_ref_local = false;
                if (stmt->as.let_stmt.value
                    && stmt->as.let_stmt.value->kind == AST_EXPR_IDENT) {
                    Str vn = stmt->as.let_stmt.value->as.ident;
                    for (int i = (int)ctx->local_count - 1; i >= 0; i--) {
                        if (str_eq(ctx->locals[i], vn)) {
                            const AstTypeRef* lt = ctx->local_type_refs[i];
                            if (lt && (lt->is_view || lt->is_mod)) src_is_ref_local = true;
                            break;
                        }
                    }
                }
                if (is_primitive_type(base)) {
                    // Check if the value is a function call returning a ref type
                    // (can't take address of rvalue — assign directly)
                    bool value_returns_ref = ref_bind_value_returns_ref(
                        ctx, stmt->as.let_stmt.value);
                    if (value_returns_ref) {
                        // Optional primitive references cross a function
                        // boundary as nullable raw pointers; locals keep the
                        // normal primitive-ref wrapper used by expression
                        // lowering, so install the returned pointer in it.
                        if (stmt->as.let_stmt.type->is_opt) fprintf(out, "{ .ptr = ");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                        if (stmt->as.let_stmt.type->is_opt) fprintf(out, " }");
                    } else if (src_is_ref_local) {
                        // The source is itself a .ptr handle: alias the same
                        // referent, not the handle's own stack slot.
                        fprintf(out, "{ .ptr = %.*s.ptr }",
                                (int)stmt->as.let_stmt.value->as.ident.len,
                                stmt->as.let_stmt.value->as.ident.data);
                    } else {
                        // Primitive ref: rae_Mod_Int64 r = { .ptr = &x };
                        fprintf(out, "{ .ptr = &");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    }
                } else {
                    /* One implementation of "does this call already hand
                     * back a pointer?", shared with the materialisation
                     * decision above — they must agree, or the binding
                     * either double-addresses or dangles. */
                    bool value_returns_ref =
                        ref_bind_value_returns_ref(ctx, stmt->as.let_stmt.value);
                    if (value_returns_ref) {
                        // The call already returns a pointer; cast away
                        // const so a `view T` binding (non-const C ptr)
                        // accepts a `const T*` from a `ret view` callee.
                        // Read-only invariant is upheld at the Rae level,
                        // not at the C level — the binding type tells
                        // emit_expr to refuse mutations.
                        fprintf(out, "(");
                        emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
                        fprintf(out, ")");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_UNARY, false, false);
                    } else if (stmt->as.let_stmt.value->kind == AST_EXPR_UNBOX) {
                        // NARROWING A BOXED OPTIONAL (`opt T` for a non-reference
                        // T, which is a RaeAny). Sema wraps the value in an
                        // UNBOX node, and the expression emitter lowers that to
                        // `.as.ptr` -- which IS the payload pointer. Taking its
                        // address would hand back the address of a pointer and
                        // type-pun the box as a T; before this, that compiled
                        // and printed nonsense.
                        //
                        // `rae_any_none()` leaves the payload null, so the
                        // emptiness test `if let` emits still holds.
                        const AstExpr* unbox_inner = stmt->as.let_stmt.value->as.unary.operand;
                        const AstTypeRef* inner_tr = infer_expr_type_ref(ctx, unbox_inner);
                        if (inner_tr && inner_tr->is_opt && !(inner_tr->is_view || inner_tr->is_mod)
                            && rae_opt_is_struct_rep(ctx, inner_tr)) {
                            // A view/mod binding into a struct-rep opt: point at
                            // the inline `.value` when present, else NULL so the
                            // `if let` presence test fails.
                            int oid = ctx->temp_counter++;
                            fprintf(out, "(");
                            emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
                            fprintf(out, ")({ %s* __ov%d = &(", rae_opt_type_name(ctx, inner_tr), oid);
                            emit_expr(ctx, unbox_inner, out, PREC_LOWEST, true, false);
                            fprintf(out, "); __ov%d->has ? &__ov%d->value : NULL; })", oid, oid);
                        } else {
                            fprintf(out, "(");
                            emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
                            fprintf(out, ")");
                            emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_UNARY, false, false);
                        }
                    } else if (src_is_ref_local) {
                        // The source is already a T* (view/mod param or an
                        // earlier alias): alias the referent directly. Emit
                        // the raw name — emit_expr would dereference a
                        // reference ident in value context. Cast for the
                        // const difference between view and mod lowering;
                        // read-only is enforced at the Rae level.
                        fprintf(out, "(");
                        emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
                        fprintf(out, ")%.*s",
                                (int)stmt->as.let_stmt.value->as.ident.len,
                                stmt->as.let_stmt.value->as.ident.data);
                    } else {
                        fprintf(out, "&");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, true);
                    }
                }
            } else if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == AST_EXPR_COLLECTION_LITERAL) {
                // Collection literal: let x: List(Int) = { 10, 20, 30 }
                // Emit as: createList(count) followed by add() calls
                const AstTypeRef* list_type = stmt->as.let_stmt.type;
                const AstTypeRef* elem_type = list_type ? list_type->generic_args : NULL;
                int count = 0;
                for (const AstCollectionElement* e = stmt->as.let_stmt.value->as.collection.elements; e; e = e->next) count++;

                // Find createList and add functions
                const AstFuncDecl* create_fd = NULL;
                const AstFuncDecl* add_fd = NULL;
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind != AST_DECL_FUNC) continue;
                    if (str_eq_cstr(d->as.func_decl.name, "createList") && d->as.func_decl.generic_params) create_fd = &d->as.func_decl;
                    if (str_eq_cstr(d->as.func_decl.name, "add") && d->as.func_decl.generic_params) add_fd = &d->as.func_decl;
                }

                if (create_fd && add_fd && elem_type) {
                    // Register specializations
                    register_function_specialization(ctx->compiler_ctx, create_fd, elem_type);
                    register_function_specialization(ctx->compiler_ctx, add_fd, elem_type);
                    // Emit: Type name = createList_T_(count);
                    const char* create_name = rae_mangle_specialized_function(ctx->compiler_ctx, create_fd, elem_type);
                    fprintf(out, "%s(((int64_t)%dLL));\n", create_name, count);
                    // Emit add calls
                    const char* add_name = rae_mangle_specialized_function(ctx->compiler_ctx, add_fd, elem_type);
                    Str var_name = stmt->as.let_stmt.name;
                    Str et_base = get_base_type_name(elem_type);
                    bool elem_is_any = str_eq_cstr(et_base, "Any") || str_eq_cstr(et_base, "RaeAny");
                    for (const AstCollectionElement* e = stmt->as.let_stmt.value->as.collection.elements; e; e = e->next) {
                        fprintf(out, "  %s(&%.*s, ", add_name, (int)var_name.len, var_name.data);
                        if (elem_is_any) fprintf(out, "rae_any((");
                        emit_expr(ctx, e->value, out, PREC_LOWEST, false, false);
                        if (elem_is_any) fprintf(out, "))");
                        fprintf(out, ");\n");
                    }
                    // Register generic type for struct emission
                    register_generic_type(ctx->compiler_ctx, list_type);
                } else {
                    fprintf(out, "{0};\n");
                }
                // Skip the trailing ";\n" since we already emitted it
                const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
                if (ctx->local_count < 256) {
                    size_t local_index = ctx->local_count;
                    ctx->locals[local_index] = stmt->as.let_stmt.name;
                    ctx->local_types[local_index] = str_from_cstr(tn);
                    ctx->local_type_refs[local_index] = stmt->as.let_stmt.type;
                    ctx->local_is_ptr[local_index] = false;
                    ctx->local_is_mod[local_index] = false;
                    ctx->local_count++;
                }
                break;
            } else if (stmt->as.let_stmt.value) {
                // Set expected type so generic call resolution can infer from let type
                if (stmt->as.let_stmt.type) { ctx->expected_type = *stmt->as.let_stmt.type; ctx->has_expected_type = true; }
                // If declared type is `opt T` and the value's inferred type is the
                // concrete T, wrap with rae_any() so RaeAny holds the boxed value.
                bool needs_box = false;
                if (stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_opt) {
                    const AstTypeRef* val_tr = infer_expr_type_ref(ctx, stmt->as.let_stmt.value);
                    if (val_tr && !val_tr->is_opt) {
                        Str val_base = get_base_type_name(val_tr);
                        // Don't box if value is already RaeAny (e.g. another opt result)
                        if (!str_eq_cstr(val_base, "Any") && val_base.len > 0) needs_box = true;
                    }
                }
                // Stage 4: if the let captures a String, detach any temp-pool
                // entry the RHS produced so the subsequent statement-end
                // flush doesn't free the binding's data. `rae_string_pool_take`
                // is a no-op when the pointer isn't actually in the pool, so
                // it's safe to wrap unconditionally for any non-borrow String
                // let. Deep-copy-on-`=` and auto-drop for String locals are
                // future work — they need every early-return path to flush
                // pools and run implicit drops, which isn't wired up yet.
                bool wrap_str_take = false;
                if (stmt->as.let_stmt.type
                    && !stmt->as.let_stmt.type->is_view
                    && !stmt->as.let_stmt.type->is_mod
                    && !stmt->as.let_stmt.type->is_opt) {
                    Str lbase = get_base_type_name(stmt->as.let_stmt.type);
                    if (str_eq_cstr(lbase, "String")) wrap_str_take = true;
                }

                // === Owning-let deep-copy ===
                //
                // Rule: Rae must never silently shallow-copy an owning
                // value. `let b: T = a` where `a` is a bare identifier
                // (no `own`, no `view`) MUST deep-copy if T owns heap.
                // Without this the binding shallow-aliases `a`'s
                // backing buffer/string and the implicit auto-drop
                // double-frees at scope end.
                //
                // The init is `a` (bare ident, not wrapped in `own`)
                // when init->kind == AST_EXPR_IDENT. `own a` parses as
                // AST_EXPR_OWN { operand: IDENT(a) }, which keeps the
                // move-semantics path (no copy).
                //
                // String IDENT case is handled by the special
                // wrap_str_take/rae_string_copy hand-off below — the
                // pool_take wrapper is replaced with rae_string_copy
                // since the ident's storage is still live in its
                // owner.
                //
                // Container & user-struct cases are emitted as a
                // statement-expression: declare a temp, call
                // rae_deep_copy_<T>, evaluate to the temp.
                bool deep_copy_ident = false;
                bool deep_copy_string_ident = false;
                /* `=` COPIES. Any initialiser that READS AN EXISTING
                 * LOCATION — a bare ident, a field, an element — binds a
                 * fresh owned value, never an alias of the source.
                 *
                 * MEMBER and INDEX used to fall through to the pool_take
                 * path below, so `let m: String = h.name` took ownership of
                 * the struct's field buffer without copying it and then
                 * dropped it at scope exit, freeing storage the struct still
                 * pointed at. Nothing in `=` suggests a transfer; that is
                 * what `own` is for.
                 *
                 * Fresh-value initialisers (a call result, an interpolation)
                 * are deliberately NOT here: they produce a new string that
                 * genuinely needs detaching from the temp pool, which is
                 * what pool_take does. */
                bool init_reads_location =
                    stmt->as.let_stmt.value->kind == AST_EXPR_IDENT
                    || stmt->as.let_stmt.value->kind == AST_EXPR_MEMBER
                    || stmt->as.let_stmt.value->kind == AST_EXPR_INDEX;
                if (stmt->as.let_stmt.type
                    && !stmt->as.let_stmt.type->is_view
                    && !stmt->as.let_stmt.type->is_mod
                    && !stmt->as.let_stmt.type->is_opt
                    && init_reads_location
                    && type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                            stmt->as.let_stmt.type, 0)) {
                    Str lbase = get_base_type_name(stmt->as.let_stmt.type);
                    if (str_eq_cstr(lbase, "String")) {
                        deep_copy_string_ident = true;
                    } else {
                        deep_copy_ident = true;
                    }
                }

                if (needs_box) {
                    emit_optional_boxed_expr(ctx, stmt->as.let_stmt.type,
                                             stmt->as.let_stmt.value, out);
                } else if (deep_copy_string_ident) {
                    // `let b: String = a` — deep copy via rae_string_copy.
                    // Replaces the pool_take wrapper which would only
                    // detach (and the ident's storage isn't in the pool
                    // anyway, so pool_take is a no-op alias). Without
                    // the copy, `b` shallow-aliases `a`'s string heap
                    // and the per-local auto-drop at scope end double-
                    // frees.
                    fprintf(out, "rae_string_copy(");
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                    let_did_deep_copy = true;
                } else if (deep_copy_ident) {
                    // Container or user-struct deep copy via
                    // statement-expression. The synthesised helper
                    // `rae_deep_copy_<MangledT>` is forward-declared at
                    // the top of the compilation unit (see
                    // c_backend.c's copy_entries / container_entries
                    // emission).
                    const char* tn_dc = rae_mangle_type_specialized(
                        ctx->compiler_ctx, ctx->generic_params,
                        ctx->generic_args, stmt->as.let_stmt.type);
                    int tmp_id = ctx->temp_counter++;
                    fprintf(out, "(__extension__ ({ %s __dc%d; rae_deep_copy_%s(&__dc%d, &(",
                            tn_dc, tmp_id, tn_dc, tmp_id);
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    fprintf(out, ")); __dc%d; }))", tmp_id);
                    let_did_deep_copy = true;
                } else {
                    if (wrap_str_take) fprintf(out, "rae_string_pool_take(");
                    emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    if (wrap_str_take) fprintf(out, ")");
                }
                ctx->has_expected_type = false;
            } else {
                // Auto-init: let x: Type (no initializer)
                emit_auto_init(ctx, stmt->as.let_stmt.type, out);
            }
            fprintf(out, ";\n");
            const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
            if (ctx->local_count < 256) {
                ctx->local_is_ptr[ctx->local_count] = false;
                ctx->local_is_mod[ctx->local_count] = false;
                ctx->locals[ctx->local_count] = stmt->as.let_stmt.name;
                ctx->local_types[ctx->local_count] = str_from_cstr(tn);
                ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type;
                // Phase 3 ownership classification — does this binding
                // uniquely own its heap, or does it shallow-alias
                // someone else's storage? Used by emit_implicit_drops
                // to pick between the full and `_alias` cascade-drop
                // variants synthesised in c_backend.c.
                //
                //   own (full drop):
                //     - auto-init (`let x: T`)
                //     - struct literal (`let x: T = {...}`)
                //     - call result — callees that return plain T
                //       went through the Stage 7 ret-epilogue which
                //       transfers ownership (pool_take, move-track on
                //       `ret x`). The bare exceptions are container
                //       extractors that alias into their argument —
                //       see below.
                //
                //   alias (strict drop, skip Strings):
                //     - bare-ident copy (`let x: T = y`) — shallow
                //       copies the C struct, aliasing y's heap.
                //     - call to a known extractor: rae_ext_rae_buf_get
                //       (List/Map slot extraction), valueAt (JsonDoc
                //       list element), componentGet (ECS component
                //       table). These return into a value-typed slot
                //       owned by the first arg's storage.
                const AstExpr* init = stmt->as.let_stmt.value;
                bool owns = (init == NULL)
                    || (init->kind == AST_EXPR_OBJECT)
                    || (init->kind == AST_EXPR_INTERP)
                    || (init->kind == AST_EXPR_BINARY);
                // Owning-let deep-copy path (above): when we wrapped the
                // bare-ident RHS in rae_string_copy / rae_deep_copy_<T>,
                // the binding now owns its own private heap and must
                // run the full drop chain at scope end.
                if (let_did_deep_copy) {
                  owns = true;
                }
                // Body-inspect both AST_EXPR_CALL and AST_EXPR_METHOD_CALL
                // initializers. Method calls like `node.childrenIds.get(
                // index: q)` lower to a free-function call internally and
                // CAN be alias-returning (List.get / StringMap.get / etc.
                // all `ret rae_ext_rae_buf_get(...)`). Previously method
                // calls were classified owning unconditionally, which
                // caused 413_scene_loader's pass-3 children loop to
                // auto-drop the let-local at each iteration end — the
                // local aliased the List slot, so freeing it freed the
                // slot's data and the next iter read garbage.
                Str cn = {0};
                bool init_is_call = false;
                if (init && init->kind == AST_EXPR_CALL && init->as.call.callee
                    && init->as.call.callee->kind == AST_EXPR_IDENT) {
                    cn = init->as.call.callee->as.ident;
                    init_is_call = true;
                } else if (init && init->kind == AST_EXPR_METHOD_CALL) {
                    cn = init->as.method_call.method_name;
                    init_is_call = true;
                }
                if (!owns && init_is_call) {
                    bool is_buf_get =
                        str_eq_cstr(cn, "rae_ext_rae_buf_get") ||
                        str_eq_cstr(cn, "__buf_get") ||
                        str_eq_cstr(cn, "rae_ext___buf_get");
                    if (is_buf_get) {
                      owns = false;
                    } else {
                      // Inspect the callee's body to see whether it
                      // returns an alias (a local initialised from
                      // buf_get / list.get / similar) or an owned
                      // value (struct literal / new call / pool_take).
                      // Default to owning — only flip to alias when
                      // we find a clear `ret <local>` whose local was
                      // initialised from a known aliasing source.
                      // For method calls, we look up by method name —
                      // List.get / StringMap.get / IntMap.get all share
                      // the name "get" but all return alias (buf_get
                      // wrapper), so first-match is safe in practice.
                      owns = true;
                      for (size_t k = 0; k < ctx->compiler_ctx->all_decl_count; k++) {
                        const AstDecl* d = ctx->compiler_ctx->all_decls[k];
                        if (d->kind != AST_DECL_FUNC) continue;
                        if (!str_eq(d->as.func_decl.name, cn)) continue;
                        if (!d->as.func_decl.body) break;
                        owns = !rae_func_returns_alias(ctx->compiler_ctx, &d->as.func_decl);
                        break;
                      }
                    }
                }
                ctx->local_struct_owns_heap[ctx->local_count] = owns;
                // Alias-clearing for String locals from aliasing inits:
                // a buf_get-flavoured RHS hands back a String value
                // whose is_owned bit was inherited from the container
                // (since list elements are owned by the buffer). The
                // local doesn't actually own that heap — clear the
                // bit so a later auto-drop / reassign drop becomes a
                // no-op and the canonical owner (the list) cleans up.
                //
                // Skip the clear if we deep-copied (rae_string_copy
                // returns is_owned=1 and the heap really is private).
                if (!owns && !let_did_deep_copy && stmt->as.let_stmt.type
                    && !stmt->as.let_stmt.type->is_view
                    && !stmt->as.let_stmt.type->is_mod
                    && !stmt->as.let_stmt.type->is_opt
                    && str_eq_cstr(get_base_type_name(stmt->as.let_stmt.type), "String")) {
                  Str ln = stmt->as.let_stmt.name;
                  fprintf(out, "  %.*s.is_owned = 0;\n",
                          (int)ln.len, ln.data);
                }
                ctx->local_count++;
            }
            break;
        }
        case AST_STMT_ASSIGN: {
            // Stage 3 move tracking (continued): a field assignment
            // `target.field = src` where src is a bare local of a
            // heap-owning type moves src's heap into the target. The
            // local must be skipped by end-of-scope auto-drop so we
            // don't double-free the heap (now reachable via both
            // src and target.field). Mirrors the move detection on
            // `own x`, `ret x`, and bare-ident arg passing.
            if (stmt->as.assign_stmt.target &&
                stmt->as.assign_stmt.target->kind == AST_EXPR_MEMBER &&
                stmt->as.assign_stmt.value &&
                stmt->as.assign_stmt.value->kind == AST_EXPR_IDENT) {
                const AstTypeRef* vtr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.value);
                if (vtr && !(vtr->is_view || vtr->is_mod) &&
                    type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, vtr, 0)) {
                    mark_expr_moved_if_local(ctx, stmt->as.assign_stmt.value);
                }
            }
            fprintf(out, "  ");
            // Check if assigning to a mod ref variable (e.g. rx = 10 where rx is rae_Mod_Int64)
            const AstTypeRef* target_tr = infer_expr_type_ref(ctx, stmt->as.assign_stmt.target);
            bool is_mod_ref = target_tr && target_tr->is_mod;
            bool is_prim_mod_ref = is_mod_ref && is_primitive_type(get_base_type_name(target_tr));

            if (is_prim_mod_ref) {
                // *rx.ptr = value
                fprintf(out, "*");
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, true);
                fprintf(out, ".ptr = ");
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
            } else if (is_mod_ref) {
                // *r = value (for non-primitive mod refs like mod Point)
                fprintf(out, "*");
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, true);
                fprintf(out, " = ");
                // Add compound literal cast for struct literals
                if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT &&
                    !stmt->as.assign_stmt.value->as.object_literal.type && target_tr) {
                    fprintf(out, "(");
                    emit_type_ref_as_c_type(ctx, target_tr, out, true);
                    fprintf(out, ")");
                }
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
            } else {
                bool had_exp = ctx->has_expected_type;
                AstTypeRef saved_exp = ctx->expected_type;
                if (target_tr) { ctx->expected_type = *target_tr; ctx->has_expected_type = true; }
                // String local reassignment: drop the previous heap
                // before storing the new one, or it leaks. RHS may
                // reference the target (e.g. `s = s.concat(other)`),
                // so evaluate RHS into a temp first, then drop, then
                // pool_take the new heap. Safe even when target was
                // aliasing a list buffer thanks to the let-stmt's
                // is_owned-clearing pass — rae_string_drop no-ops on
                // is_owned=0 entries.
                bool target_is_string = target_tr
                    && !target_tr->is_view && !target_tr->is_mod
                    && !target_tr->is_opt
                    && str_eq_cstr(get_base_type_name(target_tr), "String");
                bool target_is_opt = target_tr
                    && target_tr->is_opt
                    && !target_tr->is_view && !target_tr->is_mod;
                bool is_string_local_reassign = target_is_string
                    && stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT;
                // Struct-field String reassign: `s.text = newVal` drops
                // the previous heap held by s.text first, then takes
                // the new one. Closes the per-iter leak in the ECS
                // applyOverride-style replace pattern where each call
                // overwrote a String field without releasing the old.
                bool is_string_field_reassign = target_is_string
                    && stmt->as.assign_stmt.target->kind == AST_EXPR_MEMBER;
                // Borrow -> owned heap-struct assignment: `dst = src`
                // where dst is an owned (non-view/mod) struct that owns
                // heap (List/String/etc. fields) and src is a borrow
                // (`view`/`mod`). A borrow can't be moved, so a shallow
                // copy would alias src's heap into dst — and when both
                // owners drop, that heap is freed twice (the MsdfFont
                // double-free seen on world rebuild / teardown:
                // `world.msdfState.font = font` where font is view).
                // Deep-copy instead, dropping dst's previous heap first.
                // Mirrors the `let b: T = a` owning-let deep-copy path.
                // String is handled by the dedicated cases above; the
                // move case (bare owned local) is handled by the
                // mark_expr_moved analysis earlier, so it never reaches
                // here as a borrow.
                const AstTypeRef* asg_val_tr =
                    infer_expr_type_ref(ctx, stmt->as.assign_stmt.value);
                bool value_is_borrow = asg_val_tr
                    && (asg_val_tr->is_view || asg_val_tr->is_mod);
                bool is_struct_deepcopy_from_borrow = value_is_borrow
                    && !target_is_string
                    && target_tr && !target_tr->is_view && !target_tr->is_mod
                    && type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                            target_tr, 0);
                if (target_is_opt) {
                    int tmpn = ctx->temp_counter++;
                    bool opt_struct = target_tr && !(target_tr->is_view || target_tr->is_mod)
                        && rae_opt_is_struct_rep(ctx, target_tr);
                    const char* opt_c = opt_struct ? rae_opt_type_name(ctx, target_tr) : "RaeAny";
                    fprintf(out, "{ %s __asg%d = ", opt_c, tmpn);
                    emit_optional_boxed_expr(ctx, target_tr,
                                             stmt->as.assign_stmt.value, out);
                    fprintf(out, "; %s* __asgp%d = &(", opt_c, tmpn);
                    emit_expr(ctx, stmt->as.assign_stmt.target, out,
                              PREC_LOWEST, true, false);
                    bool opt_needs_drop = opt_struct
                        && type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, target_tr, 0);
                    if (opt_needs_drop)
                        fprintf(out, "); rae_drop_%s(__asgp%d); *__asgp%d = __asg%d; }",
                                opt_c, tmpn, tmpn, tmpn);
                    else if (opt_struct)
                        fprintf(out, "); *__asgp%d = __asg%d; }", tmpn, tmpn);
                    else
                        fprintf(out, "); rae_any_drop(__asgp%d); *__asgp%d = __asg%d; }",
                                tmpn, tmpn, tmpn);
                    ctx->has_expected_type = had_exp;
                    ctx->expected_type = saved_exp;
                } else if (is_string_local_reassign) {
                    Str tname = stmt->as.assign_stmt.target->as.ident;
                    int tmpn = ctx->temp_counter++;
                    fprintf(out, "{ rae_String __asg%d = ", tmpn);
                    emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
                    fprintf(out, "; rae_string_drop(&%.*s); %.*s = rae_string_pool_take(__asg%d); }",
                            (int)tname.len, tname.data,
                            (int)tname.len, tname.data,
                            tmpn);
                    ctx->has_expected_type = had_exp;
                    ctx->expected_type = saved_exp;
                } else if (is_string_field_reassign) {
                    // Evaluate RHS into a temp first so it doesn't read
                    // the slot we're about to drop. Then take a stable
                    // address of the target via &, drop it, store the
                    // new value through the same address.
                    //
                    // Phase-2-style RHS classification: CALL / INTERP /
                    // BINARY produce a freshly-owned temp — pool_take
                    // transfers it. IDENT / MEMBER potentially alias
                    // another live owner — deep-copy via rae_string_copy
                    // so the field gets a private heap.
                    const AstExpr* rhs = stmt->as.assign_stmt.value;
                    bool rhs_owning_temp = rhs && (
                        rhs->kind == AST_EXPR_CALL ||
                        rhs->kind == AST_EXPR_METHOD_CALL ||
                        rhs->kind == AST_EXPR_INTERP ||
                        rhs->kind == AST_EXPR_BINARY ||
                        rhs->kind == AST_EXPR_OWN);
                    int tmpn = ctx->temp_counter++;
                    fprintf(out, "{ rae_String __asg%d = ", tmpn);
                    emit_expr(ctx, rhs, out, PREC_LOWEST, false, false);
                    fprintf(out, "; rae_String* __asgp%d = &(", tmpn);
                    emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, false);
                    if (rhs_owning_temp) {
                        fprintf(out, "); rae_string_drop(__asgp%d); *__asgp%d = rae_string_pool_take(__asg%d); }",
                                tmpn, tmpn, tmpn);
                    } else {
                        fprintf(out, "); rae_String __asgc%d = rae_string_copy(__asg%d); rae_string_drop(__asgp%d); *__asgp%d = __asgc%d; }",
                                tmpn, tmpn, tmpn, tmpn, tmpn);
                    }
                    ctx->has_expected_type = had_exp;
                    ctx->expected_type = saved_exp;
                } else if (is_struct_deepcopy_from_borrow) {
                    // { T __asg; rae_deep_copy_T(&__asg, &(src));
                    //   T* __asgp = &(dst); rae_drop_struct_T(__asgp);
                    //   *__asgp = __asg; }
                    // Deep-copy src into a temp, then drop dst's old heap
                    // and store the independent copy. Evaluate the copy
                    // before dropping dst so a src that reads dst stays
                    // valid. `&(src)` is well-formed: a borrow emits as a
                    // deref lvalue (`(*font)`), so `&((*font))` is the ptr.
                    const char* tn_dc = rae_mangle_type_specialized(
                        ctx->compiler_ctx, ctx->generic_params,
                        ctx->generic_args, target_tr);
                    int tmpn = ctx->temp_counter++;
                    fprintf(out, "{ %s __asg%d; rae_deep_copy_%s(&__asg%d, &(",
                            tn_dc, tmpn, tn_dc, tmpn);
                    emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
                    fprintf(out, ")); %s* __asgp%d = &(", tn_dc, tmpn);
                    emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, false);
                    fprintf(out, "); rae_drop_struct_%s(__asgp%d); *__asgp%d = __asg%d; }",
                            tn_dc, tmpn, tmpn, tmpn);
                    ctx->has_expected_type = had_exp;
                    ctx->expected_type = saved_exp;
                } else {
                    emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, false);
                    fprintf(out, " = ");
                    if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT &&
                        !stmt->as.assign_stmt.value->as.object_literal.type && target_tr) {
                        fprintf(out, "(");
                        emit_type_ref_as_c_type(ctx, target_tr, out, true);
                        fprintf(out, ")");
                    }
                    bool wrap_str_take_a = false;
                    if (target_tr && !target_tr->is_view && !target_tr->is_mod
                        && !target_tr->is_opt) {
                        Str tbase = get_base_type_name(target_tr);
                        if (str_eq_cstr(tbase, "String")) wrap_str_take_a = true;
                    }
                    if (wrap_str_take_a) fprintf(out, "rae_string_pool_take(");
                    emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
                    if (wrap_str_take_a) fprintf(out, ")");
                    ctx->has_expected_type = had_exp;
                    ctx->expected_type = saved_exp;
                }
            }
            fprintf(out, ";\n");
            break;
        }
        case AST_STMT_RET: {
            // Stage 3 move tracking: returning an owned local moves
            // it out of the function. mark_expr_moved tells the drop
            // pass below to skip its auto-drop.
            if (stmt->as.ret_stmt.values && stmt->as.ret_stmt.values->value) {
                mark_expr_moved_if_local(ctx, stmt->as.ret_stmt.values->value);
            }

            // Stage 7 cleanup epilogue: every ret runs the same drops +
            // pool flush that fallthrough end-of-body runs. Without
            // this, parsing-heavy functions (parseScene, deserOnClick,
            // …) that always return via early `ret` skip the cleanup
            // and leak every heap String they alloc'd during the call.
            //
            // Shape:
            //   { RetT __ret_val = <value>;                       // before drops
            //     __ret_val = rae_string_pool_take(__ret_val);    // String returns only
            //     defers
            //     emit_implicit_drops_for_body(...)               // dropped locals
            //     rae_string_pool_flush(__rae_spm_func);          // pool sweep
            //     return __ret_val; }
            //
            // The pool_take detaches the return value from the temp
            // pool so the subsequent flush doesn't free what the caller
            // is about to receive. Move tracking above keeps the drop
            // pass from freeing a local whose data IS the return value.
            const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
            if (ret_type && ctx->generic_params && ctx->generic_args) {
                ret_type = substitute_type_ref(ctx->compiler_ctx,
                                               ctx->generic_params,
                                               ctx->generic_args, ret_type);
            }
            bool has_value = stmt->as.ret_stmt.values && stmt->as.ret_stmt.values->value;
            bool is_main_fn = ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "main");

            fprintf(out, "  {\n");

            if (has_value) {
                const char* rt = c_return_type(ctx, ctx->func_decl);
                fprintf(out, "    %s __ret_val = ", rt);
                bool is_ref_return = ret_type && (ret_type->is_view || ret_type->is_mod);
                // `ret none` from a function returning an optional REFERENCE is
                // a null pointer (spec 4.1), not a boxed none whose address is
                // then taken.
                if (ret_type && ret_type->is_opt && is_ref_return
                    && stmt->as.ret_stmt.values->value->kind == AST_EXPR_NONE) {
                    fprintf(out, "NULL;\n");
                    emit_implicit_drops_for_body(ctx, out, ctx->func_first_let_idx);
                    fprintf(out, "    return __ret_val;\n  }\n");
                    break;
                }
                bool is_prim_ref_return = is_ref_return && is_primitive_type(get_base_type_name(ret_type));

                // === Stage 4: return-by-deep-copy for owning types ===
                //
                // When the function's declared return type owns heap and
                // the returned expression is an *alias source* — e.g.
                // an IDENT bound to a `view`/`mod` parameter, or a
                // member/index access whose receiver could still be
                // owning at scope-exit — we must deep-copy so the
                // caller gets an independent buffer. Without this, the
                // caller's local and the original storage both drop the
                // same heap at scope end → double-free.
                //
                // Cases that DON'T need wrapping (already correct):
                //   - Fresh rvalues (CALL/METHOD_CALL/BINARY/INTERP/
                //     OBJECT/CONCAT) — the value is already a freshly
                //     owned heap, just transfer.
                //   - IDENT bound to an owning local / own / copy
                //     parameter — Stage 3's mark_expr_moved_if_local
                //     above already flagged it; the implicit-drop pass
                //     skips it. The buffer transfers to the caller.
                //   - Explicit `own X` at the return site — operand
                //     gets its move mark via mark_expr_moved_if_local
                //     recursion; no copy needed.
                const AstExpr* ret_val = stmt->as.ret_stmt.values->value;
                bool wrap_ret_string_copy = false;
                bool wrap_ret_deep_copy = false;
                bool src_is_pointer_ident = false; // view List / view Buffer ident
                if (ret_type
                    && !ret_type->is_view && !ret_type->is_mod && !ret_type->is_opt
                    && ret_val->kind != AST_EXPR_OWN
                    && type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                            ret_type, 0)) {
                    bool is_alias_source = false;
                    if (ret_val->kind == AST_EXPR_IDENT) {
                        // Only wrap when the IDENT refers to a
                        // borrow (view/mod) — owning local /
                        // own param / copy param are move-tracked
                        // above and transfer ownership cleanly.
                        Str name = ret_val->as.ident;
                        for (int i = (int)ctx->local_count - 1; i >= 0; i--) {
                            if (str_eq(ctx->locals[i], name)) {
                                const AstTypeRef* lt = ctx->local_type_refs[i];
                                if (lt && (lt->is_view || lt->is_mod)) {
                                    is_alias_source = true;
                                    Str lb = get_base_type_name(lt);
                                    // view List(E) / view Buffer
                                    // lower to a raw T* at the C
                                    // level — the IDENT itself is
                                    // the pointer we hand to
                                    // rae_deep_copy_<T>.
                                    if (str_eq_cstr(lb, "List") || str_eq_cstr(lb, "Buffer")) {
                                        src_is_pointer_ident = true;
                                    }
                                }
                                break;
                            }
                        }
                    } else if (ret_val->kind == AST_EXPR_MEMBER
                               || ret_val->kind == AST_EXPR_INDEX) {
                        // Member/index access aliases through the
                        // container; whether the container is view/
                        // mod or owning, the container's scope-exit
                        // drop would collide with the caller's drop
                        // of the returned buffer. Always wrap.
                        is_alias_source = true;
                    }
                    if (is_alias_source) {
                        Str rbase_dc = get_base_type_name(ret_type);
                        if (str_eq_cstr(rbase_dc, "String")) {
                            wrap_ret_string_copy = true;
                        } else {
                            wrap_ret_deep_copy = true;
                        }
                    }
                }

                if (is_prim_ref_return && !ret_type->is_opt) {
                    fprintf(out, "("); emit_type_ref_as_c_type(ctx, ret_type, out, false);
                    fprintf(out, "){ .ptr = &"); emit_expr(ctx, ret_val, out, PREC_LOWEST, true, true);
                    fprintf(out, " }");
                } else if (is_ref_return) {
                    /* Taking the address of a Rae function's result is
                     * wrong in both directions, and which way depends on
                     * what the callee hands back.
                     *
                     * Forwarding a reference — `ret view nowPlaying(...)`
                     * where nowPlaying itself returns `opt view Track` —
                     * already has a pointer in hand, so `&` would build a
                     * pointer-to-pointer. Emit the call bare.
                     *
                     * Returning a reference to a VALUE result — the
                     * `tracks.at(index: i)` shape, where `at` is declared
                     * `ret T` — names a temporary that dies with this
                     * frame. That is a Rae error, not something to lower
                     * into C that cannot build.
                     *
                     * Intrinsics and externs are the exception, and they
                     * are why this cannot be a blanket "calls are not
                     * lvalues" rule: `ret view rae_ext_rae_buf_get(...)`
                     * is how the standard library spells a reference INTO
                     * a buffer (componentView, componentMod, sceneNodeAt).
                     * It lowers to element indexing, so it is an lvalue
                     * and `&` on it is exactly right. */
                    const AstExpr* inner = ret_val;
                    if (inner->kind == AST_EXPR_UNARY
                        && (inner->as.unary.op == AST_UNARY_VIEW
                            || inner->as.unary.op == AST_UNARY_MOD)) {
                        inner = inner->as.unary.operand;
                    }
                    const AstDecl* dl = inner->decl_link;
                    bool callee_is_rae_func = dl && dl->kind == AST_DECL_FUNC
                                              && !dl->as.func_decl.is_extern;
                    /* Does the call hand back a reference? ref_bind_value_
                     * returns_ref resolves by NAME for both plain calls and
                     * UFCS method calls, so it catches `tracks.viewAt(...)`
                     * where sema left no decl_link — the value-returning
                     * `tracks.at(...)` stays false and is rejected below. */
                    bool yields_ref =
                        ref_bind_value_returns_ref(ctx, inner)
                        || (inner->resolved_type && inner->resolved_type->kind == TYPE_REF);
                    bool is_temp_call = (inner->kind == AST_EXPR_METHOD_CALL && !yields_ref)
                                        || (inner->kind == AST_EXPR_CALL && callee_is_rae_func
                                            && !yields_ref);
                    bool forwards_ref = (inner->kind == AST_EXPR_CALL
                                         || inner->kind == AST_EXPR_METHOD_CALL) && yields_ref;
                    if (is_temp_call) {
                        diag_error(ctx->module ? ctx->module->file_path : NULL,
                                   (int)ret_val->line, (int)ret_val->column,
                                   "cannot return a reference to a temporary: this call returns a "
                                   "value, which does not outlive the function");
                    }
                    if (!forwards_ref && !is_temp_call) fprintf(out, "&");
                    emit_expr(ctx, (forwards_ref || is_temp_call) ? inner : ret_val,
                              out, PREC_UNARY, true, true);
                } else if (wrap_ret_string_copy) {
                    // String alias source — deep-copy via rae_string_copy
                    // so the caller's binding owns an independent heap.
                    fprintf(out, "rae_string_copy(");
                    emit_expr(ctx, ret_val, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                } else if (wrap_ret_deep_copy) {
                    // Container / user-struct alias source — deep-copy
                    // via the synthesised rae_deep_copy_<T> helper.
                    // Statement-expression: declare a temp of the
                    // return C type, run the helper, evaluate to the
                    // temp. The temp is detached storage; caller takes
                    // ownership.
                    const char* tn_dc = rae_mangle_type_specialized(
                        ctx->compiler_ctx, ctx->generic_params,
                        ctx->generic_args, ret_type);
                    int tmp_id = ctx->temp_counter++;
                    fprintf(out, "(__extension__ ({ %s __rdc%d; rae_deep_copy_%s(&__rdc%d, ",
                            tn_dc, tmp_id, tn_dc, tmp_id);
                    if (src_is_pointer_ident) {
                        // view List/Buffer IDENT is already a T*.
                        // emit_expr with suppress_deref=true keeps
                        // it as the bare pointer name.
                        emit_expr(ctx, ret_val, out, PREC_LOWEST, false, true);
                    } else {
                        // Other forms: emit_expr produces an lvalue
                        // of T (member/index, owning-local IDENT,
                        // view-non-List struct IDENT auto-derefs to
                        // `(*name)`). Take its address.
                        fprintf(out, "&(");
                        emit_expr(ctx, ret_val, out, PREC_LOWEST, false, false);
                        fprintf(out, ")");
                    }
                    fprintf(out, "); __rdc%d; }))", tmp_id);
                } else {
                    bool needs_any_wrap = ret_type && (ret_type->is_opt || str_eq_cstr(get_base_type_name(ret_type), "Any"));
                    bool val_is_box = ret_val->kind == AST_EXPR_BOX;
                    if (needs_any_wrap && !val_is_box) {
                        if (ret_type && ret_type->is_opt) {
                            emit_optional_boxed_expr(ctx, ret_type, ret_val, out);
                        } else {
                            fprintf(out, "rae_any((");
                            emit_expr(ctx, ret_val, out, PREC_LOWEST, false, false);
                            fprintf(out, "))");
                        }
                    } else emit_expr(ctx, ret_val, out, PREC_LOWEST, false, false);
                }
                fprintf(out, ";\n");

                // String return: detach from pool so the flush doesn't
                // free the heap the caller is about to claim. Skip for
                // `opt String` (the C type is RaeAny, not rae_String)
                // and for view/mod refs (which are pointer wrappers).
                if (ret_type && !ret_type->is_view && !ret_type->is_mod && !ret_type->is_opt) {
                    Str rbase = get_base_type_name(ret_type);
                    if (str_eq_cstr(rbase, "String")) {
                        fprintf(out, "    __ret_val = rae_string_pool_take(__ret_val);\n");
                    }
                }
            }

            if (ctx->defer_stack.count > 0) emit_defers(ctx, 0, out);
            if (ctx->func_first_let_idx != (size_t)-1) {
                emit_implicit_drops_for_body(ctx, out, ctx->func_first_let_idx);
                emit_implicit_drops_for_own_params(ctx, out, ctx->func_first_let_idx);
            }
            fprintf(out, "    rae_string_pool_flush(__rae_spm_func);\n");

            // Stage 8: re-register an owned String return into the
            // caller's pool so nested call chains (e.g.
            // `concat(concat(a, b), c)`) don't dangle. The inner
            // `concat` would otherwise return owned heap detached
            // from any pool; the outer call reads the bytes via view
            // String and never frees the inner. Re-registering after
            // the callee's flush puts the result back in the active
            // pool, where the *caller's* surrounding mark/flush (let,
            // assign, expr-stmt, ret) sweeps it if no `pool_take`
            // claims it. `pool_register_owned` is a no-op for
            // literal-backed / view / NULL returns.
            if (has_value && ret_type && !ret_type->is_view && !ret_type->is_mod && !ret_type->is_opt) {
                Str rbase2 = get_base_type_name(ret_type);
                if (str_eq_cstr(rbase2, "String")) {
                    fprintf(out, "    __ret_val = rae_string_pool_register_owned(__ret_val);\n");
                }
            }

            if (has_value) {
                fprintf(out, "    return __ret_val;\n");
            } else if (is_main_fn) {
                fprintf(out, "    return 0;\n");
            } else if (ret_type) {
                fprintf(out, "    return ");
                // emit_auto_init handles opt (RaeAny none / struct-rep {0}).
                emit_auto_init(ctx, ret_type, out);
                fprintf(out, ";\n");
            } else {
                fprintf(out, "    return;\n");
            }
            fprintf(out, "  }\n");
            break;
        }
        case AST_STMT_IF: emit_if(ctx, stmt, out); break;
        case AST_STMT_LOOP: emit_loop(ctx, stmt, out); break;
        case AST_STMT_MATCH: {
            const AstExpr* subject = stmt->as.match_stmt.subject;
            // A String subject compares by value (rae_ext_rae_str_eq), not `==`;
            // the type is the same for every case, so resolve it once.
            Str subj_base = get_base_type_name(infer_expr_type_ref(ctx, subject));
            bool subject_is_string = str_eq_cstr(subj_base, "String")
                || str_eq_cstr(subj_base, "rae_String");
            bool first = true;
            for (const AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next) {
                if (!c->pattern) {
                    // default case
                    if (!first) fprintf(out, " else {\n");
                    else fprintf(out, "  {\n");
                } else {
                    fprintf(out, first ? "  if (" : " else if (");
                    // First pattern, then each or-pattern (`case A, B, C`),
                    // joined by `||` so any of them enters the arm.
                    emit_match_case_test(ctx, subject, c->pattern, subject_is_string, out);
                    for (const AstCasePattern* op = c->or_patterns; op; op = op->next) {
                        fprintf(out, " || ");
                        emit_match_case_test(ctx, subject, op->expr, subject_is_string, out);
                    }
                    fprintf(out, ") {\n");
                }
                first = false;
                // Each case body is its own scope: save/restore local_count and
                // emit its owned-local drops before the closing brace, so a `let`
                // of an owned value inside a case drops where its C scope ends
                // (mirrors emit_if's per-branch handling) instead of leaking to
                // the enclosing scope's drop pass, which would emit the drop on an
                // out-of-scope C identifier.
                size_t saved_locals_case = ctx->local_count;
                if (c->block) {
                    for (AstStmt* s = c->block->first; s; s = s->next)
                        emit_stmt(ctx, s, out);
                }
                emit_implicit_drops_for_body(ctx, out, saved_locals_case);
                ctx->local_count = saved_locals_case;
                fprintf(out, "  }");
            }
            fprintf(out, "\n");
            break;
        }
        case AST_STMT_BREAK:
        case AST_STMT_CONTINUE: {
            // Non-local loop exit: drop every owned local declared inside the
            // loop body up to here (reverse-construction order), then jump.
            // Sema has already rejected these outside a loop; guard anyway.
            if (ctx->loop_depth > 0) {
                emit_implicit_drops_for_body(
                    ctx, out, ctx->loop_body_local_start[ctx->loop_depth - 1]);
            }
            fprintf(out, stmt->kind == AST_STMT_BREAK ? "  break;\n" : "  continue;\n");
            break;
        }
        case AST_STMT_DEFER: {
            // Push defer block onto stack — will be emitted before returns and at function end
            if (ctx->defer_stack.count < 64) {
                ctx->defer_stack.entries[ctx->defer_stack.count].block = stmt->as.defer_stmt.block;
                ctx->defer_stack.entries[ctx->defer_stack.count].scope_depth = 0;
                ctx->defer_stack.count++;
            }
            break;
        }
        default: break;
    }
    return true;
}

bool emit_defers(CFuncContext* ctx, int min_depth, FILE* out) {
    // Emit deferred blocks in reverse order (LIFO)
    for (int i = ctx->defer_stack.count - 1; i >= min_depth; i--) {
        const AstBlock* block = ctx->defer_stack.entries[i].block;
        if (block) {
            for (AstStmt* s = block->first; s; s = s->next)
                emit_stmt(ctx, s, out);
        }
    }
    return true;
}
void pop_defers(CFuncContext* ctx, int depth) {
    while (ctx->defer_stack.count > depth) ctx->defer_stack.count--;
}
