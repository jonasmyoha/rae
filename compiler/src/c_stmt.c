// c_stmt.c — Statement emission for the C backend.
//
// `emit_stmt` is the per-AST-node switch for Rae statements. Helper emitters
// for `if`, `for`-style loops, and `match` live here too; defer-stack
// bookkeeping (used by ret/scope-exit) is also here since it's purely
// statement-scoped state.

#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "sema.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// File-local helpers.
static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out);
static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out);

// Stage 2 of scope-exit dealloc (see docs/scope-exit-dealloc.md).
// Predicate: is the type something whose owning binding should
// trigger a `drop()` call when it goes out of scope? Today this is
// just the heap-owning stdlib containers; Stage 5 will extend it to
// user structs that contain heap-owning fields.
//
// Public via c_backend_internal.h so the discovery pass can pre-
// register the matching drop specialisation. If we only registered
// during the emit pass, the drop specialisation would land in the
// output AFTER its first call site (specialised functions are emitted
// before the regular functions that use them), and the C compiler
// would warn about an implicit declaration.
bool is_drop_target_type(const AstTypeRef* type);
const AstFuncDecl* find_drop_overload_for(CFuncContext* ctx, Str container_base);

bool is_drop_target_type(const AstTypeRef* type) {
  if (!type) return false;
  // Borrows don't own — they're someone else's value.
  if (type->is_view || type->is_mod) return false;
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "List")) return true;
  if (str_eq_cstr(base, "StringMap")) return true;
  if (str_eq_cstr(base, "IntMap")) return true;
  return false;
}

// Layer 5 (docs/scope-exit-dealloc.md) — does the type transitively
// own heap storage? A type owns heap if:
//   - it's a leaf heap-owning stdlib container (is_drop_target_type),
//   - or it's a user-defined struct with at least one field whose
//     type owns heap.
// `depth` guards against runaway recursion if the type graph ever
// has a cycle (Rae structs are value types so this shouldn't happen,
// but the cap is cheap insurance).
// "Strict" heap-ownership: true only when the type transitively owns
// container heap (List / StringMap / IntMap), not when its only heap
// connection is a String field. Used by the local auto-drop pass to
// avoid double-frees on the by-value-return-shallow-alias pattern —
// e.g. `let r: JsonValue = jsonRoot(d)` shallow-copies asString.data
// out of the doc's list, so freeing it on `r`'s scope exit would
// double-free with the eventual drop of the JsonDoc list.
//
// The looser predicate `type_has_drop_target_field` (below) is true
// when a String field is present and drives cascade emission inside
// already-firing struct/element drops, where the ownership chain is
// unambiguous (the container owns its elements).
bool type_owns_heap_storage(CompilerContext* cctx, const AstModule* module,
                            const AstTypeRef* type, int depth) {
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (is_drop_target_type(type)) return true;
  Str base = get_base_type_name(type);
  // c_struct (raylib Color / Vector2 / etc.) and primitives never
  // own Rae-allocated heap storage.
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    if (type_owns_heap_storage(cctx, module, f->type, depth + 1)) return true;
  }
  return false;
}

// Permissive variant for cascade-drop sites — also true when a field
// is a String or when any nested field is a String. Used by Layer 5
// struct drop synthesis, List/Map element drop synthesis, and (since
// Phase 3) the local auto-drop pass. The alias-safety gate moved to
// `local_struct_owns_heap` in emit_implicit_drops_for_body so this
// predicate can stay purely structural.
bool type_needs_cascade_drop(CompilerContext* cctx, const AstModule* module,
                             const AstTypeRef* type, int depth) {
  if (!type || depth > 32) return false;
  if (type->is_view || type->is_mod) return false;
  if (is_drop_target_type(type)) return true;
  Str base = get_base_type_name(type);
  if (str_eq_cstr(base, "String")) return true;
  const AstDecl* d = find_type_decl(NULL, module, base);
  if (!d || d->kind != AST_DECL_TYPE) return false;
  if (has_property(d->as.type_decl.properties, "c_struct")) return false;
  for (const AstTypeField* f = d->as.type_decl.fields; f; f = f->next) {
    if (type_needs_cascade_drop(cctx, module, f->type, depth + 1)) return true;
  }
  return false;
}

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
        } else if (rv->kind == AST_EXPR_MEMBER
                   && rv->as.member.object
                   && rv->as.member.object->kind == AST_EXPR_IDENT) {
          // `ret <ident>.<member>` where the ident was bound to an
          // aliasing extraction is the canonical accessor return for
          // String fields out of aliased structs (jsonObjectKeyAt's
          // `ret f.key` where f = fieldAt(...) = buf_get alias).
          const AstStmt* ls = find_let_for_ident(first, rv->as.member.object->as.ident);
          if (let_init_is_aliasing(cctx, ls, v)) return true;
        }
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

