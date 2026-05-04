// c_call.c — Function call emission for the C backend.
//
// Handles both regular calls (`fn(args)`) and the lowered method-call form
// (`obj.method(args)` is rewritten to `method(this: obj, args)` in c_expr.c
// before reaching `emit_call_expr`). Also owns `emit_opt_unbox_suffix`, which
// inserts `.as.i` / `.as.s` / etc. on calls returning `opt T`.

#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "sema.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Emit unbox suffix for opt T return types: .as.s, .as.i, .as.f, .as.b
void emit_opt_unbox_suffix(CFuncContext* ctx, const AstFuncDecl* fd, const AstTypeRef* call_concrete, FILE* out) {
    if (!fd->returns || !fd->returns->type || !fd->returns->type->is_opt) return;

    // c_return_type always emits "RaeAny" for opt T (even on specialised clones),
    // so unboxing is needed regardless of whether the call resolves to a template
    // or a sema-generated specialisation.
    if (fd->is_extern) return;

    // Only unbox when the call's result is being consumed as a concrete primitive type
    // (e.g. `let s: String = get(...)`). When the consumer expects RaeAny (log args,
    // interpolation, none comparisons), keep the RaeAny so the runtime can format it.
    if (!ctx->has_expected_type) return;
    Str expected_base = get_base_type_name(&ctx->expected_type);
    bool expected_concrete = str_eq_cstr(expected_base, "Int") || str_eq_cstr(expected_base, "Int64") ||
        str_eq_cstr(expected_base, "Float") || str_eq_cstr(expected_base, "Float64") ||
        str_eq_cstr(expected_base, "Bool") || str_eq_cstr(expected_base, "String") ||
        str_eq_cstr(expected_base, "Char") || str_eq_cstr(expected_base, "Char32");
    if (!expected_concrete) return;

    AstTypeRef* ret_tr = fd->returns->type;
    // Pick the substitution context: prefer the call site's concrete args (when fd is
    // a generic template), then the spec's own args, finally the surrounding generic ctx.
    const AstIdentifierPart* gp = NULL;
    const AstTypeRef* ga = NULL;
    if (fd->generic_params && call_concrete) { gp = fd->generic_params; ga = call_concrete; }
    else if (fd->specialization_args && fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC) {
        gp = fd->generic_template->as.func_decl.generic_params;
        ga = fd->specialization_args;
    } else { gp = ctx->generic_params; ga = ctx->generic_args; }
    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, gp, ga, ret_tr);
    Str base = get_base_type_name(sub);
    if (str_eq_cstr(base, "Int64") || str_eq_cstr(base, "Int") || str_eq_cstr(base, "Char") || str_eq_cstr(base, "Char32")) fprintf(out, ".as.i");
    else if (str_eq_cstr(base, "Float64") || str_eq_cstr(base, "Float")) fprintf(out, ".as.f");
    else if (str_eq_cstr(base, "Bool")) fprintf(out, ".as.b");
    else if (str_eq_cstr(base, "String")) fprintf(out, ".as.s");
    // For other types (structs, Any), no unbox needed — RaeAny is the right type
}

