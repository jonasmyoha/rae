// c_discovery.c — Generic specialisation discovery pass for the C backend.
//
// Walks every reachable function body to find generic call sites and registers
// each (template, concrete-args) pair so the rest of the backend can emit
// forward declarations and bodies in the correct order.

#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "sema.h"
#include "str.h"

#include <stddef.h>

static void discover_specializations_expr_impl(CFuncContext* ctx, const AstExpr* expr);
static void discover_specializations_stmt_impl(CFuncContext* ctx, const AstStmt* stmt);

static void discover_specializations_expr_impl(CFuncContext* ctx, const AstExpr* expr) {
    if (!expr) return;
    switch (expr->kind) {
        case AST_EXPR_CALL: {
            const AstExpr* callee = expr->as.call.callee;
            if (callee->kind == AST_EXPR_IDENT) {
                uint16_t param_count = 0; for (const AstCallArg* a = expr->as.call.args; a; a = a->next) param_count++;
                // Receiver-aware overload selection: when the callee has a "this" first
                // param and we have a first arg, pick the overload whose first param's
                // base type matches the receiver's base type. This avoids picking the
                // wrong overload for `set(map, k, v)` where multiple `set` overloads
                // exist (List/StringMap/IntMap). Mirrors the emit_call_expr logic.
                const AstFuncDecl* d = NULL;
                {
                    Str receiver_base = {0};
                    if (expr->as.call.args) {
                        const AstTypeRef* recv_tr = infer_expr_type_ref(ctx, expr->as.call.args->value);
                        if (recv_tr && ctx->generic_params && ctx->generic_args) {
                            recv_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, recv_tr);
                        }
                        if (recv_tr) receiver_base = get_base_type_name(recv_tr);
                    }
                    const AstFuncDecl* generic_fallback = NULL;
                    for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count && !d; i++) {
                        const AstDecl* dd = ctx->compiler_ctx->all_decls[i];
                        if (dd->kind != AST_DECL_FUNC || !str_eq(dd->as.func_decl.name, callee->as.ident)) continue;
                        uint16_t pc = 0; for (const AstParam* pp = dd->as.func_decl.params; pp; pp = pp->next) pc++;
                        if (pc != param_count) continue;
                        // Skip specialization clones — they would force their own concrete
                        // args; we want the generic template so we can re-infer per call.
                        if (dd->as.func_decl.specialization_args) continue;
                        // Prefer matching receiver base when first param is "this".
                        if (dd->as.func_decl.params && str_eq_cstr(dd->as.func_decl.params->name, "this") && receiver_base.len > 0) {
                            Str param_base = get_base_type_name(dd->as.func_decl.params->type);
                            if (str_eq(param_base, receiver_base)) { d = &dd->as.func_decl; break; }
                        }
                        if (!generic_fallback) generic_fallback = &dd->as.func_decl;
                    }
                    if (!d) d = generic_fallback;
                }
                if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, false, expr);
                if (!d) d = find_function_overload(ctx->module, ctx, callee->as.ident, NULL, param_count, true, expr);
                if ((str_eq_cstr(callee->as.ident, "sizeof") || str_eq_cstr(callee->as.ident, "__buf_alloc") || str_eq_cstr(callee->as.ident, "__buf_free") || str_eq_cstr(callee->as.ident, "__buf_copy")) && expr->as.call.generic_args) {
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args); register_generic_type(ctx->compiler_ctx, sub);
                }
                if (d && d->generic_params) {
                    AstTypeRef* inferred_args = NULL;
                    if (expr->as.call.generic_args) {
                        AstTypeRef* concrete_args = NULL; AstTypeRef* last_arg = NULL;
                        for (const AstTypeRef* arg = expr->as.call.generic_args; arg; arg = arg->next) {
                            AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg);
                            if (!concrete_args) concrete_args = sub; else last_arg->next = sub; last_arg = sub;
                        }
                        inferred_args = concrete_args;
                    } else if (d->params) {
                        // Try inference from each param/arg pair (not just `this`).
                        const AstParam* p = d->params;
                        const AstCallArg* a = expr->as.call.args;
                        while (!inferred_args && p && a) {
                            const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                            if (arg_tr) {
                                if (ctx->generic_params && ctx->generic_args)
                                    arg_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg_tr);
                                AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, p->type, arg_tr);
                                if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                            }
                            p = p->next; a = a->next;
                        }
                    }
                    // Try return-type inference from expected type (let x: Type = genericFunc(...))
                    if (!inferred_args && ctx->has_expected_type && d->returns && d->returns->type) {
                        AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->returns->type, &ctx->expected_type);
                        if (inferred) inferred_args = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred);
                    }
                    if (inferred_args) register_function_specialization(ctx->compiler_ctx, d, inferred_args);
                }
            }
            for (const AstCallArg* a = expr->as.call.args; a; a = a->next) discover_specializations_expr_impl(ctx, a->value);
            break;
        }
        case AST_EXPR_METHOD_CALL: {
            uint16_t param_count = 1; for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) param_count++;
            Str obj_type = infer_expr_type(ctx, expr->as.method_call.object);
            const AstFuncDecl* d = find_function_overload(ctx->module, ctx, expr->as.method_call.method_name, &obj_type, param_count, true, expr);
            if (d && d->generic_params) {
                const AstTypeRef* receiver_type = infer_expr_type_ref(ctx, expr->as.method_call.object);
                // When discovering inside a generic body, `this`'s type is the template
                // (e.g. `view List(T)` with literal T). Substitute through the current
                // generic context so inference can bind T to the concrete arg.
                if (receiver_type && ctx->generic_params && ctx->generic_args)
                    receiver_type = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, receiver_type);
                AstTypeRef* inferred = infer_generic_args(ctx->compiler_ctx, d, d->params->type, receiver_type);
                if (inferred) { AstTypeRef* concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, inferred); register_function_specialization(ctx->compiler_ctx, d, concrete); }
            }
            discover_specializations_expr_impl(ctx, expr->as.method_call.object); for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) discover_specializations_expr_impl(ctx, a->value);
            break;
        }
        case AST_EXPR_BINARY: discover_specializations_expr_impl(ctx, expr->as.binary.lhs); discover_specializations_expr_impl(ctx, expr->as.binary.rhs); break;
        case AST_EXPR_UNARY: discover_specializations_expr_impl(ctx, expr->as.unary.operand); break;
        case AST_EXPR_MEMBER: discover_specializations_expr_impl(ctx, expr->as.member.object); break;
        case AST_EXPR_INDEX: discover_specializations_expr_impl(ctx, expr->as.index.target); discover_specializations_expr_impl(ctx, expr->as.index.index); break;
        case AST_EXPR_OBJECT: {
            // Propagate per-field expected types so generic calls inside a struct
            // literal infer from the field's declared type
            // (e.g. `Game { grid: createList(initialCap: 200) }` where
            // `grid: List(Int)`).
            const AstTypeRef* obj_tr = expr->as.object_literal.type;
            if (!obj_tr && ctx->has_expected_type) obj_tr = &ctx->expected_type;
            const AstDecl* struct_decl = NULL;
            if (obj_tr) {
                Str obj_base = get_base_type_name(obj_tr);
                struct_decl = find_type_decl(ctx, ctx->module, obj_base);
            }
            bool saved_has_exp = ctx->has_expected_type;
            AstTypeRef saved_exp = ctx->expected_type;
            for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
                const AstTypeRef* field_tr = NULL;
                if (struct_decl && struct_decl->kind == AST_DECL_TYPE) {
                    for (const AstTypeField* td = struct_decl->as.type_decl.fields; td; td = td->next) {
                        if (str_eq(td->name, f->name)) { field_tr = td->type; break; }
                    }
                }
                if (field_tr) {
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx,
                        struct_decl->as.type_decl.generic_params,
                        obj_tr ? obj_tr->generic_args : NULL, field_tr);
                    ctx->expected_type = *sub;
                    ctx->has_expected_type = true;
                } else {
                    ctx->has_expected_type = false;
                }
                discover_specializations_expr_impl(ctx, f->value);
            }
            ctx->has_expected_type = saved_has_exp;
            ctx->expected_type = saved_exp;
            break;
        }
        case AST_EXPR_INTERP: {
            for (const AstInterpPart* p = expr->as.interp.parts; p; p = p->next) discover_specializations_expr_impl(ctx, p->value);
            break;
        }
        case AST_EXPR_BOX:
        case AST_EXPR_UNBOX:
            // Sema wraps args being coerced to/from RaeAny; the inner expression may
            // contain method calls whose specialisations must still be discovered.
            discover_specializations_expr_impl(ctx, expr->as.unary.operand);
            break;
        case AST_EXPR_COLLECTION_LITERAL: {
            for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) discover_specializations_expr_impl(ctx, e->value);
            // Register createList and add specializations from expected type
            if (ctx->has_expected_type && ctx->expected_type.generic_args) {
                const AstTypeRef* elem_type = ctx->expected_type.generic_args;
                const AstFuncDecl* create_fd = NULL;
                const AstFuncDecl* add_fd = NULL;
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind != AST_DECL_FUNC) continue;
                    if (str_eq_cstr(d->as.func_decl.name, "createList") && d->as.func_decl.generic_params) create_fd = &d->as.func_decl;
                    if (str_eq_cstr(d->as.func_decl.name, "add") && d->as.func_decl.generic_params) add_fd = &d->as.func_decl;
                }
                if (create_fd) register_function_specialization(ctx->compiler_ctx, create_fd, elem_type);
                if (add_fd) register_function_specialization(ctx->compiler_ctx, add_fd, elem_type);
                register_generic_type(ctx->compiler_ctx, &ctx->expected_type);
            }
            break;
        }
        default: break;
    }
}