bool emit_implicit_drops_for_body(CFuncContext* ctx, FILE* out,
                                  size_t first_let_index) {
  if (!ctx || !out) return false;
  for (size_t i = ctx->local_count; i > first_let_index; i--) {
    size_t idx = i - 1;
    const AstTypeRef* type = ctx->local_type_refs[idx];
    if (!type) continue;
    if (type->is_view || type->is_mod) continue;
    if (ctx->local_moved[idx]) continue;
    // Skip cheap value types — they own no heap and don't need a
    // drop call. Permissive predicate so String-only owning structs
    // are eligible too — alias safety is gated by local_struct_owns_heap
    // below.
    if (!type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0)) {
      continue;
    }
    Str name = ctx->locals[idx];
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
      // may alias the source (alias variant). Generic-instance
      // structs (Stack(Int), ComponentTable(T)) still need user-
      // defined `drop(T)` overloads — Pass A only synthesises
      // rae_drop_struct_ helpers for non-generic structs.
      if (type->generic_args) continue;
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
    fprintf(out, "  if (");
    emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST, false, false);
    fprintf(out, ") {\n");
    // Stage 2 scope tracking: save/restore local_count around each
    // branch so lets declared inside the block don't pollute the
    // outer scope's `ctx->locals` view. Without this, the end-of-body
    // drop pass would try to drop names that the C compiler can't
    // see (out-of-scope C identifiers).
    size_t saved_locals_then = ctx->local_count;
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
    return true;
}

static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    fprintf(out, "  for (");
    // Save outer local_count so the loop's init-let + body-lets all
    // disappear from the locals view when the loop ends.
    size_t saved_locals = ctx->local_count;
    if (stmt->as.loop_stmt.init) {
        // Init stmt usually doesn't have a newline/indent in for loop
        if (stmt->as.loop_stmt.init->kind == AST_STMT_LET) {
            const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.loop_stmt.init->as.let_stmt.type);
            fprintf(out, "%s %.*s = ", tn, (int)stmt->as.loop_stmt.init->as.let_stmt.name.len, stmt->as.loop_stmt.init->as.let_stmt.name.data);
            emit_expr(ctx, stmt->as.loop_stmt.init->as.let_stmt.value, out, PREC_LOWEST, false, false);
        } else {
            emit_expr(ctx, stmt->as.loop_stmt.init->as.expr_stmt, out, PREC_LOWEST, false, false);
        }
    }
    fprintf(out, "; ");
    if (stmt->as.loop_stmt.condition) emit_expr(ctx, stmt->as.loop_stmt.condition, out, PREC_LOWEST, false, false);
    fprintf(out, "; ");
    if (stmt->as.loop_stmt.increment) emit_expr(ctx, stmt->as.loop_stmt.increment, out, PREC_LOWEST, false, false);
    fprintf(out, ") {\n");
    if (stmt->as.loop_stmt.body) {
        for (const AstStmt* s = stmt->as.loop_stmt.body->first; s; s = s->next) emit_stmt(ctx, s, out);
    }
    emit_implicit_drops_for_body(ctx, out, saved_locals);
    ctx->local_count = saved_locals;
    fprintf(out, "  }\n");
    return true;
}

