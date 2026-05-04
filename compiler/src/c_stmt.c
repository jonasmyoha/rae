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

static bool emit_if(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    fprintf(out, "  if (");
    emit_expr(ctx, stmt->as.if_stmt.condition, out, PREC_LOWEST, false, false);
    fprintf(out, ") {\n");
    if (stmt->as.if_stmt.then_block) {
        for (const AstStmt* s = stmt->as.if_stmt.then_block->first; s; s = s->next) emit_stmt(ctx, s, out);
    }
    fprintf(out, "  }");
    if (stmt->as.if_stmt.else_block) {
        fprintf(out, " else {\n");
        for (const AstStmt* s = stmt->as.if_stmt.else_block->first; s; s = s->next) emit_stmt(ctx, s, out);
        fprintf(out, "  }\n");
    } else {
        fprintf(out, "\n");
    }
    return true;
}

static bool emit_loop(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
    fprintf(out, "  for (");
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
        case AST_STMT_EXPR: emit_expr(ctx, stmt->as.expr_stmt, out, PREC_LOWEST, false, false); fprintf(out, ";\n"); break;
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
                    }
                    if (value_returns_ref) {
                        emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
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
                emit_expr(ctx, stmt->as.let_stmt.value, out, PREC_LOWEST, false, false);
                if (needs_box) fprintf(out, "))");
                ctx->has_expected_type = false;
            } else {
                // Auto-init: let x: Type (no initializer)
                emit_auto_init(ctx, stmt->as.let_stmt.type, out);
            }
            fprintf(out, ";\n");
            const char* tn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, stmt->as.let_stmt.type);
            if (ctx->local_count < 256) { ctx->locals[ctx->local_count] = stmt->as.let_stmt.name; ctx->local_types[ctx->local_count] = str_from_cstr(tn); ctx->local_type_refs[ctx->local_count] = stmt->as.let_stmt.type; ctx->local_count++; }
            break;
        }
        case AST_STMT_ASSIGN: {
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
                emit_expr(ctx, stmt->as.assign_stmt.target, out, PREC_LOWEST, true, false);
                fprintf(out, " = ");
                // Propagate target type as expected so opt T returns get unboxed
                // (e.g. `total = total + l.get(...)` where total: Int).
                bool had_exp = ctx->has_expected_type;
                AstTypeRef saved_exp = ctx->expected_type;
                if (target_tr) { ctx->expected_type = *target_tr; ctx->has_expected_type = true; }
                // For struct literal assignments, add compound literal cast if missing
                if (stmt->as.assign_stmt.value->kind == AST_EXPR_OBJECT &&
                    !stmt->as.assign_stmt.value->as.object_literal.type && target_tr) {
                    fprintf(out, "(");
                    emit_type_ref_as_c_type(ctx, target_tr, out, true);
                    fprintf(out, ")");
                }
                emit_expr(ctx, stmt->as.assign_stmt.value, out, PREC_LOWEST, false, false);
                ctx->has_expected_type = had_exp;
                ctx->expected_type = saved_exp;
            }
            fprintf(out, ";\n");
            break;
        }
        case AST_STMT_RET: {
            if (ctx->defer_stack.count > 0) {
                // Has defers — emit them before returning
                if (stmt->as.ret_stmt.values) {
                    // Store return value in temp, emit defers, then return temp
                    const char* rt = c_return_type(ctx, ctx->func_decl);
                    fprintf(out, "  %s __ret_val = ", rt);
                    const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                    bool is_ref_return = ret_type && (ret_type->is_view || ret_type->is_mod);
                    bool is_prim_ref_return = is_ref_return && is_primitive_type(get_base_type_name(ret_type));
                    if (is_prim_ref_return) {
                        fprintf(out, "("); emit_type_ref_as_c_type(ctx, ret_type, out, false);
                        fprintf(out, "){ .ptr = &"); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    } else {
                        bool needs_any_wrap = ret_type && (ret_type->is_opt || str_eq_cstr(get_base_type_name(ret_type), "Any"));
                        bool val_is_box = stmt->as.ret_stmt.values->value->kind == AST_EXPR_BOX;
                        if (needs_any_wrap && !val_is_box) { fprintf(out, "rae_any(("); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false); fprintf(out, "))"); }
                        else emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                    }
                    fprintf(out, ";\n");
                    emit_defers(ctx, 0, out);
                    fprintf(out, "  return __ret_val;\n");
                } else {
                    // Bare return
                    emit_defers(ctx, 0, out);
                    fprintf(out, "  return ");
                    if (ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "main")) fprintf(out, "0");
                    else {
                        const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                        if (ret_type) {
                            if (ret_type->is_opt) fprintf(out, "rae_any_none()");
                            else emit_auto_init(ctx, ret_type, out);
                        }
                    }
                    fprintf(out, ";\n");
                }
            } else {
                // No defers — direct return
                fprintf(out, "  return ");
                if (stmt->as.ret_stmt.values) {
                    const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                    bool is_ref_return = ret_type && (ret_type->is_view || ret_type->is_mod);
                    bool is_prim_ref_return = is_ref_return && is_primitive_type(get_base_type_name(ret_type));
                    if (is_prim_ref_return) {
                        fprintf(out, "("); emit_type_ref_as_c_type(ctx, ret_type, out, false);
                        fprintf(out, "){ .ptr = &"); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, true, true);
                        fprintf(out, " }");
                    } else {
                        bool needs_any_wrap = ret_type && (ret_type->is_opt || str_eq_cstr(get_base_type_name(ret_type), "Any"));
                        bool val_is_box = stmt->as.ret_stmt.values->value->kind == AST_EXPR_BOX;
                        if (needs_any_wrap && !val_is_box) { fprintf(out, "rae_any(("); emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false); fprintf(out, "))"); }
                        else emit_expr(ctx, stmt->as.ret_stmt.values->value, out, PREC_LOWEST, false, false);
                    }
                } else {
                    if (ctx->func_decl && str_eq_cstr(ctx->func_decl->name, "main")) fprintf(out, "0");
                    else {
                        const AstTypeRef* ret_type = ctx->func_decl && ctx->func_decl->returns ? ctx->func_decl->returns->type : NULL;
                        if (ret_type) {
                            if (ret_type->is_opt) fprintf(out, "rae_any_none()");
                            else emit_auto_init(ctx, ret_type, out);
                        }
                    }
                }
                fprintf(out, ";\n");
            }
            break;
        } 
            break;
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