static void discover_specializations_stmt_impl(CFuncContext* ctx, const AstStmt* stmt) {
    for (const AstStmt* s = stmt; s; s = s->next) {
        switch (s->kind) {
            case AST_STMT_LET:
                if (s->as.let_stmt.value) {
                    const AstTypeRef* type = s->as.let_stmt.type ? s->as.let_stmt.type : infer_expr_type_ref(ctx, s->as.let_stmt.value);
                    if (type) { ctx->expected_type = *type; ctx->has_expected_type = true; }
                    discover_specializations_expr_impl(ctx, s->as.let_stmt.value); ctx->has_expected_type = false;
                    if (ctx->local_count < 256) {
                        ctx->locals[ctx->local_count] = s->as.let_stmt.name; ctx->local_type_refs[ctx->local_count] = type;
                        const char* mn = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, type);
                        ctx->local_types[ctx->local_count] = str_from_cstr(mn); ctx->local_count++;
                    }
                }
                break;
            case AST_STMT_ASSIGN:
                discover_specializations_expr_impl(ctx, s->as.assign_stmt.target);
                {
                    const AstTypeRef* tr = infer_expr_type_ref(ctx, s->as.assign_stmt.target);
                    if (tr) { ctx->expected_type = *tr; ctx->has_expected_type = true; }
                    discover_specializations_expr_impl(ctx, s->as.assign_stmt.value); ctx->has_expected_type = false;
                }
                break;
            case AST_STMT_EXPR: discover_specializations_expr_impl(ctx, s->as.expr_stmt); break;
            case AST_STMT_IF: discover_specializations_expr_impl(ctx, s->as.if_stmt.condition); if (s->as.if_stmt.then_block) discover_specializations_stmt_impl(ctx, s->as.if_stmt.then_block->first); if (s->as.if_stmt.else_block) discover_specializations_stmt_impl(ctx, s->as.if_stmt.else_block->first); break;
            case AST_STMT_LOOP: if (s->as.loop_stmt.init) discover_specializations_stmt_impl(ctx, s->as.loop_stmt.init); if (s->as.loop_stmt.condition) discover_specializations_expr_impl(ctx, s->as.loop_stmt.condition); if (s->as.loop_stmt.increment) discover_specializations_expr_impl(ctx, s->as.loop_stmt.increment); if (s->as.loop_stmt.body) discover_specializations_stmt_impl(ctx, s->as.loop_stmt.body->first); break;
            case AST_STMT_RET: if (s->as.ret_stmt.values && s->as.ret_stmt.values->value) discover_specializations_expr_impl(ctx, s->as.ret_stmt.values->value); break;
            case AST_STMT_MATCH: {
                if (s->as.match_stmt.subject) discover_specializations_expr_impl(ctx, s->as.match_stmt.subject);
                for (const AstMatchCase* mc = s->as.match_stmt.cases; mc; mc = mc->next) {
                    if (mc->pattern) discover_specializations_expr_impl(ctx, mc->pattern);
                    if (mc->block) discover_specializations_stmt_impl(ctx, mc->block->first);
                }
                break;
            }
            default: break;
        }
    }
}