static bool emit_match(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    (void)ctx; (void)stmt; (void)out;
    fprintf(stderr, "warning: match stmt not yet implemented in unified C backend\n");
    return true;
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
            fprintf(out, "  ");
            emit_type_ref_as_c_type(ctx, stmt->as.let_stmt.type, out, false);
            fprintf(out, " %.*s = ", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
            bool is_ref_bind = stmt->as.let_stmt.is_bind && stmt->as.let_stmt.type &&
                               (stmt->as.let_stmt.type->is_view || stmt->as.let_stmt.type->is_mod);
            if (is_ref_bind) {
                Str base = get_base_type_name(stmt->as.let_stmt.type);
                if (is_primitive_type(base)) {
                    // Check if the value is a function call returning a ref type
                    // (can't take address of rvalue — assign directly)
                    bool value_returns_ref = false;
                    if (stmt->as.let_stmt.value && (stmt->as.let_stmt.value->kind == AST_EXPR_CALL || stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL)) {
                        const AstExpr* val = stmt->as.let_stmt.value;
                        const AstFuncDecl* vfd = val->decl_link ? &val->decl_link->as.func_decl : NULL;
                        if (vfd && vfd->returns && vfd->returns->type && (vfd->returns->type->is_view || vfd->returns->type->is_mod))
                            value_returns_ref = true;
                    }
                    if (value_returns_ref) {
                        // Function already returns ref wrapper — assign directly
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                    } else {
                        // Primitive ref: rae_Mod_Int64 r = { .ptr = &x };
                        fprintf(out, "{ .ptr = &");
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    }
                } else {
                    bool value_returns_ref = false;
                    if (stmt->as.let_stmt.value && (stmt->as.let_stmt.value->kind == AST_EXPR_CALL || stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL)) {
                        const AstExpr* val = stmt->as.let_stmt.value;
                        const AstFuncDecl* vfd = val->decl_link ? &val->decl_link->as.func_decl : NULL;
                        if (vfd && vfd->returns && vfd->returns->type && (vfd->returns->type->is_view || vfd->returns->type->is_mod))
                            value_returns_ref = true;
                        // Sema doesn't always populate decl_link on free-
                        // function call sites — c_call.c re-resolves by
                        // name from the all_decls list. Mirror that
                        // lookup here so we can ask "does the call return
                        // view T / mod T?".
                        if (!value_returns_ref && val->kind == AST_EXPR_CALL && val->as.call.callee
                            && val->as.call.callee->kind == AST_EXPR_IDENT) {
                            Str callee = val->as.call.callee->as.ident;
                            for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                                const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                                if (d->kind != AST_DECL_FUNC) continue;
                                if (!str_eq(d->as.func_decl.name, callee)) continue;
                                const AstFuncDecl* cfd = &d->as.func_decl;
                                if (cfd->returns && cfd->returns->type
                                    && (cfd->returns->type->is_view || cfd->returns->type->is_mod)) {
                                    value_returns_ref = true;
                                    break;
                                }
                            }
                        }
                    }
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
                if (ctx->local_count < 256) { ctx->locals[ctx->local_count] = stmt->as.let_stmt.name; ctx->local_types[ctx->local_count] = str_from_cstr(tn); ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type; ctx->local_count++; }
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
                if (needs_box) fprintf(out, "rae_any((");
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
                    && !stmt->as.let_stmt.type->is_mod) {
                    Str lbase = get_base_type_name(stmt->as.let_stmt.type);
                    if (str_eq_cstr(lbase, "String")) wrap_str_take = true;
                }
                if (wrap_str_take) fprintf(out, "rae_string_pool_take(");
                emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                if (wrap_str_take) fprintf(out, ")");
                if (needs_box) fprintf(out, "))");
                ctx->has_expected_type = false;
            } else {
                // Auto-init: let x: Type (no initializer)
                emit_auto_init(ctx, stmt->as.let_stmt.type, out);
            }
            fprintf(out, ";\n");
            const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
            if (ctx->local_count < 256) {
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
                bool owns = (init == NULL) || (init->kind == AST_EXPR_OBJECT);
                if (!owns && init && init->kind == AST_EXPR_CALL) {
                  const AstExpr* callee = init->as.call.callee;
                  if (callee && callee->kind == AST_EXPR_IDENT) {
                    Str cn = callee->as.ident;
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
                      // This avoids the Stage-6 whitelist treadmill
                      // (jsonRoot / jsonString deep-copy but match the
                      // earlier "view T first param" signature).
                      owns = true;
                      for (size_t k = 0; k < ctx->compiler_ctx->all_decl_count; k++) {
                        const AstDecl* d = ctx->compiler_ctx->all_decls[k];
                        if (d->kind != AST_DECL_FUNC) continue;
                        if (!str_eq(d->as.func_decl.name, cn)) continue;
                        // Walk the body looking for `ret <ident>` whose
                        // <ident> was let-init from buf_get. This is the
                        // accessor pattern (valueAt, fieldAt, jsonArrayAt).
                        // Only handles the top-level body — inlined ret
                        // expressions inside if/loop are also walked.
                        if (!d->as.func_decl.body) break;
                        owns = !rae_func_returns_alias(ctx->compiler_ctx, &d->as.func_decl);
                        break;
                      }
                    }
                  } else {
                    owns = true;
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
                if (!owns && stmt->as.let_stmt.type
                    && !stmt->as.let_stmt.type->is_view
                    && !stmt->as.let_stmt.type->is_mod
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
                bool is_string_local_reassign =
                    target_tr && !target_tr->is_view && !target_tr->is_mod
                    && stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT
                    && str_eq_cstr(get_base_type_name(target_tr), "String");
                if (is_string_local_reassign) {
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
                    if (target_tr && !target_tr->is_view && !target_tr->is_mod) {
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
            bool has_value = stmt->as.ret_stmt.values && stmt->as.ret_stmt.values->value;
            bool is_main_fn = ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "main");

            fprintf(out, "  {\n");

            if (has_value) {
                const char* rt = c_return_type(ctx, ctx->func_decl);
                fprintf(out, "    %s __ret_val = ", rt);
                bool is_ref_return = ret_type && (ret_type->is_view || ret_type->is_mod);
                bool is_prim_ref_return = is_ref_return && is_primitive_type(get_base_type_name(ret_type));
                if (is_prim_ref_return) {
                    fprintf(out, "("); emit_type_ref_as_c_type(ctx, ret_type, out, false);
                    fprintf(out, "){ .ptr = &"); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, true, true);
                    fprintf(out, " }");
                } else if (is_ref_return) {
                    fprintf(out, "&");
                    emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_UNARY, true, true);
                } else {
                    bool needs_any_wrap = ret_type && (ret_type->is_opt || str_eq_cstr(get_base_type_name(ret_type), "Any"));
                    bool val_is_box = stmt->as.ret_stmt.values->value->kind == AST_EXPR_BOX;
                    if (needs_any_wrap && !val_is_box) { fprintf(out, "rae_any(("); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false); fprintf(out, "))"); }
                    else emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
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
                if (ret_type->is_opt) fprintf(out, "rae_any_none()");
                else emit_auto_init(ctx, ret_type, out);
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
            bool first = true;
            for (const AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next) {
                if (!c->pattern) {
                    // default case
                    if (!first) fprintf(out, " else {\n");
                    else fprintf(out, "  {\n");
                } else {
                    fprintf(out, first ? "  if (" : " else if (");
                    // When the pattern is `none`, the subject is a RaeAny — use the
                    // runtime tag check rather than `==` (RaeAny is a struct).
                    if (c->pattern->kind == AST_EXPR_NONE) {
                        bool saved = ctx->suppress_opt_unbox;
                        ctx->suppress_opt_unbox = true;
                        fprintf(out, "rae_any_is_none(");
                        emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
                        fprintf(out, ")");
                        ctx->suppress_opt_unbox = saved;
                    } else {
                        emit_expr(ctx, subject, out, PREC_LOWEST, false, false);
                        fprintf(out, " == ");
                        emit_expr(ctx, c->pattern, out, PREC_LOWEST, false, false);
                    }
                    fprintf(out, ") {\n");
                }
                first = false;
                if (c->block) {
                    for (AstStmt* s = c->block->first; s; s = s->next)
                        emit_stmt(ctx, s, out);
                }
                fprintf(out, "  }");
            }
            fprintf(out, "\n");
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