bool emit_call_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
    Str name = {0};
    if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
        name = expr->as.call.callee->as.ident;
    } else if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
        name = expr->decl_link->as.func_decl.name;
        if (expr->decl_link->as.func_decl.generic_template && expr->decl_link->as.func_decl.generic_template->kind == AST_DECL_FUNC) {
            name = expr->decl_link->as.func_decl.generic_template->as.func_decl.name;
        }
    }

    // -- INTRINSICS / SPECIAL CASES --
    if (str_eq_cstr(name, "sizeof")) {
        const AstTypeRef* tr = expr->as.call.generic_args;
        if (!tr && expr->as.call.args) tr = infer_expr_type_ref(ctx, expr->as.call.args->value);
        if (tr) { fprintf(out, "sizeof("); emit_type_ref_as_c_type(ctx, tr, out, false); fprintf(out, ")"); }
        else fprintf(out, "sizeof(RaeAny)");
        return true;
    }

    // Buffer Operations (Intrinsics)
    bool is_buf_get = str_eq_cstr(name, "rae_ext___buf_get") || str_eq_cstr(name, "__buf_get") || str_eq_cstr(name, "rae_ext_rae_buf_get");
    bool is_buf_set = str_eq_cstr(name, "rae_ext___buf_set") || str_eq_cstr(name, "__buf_set") || str_eq_cstr(name, "rae_ext_rae_buf_set");
    bool is_buf_copy = str_eq_cstr(name, "rae_ext___buf_copy") || str_eq_cstr(name, "__buf_copy") || str_eq_cstr(name, "rae_ext_rae_buf_copy");

    // Helpers: emit the buffer element type (prefer AstTypeRef if available, fall back to TypeInfo).
    #define EMIT_ELEM_TYPE() do { \
        if (elem_tr) emit_type_ref_as_c_type(ctx, elem_tr, out, false); \
        else emit_type_info_as_c_type(ctx, elem_t, out); \
    } while (0)
    if (is_buf_get) {
        const AstCallArg* arg = expr->as.call.args;
        if (!arg || !arg->next) return false;
        TypeInfo* elem_t = NULL;
        AstTypeRef* elem_tr = NULL;
        // Primary path: walk the AST type of the buffer arg and substitute through the
        // current generic context. This handles compound element types like
        // `Buffer(StringMapEntry(V))` that sema may leave shallow as `Buffer(V)`.
        {
            const AstTypeRef* buf_tr = infer_expr_type_ref(ctx, arg->value);
            if (buf_tr) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, buf_tr);
                Str base = get_base_type_name(sub);
                if (str_eq_cstr(base, "Buffer") && sub->generic_args) {
                    elem_tr = sub->generic_args;
                }
            }
        }
        // Secondary: use the resolved_type if AST inference failed (works for primitive T).
        if (!elem_tr && arg->value->resolved_type) {
            TypeInfo* bt = arg->value->resolved_type;
            while (bt->kind == TYPE_REF) bt = bt->as.ref.base;
            if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base;
        }
        if (elem_t && elem_t->kind == TYPE_GENERIC_PARAM && ctx->generic_params && ctx->generic_args) {
            const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* concrete = ctx->generic_args;
            while (gp && concrete) {
                if (str_eq(gp->text, elem_t->as.generic_param.param_name)) {
                    elem_t = concrete->resolved_type
                        ? concrete->resolved_type
                        : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)concrete);
                    break;
                }
                gp = gp->next; concrete = concrete->next;
            }
        }
        if (!elem_tr && !elem_t && ctx->generic_args) {
            elem_t = ctx->generic_args->resolved_type
                ? ctx->generic_args->resolved_type
                : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)ctx->generic_args);
        }
        fprintf(out, "(*("); EMIT_ELEM_TYPE();
        fprintf(out, "*)( (char*)("); emit_expr(ctx, arg->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") + ("); emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") * sizeof("); EMIT_ELEM_TYPE(); fprintf(out, ") ))");
        return true;
    }
    if (is_buf_set) {
        const AstCallArg* arg = expr->as.call.args;
        if (!arg || !arg->next || !arg->next->next) return false;
        TypeInfo* elem_t = NULL;
        AstTypeRef* elem_tr = NULL;
        // Primary: AST-based inference (matches buf_get path above).
        {
            const AstTypeRef* buf_tr = infer_expr_type_ref(ctx, arg->value);
            if (buf_tr) {
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, buf_tr);
                Str base = get_base_type_name(sub);
                if (str_eq_cstr(base, "Buffer") && sub->generic_args) {
                    elem_tr = sub->generic_args;
                }
            }
        }
        if (!elem_tr && arg->value->resolved_type) {
            TypeInfo* bt = arg->value->resolved_type;
            while (bt->kind == TYPE_REF) bt = bt->as.ref.base;
            if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base;
        }
        if (elem_t && elem_t->kind == TYPE_GENERIC_PARAM && ctx->generic_params && ctx->generic_args) {
            const AstIdentifierPart* gp = ctx->generic_params; const AstTypeRef* concrete = ctx->generic_args;
            while (gp && concrete) {
                if (str_eq(gp->text, elem_t->as.generic_param.param_name)) {
                    elem_t = concrete->resolved_type
                        ? concrete->resolved_type
                        : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)concrete);
                    break;
                }
                gp = gp->next; concrete = concrete->next;
            }
        }
        if (!elem_tr && !elem_t && ctx->generic_args) {
            elem_t = ctx->generic_args->resolved_type
                ? ctx->generic_args->resolved_type
                : sema_resolve_type(ctx->compiler_ctx, (AstTypeRef*)ctx->generic_args);
        }
        fprintf(out, "(*("); EMIT_ELEM_TYPE();
        fprintf(out, "*)( (char*)("); emit_expr(ctx, arg->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") + ("); emit_expr(ctx, arg->next->value, out, PREC_LOWEST, false, false);
        fprintf(out, ") * sizeof("); EMIT_ELEM_TYPE(); fprintf(out, ") )) = ");
        const AstExpr* val_expr = arg->next->next->value;
        bool target_is_any = (elem_t && elem_t->kind == TYPE_ANY)
            || (elem_t && str_eq_cstr(elem_t->name, "Any"))
            || (elem_tr && str_eq_cstr(get_base_type_name(elem_tr), "Any"));
        if (target_is_any) {
            if (val_expr->kind != AST_EXPR_BOX && val_expr->kind != AST_EXPR_UNBOX) {
                fprintf(out, "rae_any(("); emit_expr(ctx, val_expr, out, PREC_LOWEST, false, false); fprintf(out, "))");
            } else {
                emit_expr(ctx, val_expr, out, PREC_LOWEST, false, false);
            }
        } else {
            // Struct literal needs a compound literal cast: (Type){ .x = ... }
            bool needs_struct_cast = val_expr->kind == AST_EXPR_OBJECT &&
                ((elem_t && elem_t->kind == TYPE_STRUCT) ||
                 (elem_tr && get_base_type_name(elem_tr).len > 0 &&
                  !is_primitive_type(get_base_type_name(elem_tr))));
            if (needs_struct_cast) {
                fprintf(out, "("); EMIT_ELEM_TYPE(); fprintf(out, ")");
            }
            emit_expr(ctx, val_expr, out, PREC_LOWEST, false, false);
        }
        return true;
    }

    if (is_buf_copy) {
        const AstCallArg* src_arg = expr->as.call.args;
        const AstCallArg* src_off_arg = src_arg ? src_arg->next : NULL;
        const AstCallArg* dst_arg = src_off_arg ? src_off_arg->next : NULL;
        const AstCallArg* dst_off_arg = dst_arg ? dst_arg->next : NULL;
        const AstCallArg* len_arg = dst_off_arg ? dst_off_arg->next : NULL;
        const AstCallArg* elem_size_arg = len_arg ? len_arg->next : NULL;
        if (src_arg && src_off_arg && dst_arg && dst_off_arg && len_arg) {
            fprintf(out, "memcpy((char*)("); emit_expr(ctx, dst_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") + ("); emit_expr(ctx, dst_off_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") * ");
            if (elem_size_arg) emit_expr(ctx, elem_size_arg->value, out, PREC_LOWEST, false, false);
            else {
                TypeInfo* elem_t = NULL;
                if (src_arg->value->resolved_type) { TypeInfo* bt = src_arg->value->resolved_type; if (bt->kind == TYPE_REF) bt = bt->as.ref.base; if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base; }
                fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, elem_t, out); fprintf(out, ")");
            }
            fprintf(out, ", (char*)("); emit_expr(ctx, src_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") + ("); emit_expr(ctx, src_off_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") * ");
            if (elem_size_arg) emit_expr(ctx, elem_size_arg->value, out, PREC_LOWEST, false, false);
            else {
                TypeInfo* elem_t = NULL;
                if (src_arg->value->resolved_type) { TypeInfo* bt = src_arg->value->resolved_type; if (bt->kind == TYPE_REF) bt = bt->as.ref.base; if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base; }
                fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, elem_t, out); fprintf(out, ")");
            }
            fprintf(out, ", ("); emit_expr(ctx, len_arg->value, out, PREC_LOWEST, false, false);
            fprintf(out, ") * ");
            if (elem_size_arg) emit_expr(ctx, elem_size_arg->value, out, PREC_LOWEST, false, false);
            else {
                TypeInfo* elem_t = NULL;
                if (src_arg->value->resolved_type) { TypeInfo* bt = src_arg->value->resolved_type; if (bt->kind == TYPE_REF) bt = bt->as.ref.base; if (bt->kind == TYPE_BUFFER) elem_t = bt->as.buffer.base; }
                fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, elem_t, out); fprintf(out, ")");
            }
            fprintf(out, ")"); return true;
        }
    }

    // -- REGULAR FUNCTION RESOLUTION --
    const AstFuncDecl* fd = NULL;
    if (expr->decl_link && expr->decl_link->kind == AST_DECL_FUNC) {
        fd = &expr->decl_link->as.func_decl;
    } else if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str callee_name = expr->as.call.callee->as.ident;
        uint16_t call_arg_count = 0;
        for (const AstCallArg* ca = expr->as.call.args; ca; ca = ca->next) call_arg_count++;
        // Treat the call as a method-style invocation whenever there is a first arg
        // (the call may have been written as `set(this, ...)` with `this` as a positional
        // ident, so we cannot rely on `args->name == "this"`).
        Str receiver_base = {0};
        if (expr->as.call.args) {
            const AstTypeRef* recv_tr = infer_expr_type_ref(ctx, expr->as.call.args->value);
            if (recv_tr && ctx->generic_params && ctx->generic_args)
                recv_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, recv_tr);
            if (recv_tr) receiver_base = get_base_type_name(recv_tr);
        }
        const AstFuncDecl* generic_fallback = NULL;
        const AstFuncDecl* receiver_match = NULL;
        const AstFuncDecl* nongeneric_match = NULL;
        for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
            const AstDecl* d = ctx->compiler_ctx->all_decls[i];
            if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, callee_name)) {
                uint16_t param_count = 0; for (const AstParam* pp = d->as.func_decl.params; pp; pp = pp->next) param_count++;
                if (param_count != call_arg_count) continue;
                // Skip specialization clones (specialization_args set, generic_params cleared)
                // — they would force their own concrete args regardless of context.
                // Prefer the generic template so we can re-infer from the call site.
                if (d->as.func_decl.specialization_args) continue;
                if (!d->as.func_decl.generic_params) { nongeneric_match = &d->as.func_decl; continue; }
                if (d->as.func_decl.params && str_eq_cstr(d->as.func_decl.params->name, "this") && receiver_base.len > 0) {
                    Str param_base = get_base_type_name(d->as.func_decl.params->type);
                    if (str_eq(param_base, receiver_base)) { receiver_match = &d->as.func_decl; continue; }
                }
                if (!generic_fallback) generic_fallback = &d->as.func_decl;
            }
        }
        if (!fd) fd = nongeneric_match ? nongeneric_match : (receiver_match ? receiver_match : generic_fallback);
    }

    if (fd) {
        if ((str_eq_cstr(fd->name, "log") || str_eq_cstr(fd->name, "logS")) && expr->as.call.args) {
            const AstExpr* arg_val = expr->as.call.args->value; if (arg_val->kind == AST_EXPR_BOX) arg_val = arg_val->as.unary.operand;
            const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, arg_val);
            Str arg_base = get_base_type_name(arg_tr);
            if (arg_tr && str_eq_cstr(arg_base, "List") && !arg_tr->is_view && !arg_tr->is_mod) {
                bool is_log = str_eq_cstr(fd->name, "log");
                // Pick element-kind tag for the typed runtime helper. Lists are
                // monomorphised so the buffer holds concrete elements; we cannot
                // cast `data` to `RaeAny*`.
                Str elem_base = {0};
                const AstTypeRef* elem_tr = arg_tr->generic_args;
                if (elem_tr) {
                    AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, elem_tr);
                    elem_base = get_base_type_name(sub);
                }
                int elem_kind = 0; // RAE_LIST_ELEM_ANY
                if (str_eq_cstr(elem_base, "Int") || str_eq_cstr(elem_base, "Int64")) elem_kind = 1;
                else if (str_eq_cstr(elem_base, "Float") || str_eq_cstr(elem_base, "Float64")) elem_kind = 2;
                else if (str_eq_cstr(elem_base, "Bool")) elem_kind = 3;
                else if (str_eq_cstr(elem_base, "Char") || str_eq_cstr(elem_base, "Char32")) elem_kind = 4;
                else if (str_eq_cstr(elem_base, "String")) elem_kind = 5;
                fprintf(out, "rae_ext_rae_%s_list_typed((void*)(", is_log ? "log" : "log_stream");
                emit_expr(ctx, arg_val, out, PREC_LOWEST, false, false);
                fprintf(out, ").data, ("); emit_expr(ctx, arg_val, out, PREC_LOWEST, false, false);
                fprintf(out, ").length, ("); emit_expr(ctx, arg_val, out, PREC_LOWEST, false, false);
                fprintf(out, ").capacity, %d)", elem_kind); return true;
            }
        }

        const char* call_name = NULL; AstTypeRef* concrete = NULL;
        // If sema linked the call to a spec clone but the call site has a clear
        // expected return type, prefer to re-infer from that. This catches cases
        // like `g.grid = createList(initialCap: 200)` where `grid: List(Int)` should
        // override any earlier `createList<String>` specialisation sema may have
        // attached.
        if (fd->specialization_args && fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC &&
            ctx->has_expected_type && !expr->as.call.generic_args) {
            const AstFuncDecl* tmpl = &fd->generic_template->as.func_decl;
            if (tmpl->returns && tmpl->returns->type) {
                AstTypeRef* re_inferred = infer_generic_args(ctx->compiler_ctx, tmpl, tmpl->returns->type, &ctx->expected_type);
                if (re_inferred) { concrete = re_inferred; fd = tmpl; }
            }
        }
        if (concrete) {
            // already resolved above
        } else if (expr->as.call.generic_args) {
            concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, expr->as.call.generic_args);
        } else if (fd->generic_params && ctx->generic_params && ctx->generic_args) {
            AstTypeRef* head = NULL; AstTypeRef* tail = NULL;
            for (const AstIdentifierPart* gp = fd->generic_params; gp; gp = gp->next) {
                AstTypeRef tmp = {0}; AstIdentifierPart part = {0}; part.text = gp->text; tmp.parts = &part;
                AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &tmp);
                AstTypeRef* node = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef)); *node = *sub; node->next = NULL;
                if (!head) head = node; else tail->next = node; tail = node;
            }
            concrete = head;
        } else if (fd->generic_params) {
            // Try inference from each param/arg pair (not just `this`) — this handles
            // top-level calls like `setValue(b, val: 100)` where the first param is
            // not named "this" but its type still binds the generic arg.
            const AstParam* p = fd->params;
            const AstCallArg* a = expr->as.call.args;
            while (!concrete && p && a) {
                const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                if (arg_tr && p->type) {
                    if (ctx->generic_params && ctx->generic_args)
                        arg_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, arg_tr);
                    concrete = infer_generic_args(ctx->compiler_ctx, fd, p->type, arg_tr);
                }
                p = p->next; a = a->next;
            }
            if (!concrete && ctx->has_expected_type && fd->returns && fd->returns->type) {
                concrete = infer_generic_args(ctx->compiler_ctx, fd, fd->returns->type, &ctx->expected_type);
            }
        }
        if (concrete) {
            const AstFuncDecl* gen_fd = fd->generic_template ? &fd->generic_template->as.func_decl : fd;
            call_name = rae_mangle_specialized_function(ctx->compiler_ctx, gen_fd, concrete);
            register_function_specialization(ctx->compiler_ctx, gen_fd, concrete);
        } else if (fd->specialization_args) {
            call_name = rae_mangle_specialized_function(ctx->compiler_ctx, fd, fd->specialization_args);
        } else {
            call_name = rae_mangle_function(ctx->compiler_ctx, fd);
        }

        fprintf(out, "%s(", call_name);
        const AstCallArg* a = expr->as.call.args; const AstParam* p = fd->params;
        while (a) {
            bool needs_addr = false; bool needs_deref = false; bool needs_prim_wrap = false; bool needs_box = false;
            if (p && p->type && (p->type->is_view || p->type->is_mod)) {
                const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                if (!(arg_tr && (arg_tr->is_view || arg_tr->is_mod))) {
                    Str base = get_base_type_name(p->type);
                    if (is_primitive_type(base) && !str_eq_cstr(base, "Buffer") && !str_eq_cstr(base, "Any")) needs_prim_wrap = true;
                    else if (!str_eq_cstr(base, "Buffer") && !str_eq_cstr(base, "Any")) needs_addr = true;
                }
            }
            if (p && p->type && fd->is_extern && !(p->type->is_view || p->type->is_mod)) {
                Str pbase = get_base_type_name(p->type);
                if (!is_primitive_type(pbase) && pbase.len > 0) {
                    const AstTypeRef* arg_tr = infer_expr_type_ref(ctx, a->value);
                    if ((arg_tr && (arg_tr->is_view || arg_tr->is_mod)) || (a->value->kind == AST_EXPR_IDENT && is_pointer_type(ctx, a->value->as.ident))) needs_deref = true;
                }
            }
            if (p && p->type && a->value->kind != AST_EXPR_BOX) {
                Str pbase_check = get_base_type_name(p->type);
                bool param_is_any = str_eq_cstr(pbase_check, "Any");
                // For generic param T that resolves to Any (e.g. List(Any) → T=Any),
                // also detect via the call's `concrete` substitution.
                if (!param_is_any && fd->generic_params && concrete) {
                    const AstIdentifierPart* gp = fd->generic_params; const AstTypeRef* ga = concrete;
                    while (gp && ga) {
                        if (str_eq(gp->text, pbase_check)) {
                            Str ga_base = get_base_type_name(ga);
                            if (str_eq_cstr(ga_base, "Any")) param_is_any = true;
                            break;
                        }
                        gp = gp->next; ga = ga->next;
                    }
                }
                if (param_is_any) needs_box = true;
            }

            if (needs_prim_wrap) {
                fprintf(out, "("); emit_type_ref_as_c_type(ctx, p->type, out, false);
                fprintf(out, "){ .ptr = ("); emit_type_ref_as_c_type(ctx, p->type, out, true); fprintf(out, "[]){");
            }
            bool had_exp = ctx->has_expected_type; AstTypeRef saved_exp = ctx->expected_type;
            if (p && p->type) {
                // Substitute generic params in the param type so opt-unbox can detect
                // concrete primitive types when the callee is a generic template.
                AstTypeRef* p_substituted = p->type;
                if (fd->generic_params && concrete) {
                    p_substituted = substitute_type_ref(ctx->compiler_ctx, fd->generic_params, concrete, p->type);
                } else if (fd->specialization_args && fd->generic_template && fd->generic_template->kind == AST_DECL_FUNC) {
                    p_substituted = substitute_type_ref(ctx->compiler_ctx,
                        fd->generic_template->as.func_decl.generic_params,
                        fd->specialization_args, p->type);
                }
                ctx->expected_type = *p_substituted; ctx->has_expected_type = true;
            }
            if (needs_addr) fprintf(out, "&");
            if (needs_deref) fprintf(out, "(*");
            if (needs_box) {
                const AstTypeRef* arg_tr2 = infer_expr_type_ref(ctx, a->value);
                bool is_prim_ref = arg_tr2 && (arg_tr2->is_view || arg_tr2->is_mod) && is_primitive_type(get_base_type_name(arg_tr2));
                fprintf(out, "rae_any(("); emit_expr(ctx, a->value, out, PREC_LOWEST, false, is_prim_ref); fprintf(out, "))");
            } else emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
            if (needs_deref) fprintf(out, ")");
            if (needs_prim_wrap) fprintf(out, "} }");
            ctx->has_expected_type = had_exp; ctx->expected_type = saved_exp;
            if (a->next) fprintf(out, ", ");
            a = a->next; if (p) p = p->next;
        }
        fprintf(out, ")");
        if (!fd->is_extern && !ctx->suppress_opt_unbox) emit_opt_unbox_suffix(ctx, fd, concrete, out);
        return true;
    }

    if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
        Str callee_name = expr->as.call.callee->as.ident;
        if (str_starts_with_cstr(callee_name, "__buf_")) fprintf(out, "rae_ext_%.*s(", (int)callee_name.len, callee_name.data);
        else fprintf(out, "rae_%.*s(", (int)callee_name.len, callee_name.data);
        const AstCallArg* a = expr->as.call.args;
        while (a) { emit_expr(ctx, a->value, out, PREC_LOWEST, false, false); if (a->next) fprintf(out, ", "); a = a->next; }
        fprintf(out, ")"); return true;
    }
    return false;
}