void discover_specializations_expr(CFuncContext* ctx, const AstExpr* expr) {
    discover_specializations_expr_impl(ctx, expr);
}

void discover_specializations_stmt(CFuncContext* ctx, const AstStmt* stmt) {
    discover_specializations_stmt_impl(ctx, stmt);
}

void discover_specializations_module(CompilerContext* ctx, const AstModule* module) {
    for (size_t i = 0; i < ctx->all_decl_count; i++) {
        const AstDecl* d = ctx->all_decls[i];
        if (d->kind == AST_DECL_FUNC && !d->as.func_decl.generic_params && !d->as.func_decl.specialization_args) {
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = &d->as.func_decl};
            // Pre-populate params as locals so infer_expr_type_ref can resolve `this`
            // and other parameters when walking the body.
            for (const AstParam* p = d->as.func_decl.params; p; p = p->next) {
                if (fctx.local_count < 256) {
                    fctx.locals[fctx.local_count] = p->name;
                    fctx.local_type_refs[fctx.local_count] = p->type;
                    const char* tn = rae_mangle_type_specialized(ctx, NULL, NULL, p->type);
                    fctx.local_types[fctx.local_count] = str_from_cstr(tn);
                    fctx.local_count++;
                }
            }
            if (d->as.func_decl.body) discover_specializations_stmt_impl(&fctx, d->as.func_decl.body->first);
        }
    }
    size_t discovered = 0;
    while (discovered < ctx->specialized_func_count) {
        size_t limit = ctx->specialized_func_count;
        for (size_t i = discovered; i < limit; i++) {
            const AstFuncDecl* f = ctx->specialized_funcs[i].decl; const AstTypeRef* args = ctx->specialized_funcs[i].concrete_args;
            if (!f) continue;
            const AstIdentifierPart* disc_gp = f->generic_params;
            if (!disc_gp && f->generic_template && f->generic_template->kind == AST_DECL_FUNC) disc_gp = f->generic_template->as.func_decl.generic_params;
            CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = f, .generic_params = disc_gp, .generic_args = args};
            // Populate locals from params so infer_expr_type_ref works for 'this' etc.
            for (const AstParam* p = f->params; p; p = p->next) {
                if (fctx.local_count < 256) {
                    fctx.locals[fctx.local_count] = p->name;
                    fctx.local_type_refs[fctx.local_count] = p->type;
                    const char* tn = rae_mangle_type_specialized(ctx, disc_gp, args, p->type);
                    fctx.local_types[fctx.local_count] = str_from_cstr(tn);
                    fctx.local_count++;
                }
            }
            if (f->body) discover_specializations_stmt_impl(&fctx, f->body->first);
        }
        discovered = limit;
    }
}

void collect_type_refs_module(CompilerContext* ctx) {
    size_t last_generic_count = 0; size_t last_func_count = 0;
    do {
        last_generic_count = ctx->generic_type_count; last_func_count = ctx->specialized_func_count;
        discover_specializations_module(ctx, ctx->current_module);
    } while (ctx->generic_type_count > last_generic_count || ctx->specialized_func_count > last_func_count);
}
