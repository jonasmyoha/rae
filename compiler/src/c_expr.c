// c_expr.c — Expression emission for the C backend.
//
// `emit_expr` is the per-AST-node switch that turns Rae expressions into C
// expressions. Method calls are lowered here to plain calls and forwarded to
// `emit_call_expr` (in c_call.c).

#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "sema.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue, bool suppress_deref) {
  if (!expr) return true;
  switch (expr->kind) {
    case AST_EXPR_INTEGER: fprintf(out, "((int64_t)%.*sLL)", (int)expr->as.integer.len, expr->as.integer.data); break;
    case AST_EXPR_FLOAT: fprintf(out, "%.*s", (int)expr->as.floating.len, expr->as.floating.data); break;
    case AST_EXPR_BOOL: fprintf(out, "(bool)%s", expr->as.boolean ? "true" : "false"); break;
    case AST_EXPR_STRING: emit_string_literal(out, expr->as.string_lit); break;
    case AST_EXPR_CHAR: fprintf(out, "(uint32_t)%uU", (uint32_t)expr->as.char_value); break;
    case AST_EXPR_IDENT: {
        const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
        bool is_prim_ref = is_primitive_ref(ctx, tr);
        bool is_ptr = is_pointer_type(ctx, expr->as.ident);
        
        if (is_prim_ref && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s.ptr)", (int)expr->as.ident.len, expr->as.ident.data);
        } else if (is_ptr && !is_lvalue && !suppress_deref) {
            // Check if it's a Buffer or List - they are pointers but shouldn't be dereferenced here 
            // if we are just passing them or accessing members via ->
            Str base = get_base_type_name(tr);
            if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List")) {
                fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
            } else {
                fprintf(out, "(*%.*s)", (int)expr->as.ident.len, expr->as.ident.data);
            }
        } else {
            fprintf(out, "%.*s", (int)expr->as.ident.len, expr->as.ident.data);
        }
        break;
    }
    case AST_EXPR_BINARY: {
      // Special case: string equality — use rae_ext_rae_str_eq instead of ==
      if (expr->as.binary.op == AST_BIN_IS) {
          const AstTypeRef* lhs_tr = infer_expr_type_ref(ctx, expr->as.binary.lhs);
          Str lhs_base = get_base_type_name(lhs_tr);
          bool lhs_is_string = str_eq_cstr(lhs_base, "String") || str_eq_cstr(lhs_base, "rae_String");
          bool rhs_is_string_lit = expr->as.binary.rhs->kind == AST_EXPR_STRING;
          // Also detect toString() calls — they always return String
          bool lhs_is_tostring = expr->as.binary.lhs->kind == AST_EXPR_METHOD_CALL &&
              str_eq_cstr(expr->as.binary.lhs->as.method_call.method_name, "toString");
          if (lhs_is_string || rhs_is_string_lit || lhs_is_tostring) {
              fprintf(out, "(bool)rae_ext_rae_str_eq(");
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              fprintf(out, ")");
              break;
          }
      }
      // `x is none` / `none is x`: keep the opt result as RaeAny so the comparison
      // against rae_any_none() type-checks. Otherwise auto-unbox would yield a
      // primitive on one side and RaeAny on the other.
      bool none_compare = (expr->as.binary.op == AST_BIN_IS) &&
          (expr->as.binary.rhs->kind == AST_EXPR_NONE || expr->as.binary.lhs->kind == AST_EXPR_NONE);
      bool saved_unbox = ctx->suppress_opt_unbox;
      if (none_compare) ctx->suppress_opt_unbox = true;
      // Float modulo: emit fmod(a, b) instead of a % b
      if (expr->as.binary.op == AST_BIN_MOD) {
          bool lhs_float = expr->as.binary.lhs->kind == AST_EXPR_FLOAT;
          bool rhs_float = expr->as.binary.rhs->kind == AST_EXPR_FLOAT;
          if (!lhs_float && !rhs_float) {
              const AstTypeRef* ltr = infer_expr_type_ref(ctx, expr->as.binary.lhs);
              Str lb = get_base_type_name(ltr);
              if (str_eq_cstr(lb, "Float64") || str_eq_cstr(lb, "Float") || str_eq_cstr(lb, "Float32") || str_eq_cstr(lb, "double")) lhs_float = true;
          }
          if (lhs_float || rhs_float) {
              fprintf(out, "fmod(");
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              fprintf(out, ")");
              ctx->suppress_opt_unbox = saved_unbox;
              break;
          }
      }
      // For arithmetic/comparison ops on primitives, propagate the side that has a
      // known primitive type as the expected type for both sides. This lets calls
      // returning opt T auto-unbox when used in `g.grid.get(i) > 0` etc.
      bool is_arith_or_cmp = (expr->as.binary.op >= AST_BIN_ADD && expr->as.binary.op <= AST_BIN_GE) ||
                             expr->as.binary.op == AST_BIN_IS;
      bool had_exp_bin = ctx->has_expected_type;
      AstTypeRef saved_exp_bin = ctx->expected_type;
      if (is_arith_or_cmp && !none_compare) {
          const AstTypeRef* lhs_ti = infer_expr_type_ref(ctx, expr->as.binary.lhs);
          const AstTypeRef* rhs_ti = infer_expr_type_ref(ctx, expr->as.binary.rhs);
          const AstTypeRef* picked = NULL;
          // Prefer whichever side has a non-opt primitive type.
          if (lhs_ti && !lhs_ti->is_opt) {
              Str b = get_base_type_name(lhs_ti);
              if (is_primitive_type(b) && !str_eq_cstr(b, "Any")) picked = lhs_ti;
          }
          if (!picked && rhs_ti && !rhs_ti->is_opt) {
              Str b = get_base_type_name(rhs_ti);
              if (is_primitive_type(b) && !str_eq_cstr(b, "Any")) picked = rhs_ti;
          }
          if (picked) { ctx->expected_type = *picked; ctx->has_expected_type = true; }
      }
      int prec = binary_op_precedence(expr->as.binary.op); bool is_bool_op = expr->as.binary.op >= AST_BIN_LT && expr->as.binary.op <= AST_BIN_OR;
      if (is_bool_op) fprintf(out, "(bool)("); if (prec < parent_prec) fprintf(out, "(");
      emit_expr(ctx, expr->as.binary.lhs, out, prec, false, false);
      switch (expr->as.binary.op) {
        case AST_BIN_ADD: fprintf(out, " + "); break; case AST_BIN_SUB: fprintf(out, " - "); break;
        case AST_BIN_MUL: fprintf(out, " * "); break; case AST_BIN_DIV: fprintf(out, " / "); break;
        case AST_BIN_MOD: fprintf(out, " %% "); break; case AST_BIN_LT: fprintf(out, " < "); break;
        case AST_BIN_GT: fprintf(out, " > "); break; case AST_BIN_LE: fprintf(out, " <= "); break;
        case AST_BIN_GE: fprintf(out, " >= "); break; case AST_BIN_IS: fprintf(out, " == "); break;
        case AST_BIN_AND: fprintf(out, " && "); break; case AST_BIN_OR: fprintf(out, " || "); break;
      }
      emit_expr(ctx, expr->as.binary.rhs, out, prec, false, false);
      if (prec < parent_prec) fprintf(out, ")"); if (is_bool_op) fprintf(out, ")");
      ctx->has_expected_type = had_exp_bin;
      ctx->expected_type = saved_exp_bin;
      ctx->suppress_opt_unbox = saved_unbox;
      break;
    }
    case AST_EXPR_UNARY: {
        switch (expr->as.unary.op) {
            case AST_UNARY_NOT: fprintf(out, "((bool)!("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, "))"); break;
            case AST_UNARY_NEG: fprintf(out, "-("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, ")"); break;
            case AST_UNARY_VIEW: case AST_UNARY_MOD: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); break;
            case AST_UNARY_PRE_INC: fprintf(out, "++"); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); break;
            case AST_UNARY_PRE_DEC: fprintf(out, "--"); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); break;
            case AST_UNARY_POST_INC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "++"); break;
            case AST_UNARY_POST_DEC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "--"); break;
            default: break;
        }
        break;
    }
    case AST_EXPR_CALL: emit_call_expr(ctx, expr, out); break;
    case AST_EXPR_METHOD_CALL: {
        // Built-in method: toString() → rae_ext_rae_str(object)
        if (str_eq_cstr(expr->as.method_call.method_name, "toString") && !expr->as.method_call.args) {
            fprintf(out, "rae_ext_rae_str((");
            emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, false);
            fprintf(out, "))");
            break;
        }
        // Built-in method: toJson() → rae_toJson_TYPE_(&object)
        if (str_eq_cstr(expr->as.method_call.method_name, "toJson") && !expr->as.method_call.args) {
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.method_call.object);
            Str obj_base = get_base_type_name(obj_tr);
            const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, obj_tr);
            fprintf(out, "rae_toJson_%s_(&", mangled);
            emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, true, false);
            fprintf(out, ")");
            break;
        }
        // Built-in static method: Type.fromJson(json: str) → rae_fromJson_TYPE_(str)
        if (str_eq_cstr(expr->as.method_call.method_name, "fromJson")) {
            Str type_name = {0};
            if (expr->as.method_call.object->kind == AST_EXPR_IDENT) type_name = expr->as.method_call.object->as.ident;
            if (type_name.len > 0) {
                fprintf(out, "rae_fromJson_rae_%.*s_(", (int)type_name.len, type_name.data);
                if (expr->as.method_call.args) emit_expr(ctx, expr->as.method_call.args->value, out, PREC_LOWEST, false, false);
                fprintf(out, ")");
                break;
            }
        }
        // Module-qualified call: `sys.fn(args)` where `sys` is an imported module
        // name. The c_backend flattens imports into ctx->all_decls and clears
        // module->imports, so detect it by: object is an IDENT, the ident has
        // no local binding and no inferable type, and a function `method_name`
        // exists in the project.
        if (expr->as.method_call.object->kind == AST_EXPR_IDENT) {
            Str obj_name = expr->as.method_call.object->as.ident;
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.method_call.object);
            bool obj_has_value = obj_tr != NULL || is_pointer_type(ctx, obj_name);
            bool fn_exists = false;
            if (!obj_has_value) {
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, expr->as.method_call.method_name)) {
                        fn_exists = true; break;
                    }
                }
            }
            if (!obj_has_value && fn_exists) {
                AstExpr call = { .kind = AST_EXPR_CALL, .line = expr->line, .column = expr->column, .decl_link = expr->decl_link };
                call.as.call.callee = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr));
                call.as.call.callee->kind = AST_EXPR_IDENT;
                call.as.call.callee->as.ident = expr->as.method_call.method_name;
                call.as.call.args = expr->as.method_call.args;
                call.as.call.generic_args = expr->as.method_call.generic_args;
                emit_call_expr(ctx, &call, out); break;
            }
        }
        AstExpr call = { .kind = AST_EXPR_CALL, .line = expr->line, .column = expr->column, .decl_link = expr->decl_link };
        call.as.call.callee = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr)); call.as.call.callee->kind = AST_EXPR_IDENT; call.as.call.callee->as.ident = expr->as.method_call.method_name;
        AstCallArg* first_arg = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstCallArg)); first_arg->name = str_from_cstr("this"); first_arg->value = expr->as.method_call.object; first_arg->next = expr->as.method_call.args;
        call.as.call.args = first_arg; call.as.call.generic_args = expr->as.method_call.generic_args;
        emit_call_expr(ctx, &call, out); break;
    }
    case AST_EXPR_MEMBER: {
        // Check if this is an enum access (e.g. Color.Green)
        if (expr->as.member.object->kind == AST_EXPR_IDENT) {
            const AstDecl* ed = find_enum_decl(ctx, ctx->module, expr->as.member.object->as.ident);
            if (ed) {
                fprintf(out, "%.*s_%.*s", (int)expr->as.member.object->as.ident.len, expr->as.member.object->as.ident.data,
                    (int)expr->as.member.member.len, expr->as.member.member.data);
                break;
            }
        }
        const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.member.object);
        Str obj_base = get_base_type_name(obj_tr);
        bool use_arrow = (obj_tr && (obj_tr->is_view || obj_tr->is_mod));
        emit_expr(ctx, expr->as.member.object, out, PREC_CALL, true, false);
        fprintf(out, "%s%.*s", use_arrow ? "->" : ".", (int)expr->as.member.member.len, expr->as.member.member.data);
        break;
    }
    case AST_EXPR_INDEX: {
        // List(T) is a struct, not a raw array; lower `xs[i]` to a typed
        // buffer access on the data pointer.
        const AstTypeRef* tgt_tr = infer_expr_type_ref(ctx, expr->as.index.target);
        Str tgt_base = get_base_type_name(tgt_tr);
        if (str_eq_cstr(tgt_base, "List")) {
            const AstTypeRef* elem_tr = tgt_tr ? tgt_tr->generic_args : NULL;
            AstTypeRef* sub = elem_tr ? substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, elem_tr) : NULL;
            fprintf(out, "(*(");
            if (sub) emit_type_ref_as_c_type(ctx, sub, out, false); else fprintf(out, "RaeAny");
            fprintf(out, "*)( (char*)((");
            emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false);
            fprintf(out, ").data) + (");
            emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false);
            fprintf(out, ") * sizeof(");
            if (sub) emit_type_ref_as_c_type(ctx, sub, out, false); else fprintf(out, "RaeAny");
            fprintf(out, ") ))");
            break;
        }
        emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false); fprintf(out, "["); emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false); fprintf(out, "]"); break;
    }
    case AST_EXPR_BOX: {
        // For primitive refs, pass wrapper directly so rae_any picks mod/view variant
        const AstTypeRef* box_tr = infer_expr_type_ref(ctx, expr->as.unary.operand);
        bool box_suppress = box_tr && (box_tr->is_view || box_tr->is_mod) && is_primitive_type(get_base_type_name(box_tr));
        fprintf(out, "rae_any(("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, box_suppress); fprintf(out, "))");
        break;
    }
    case AST_EXPR_UNBOX: {
        if (expr->resolved_type) {
            // Check if the operand already returns the concrete type (not RaeAny)
            // This happens when an extern function returns e.g. rae_String directly
            // but sema inserted UNBOX because the Rae decl says ret opt String
            bool skip_unbox = false;
            const AstExpr* inner = expr->as.unary.operand;
            // Skip through nested BOX to find the actual call
            if (inner->kind == AST_EXPR_BOX) inner = inner->as.unary.operand;
            // Check via decl_link
            if (inner->kind == AST_EXPR_CALL && inner->decl_link && inner->decl_link->kind == AST_DECL_FUNC) {
                const AstFuncDecl* ifd = &inner->decl_link->as.func_decl;
                if (ifd->is_extern) skip_unbox = true;
            }
            // Also check: if inner is a call whose callee name starts with rae_ext_ or is an extern
            // (handles fallback path where decl_link is NULL)
            if (!skip_unbox && inner->kind == AST_EXPR_CALL && inner->as.call.callee &&
                inner->as.call.callee->kind == AST_EXPR_IDENT) {
                Str cname = inner->as.call.callee->as.ident;
                if (str_starts_with_cstr(cname, "rae_ext_") || str_starts_with_cstr(cname, "rae_sys_") ||
                    str_starts_with_cstr(cname, "rae_io_") || str_starts_with_cstr(cname, "rae_crypto_")) {
                    skip_unbox = true;
                }
                // Also look up function by name — if it's extern, skip
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count && !skip_unbox; i++) {
                    const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                    if (d->kind == AST_DECL_FUNC && str_eq(d->as.func_decl.name, cname) && d->as.func_decl.is_extern)
                        skip_unbox = true;
                }
            }
            if (skip_unbox) {
                emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
            } else {
                TypeInfo* t = expr->resolved_type; if (t->kind == TYPE_REF) t = t->as.ref.base;
                fprintf(out, "("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
                if (t->kind == TYPE_INT || t->kind == TYPE_CHAR) fprintf(out, ").as.i"); else if (t->kind == TYPE_FLOAT) fprintf(out, ").as.f"); else if (t->kind == TYPE_BOOL) fprintf(out, ").as.b"); else if (t->kind == TYPE_STRING) fprintf(out, ").as.s"); else fprintf(out, ").as.ptr");
            }
        } else emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
        break;
    }
    case AST_EXPR_OBJECT: {
        // Resolve the literal's struct type so we can propagate per-field expected
        // types into each value. This lets generic calls infer args from the
        // surrounding field type (e.g. `grid: createList(initialCap: 200)`
        // where the field's declared type is `List(Int)`).
        const AstTypeRef* obj_tr = expr->as.object_literal.type;
        if (!obj_tr && ctx->has_expected_type) obj_tr = &ctx->expected_type;
        const AstDecl* struct_decl = NULL;
        if (obj_tr) {
            Str obj_base = get_base_type_name(obj_tr);
            struct_decl = find_type_decl(ctx, ctx->module, obj_base);
        }
        if (expr->as.object_literal.type) {
            fprintf(out, "(");
            emit_type_ref_as_c_type(ctx, expr->as.object_literal.type, out, false);
            fprintf(out, ")");
        } else if (ctx->has_expected_type) {
            fprintf(out, "(");
            emit_type_ref_as_c_type(ctx, &ctx->expected_type, out, false);
            fprintf(out, ")");
        }
        fprintf(out, "{ ");
        bool saved_has_exp = ctx->has_expected_type;
        AstTypeRef saved_exp = ctx->expected_type;
        for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
            fprintf(out, ".%.*s = ", (int)f->name.len, f->name.data);
            // Look up the field's declared type and use it as expected_type.
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
            emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
            if (f->next) fprintf(out, ", ");
        }
        ctx->has_expected_type = saved_has_exp;
        ctx->expected_type = saved_exp;
        fprintf(out, " }");
        break;
    }
    case AST_EXPR_LIST: {
        for (const AstExprList* item = expr->as.list; item; item = item->next) {
            emit_expr(ctx, item->value, out, PREC_LOWEST, false, false);
            if (item->next) fprintf(out, ", ");
        }
        break;
    }
    case AST_EXPR_INTERP: {
        AstInterpPart* part = expr->as.interp.parts;
        if (!part) { fprintf(out, "(rae_String){(uint8_t*)\"\", 0}"); break; }
        // Count parts to determine nesting
        int count = 0;
        for (AstInterpPart* p = part; p; p = p->next) count++;
        if (count == 1) {
            // Single part - just emit as string
            if (part->value->kind == AST_EXPR_STRING) {
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
            } else {
                fprintf(out, "rae_ext_rae_str((");
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
                fprintf(out, "))");
            }
        } else {
            // Multiple parts - nest rae_ext_rae_str_concat calls
            // Build: concat(concat(concat(part1, str(part2)), str(part3)), str(part4))
            int opens = 0;
            bool first = true;
            for (AstInterpPart* p = part; p; p = p->next) {
                if (first) { first = false; continue; }
                fprintf(out, "rae_ext_rae_str_concat(");
                opens++;
            }
            // Emit first part
            if (part->value->kind == AST_EXPR_STRING) {
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
            } else {
                fprintf(out, "rae_ext_rae_str((");
                emit_expr(ctx, part->value, out, PREC_LOWEST, false, false);
                fprintf(out, "))");
            }
            // Emit remaining parts
            for (AstInterpPart* p = part->next; p; p = p->next) {
                fprintf(out, ", ");
                if (p->value->kind == AST_EXPR_STRING) {
                    emit_expr(ctx, p->value, out, PREC_LOWEST, false, false);
                } else {
                    fprintf(out, "rae_ext_rae_str((");
                    emit_expr(ctx, p->value, out, PREC_LOWEST, false, false);
                    fprintf(out, "))");
                }
                fprintf(out, ")");
            }
        }
        break;
    }
    case AST_EXPR_MATCH: {
        // Match expression: emit as ternary chain
        // match x { case 1 => 10, case 2 => 20, default => 30 }
        // -> (x == 1) ? 10 : (x == 2) ? 20 : 30
        const AstMatchArm* arm = expr->as.match_expr.arms;
        fprintf(out, "(");
        while (arm) {
            if (!arm->pattern) {
                // default arm
                emit_expr(ctx, arm->value, out, PREC_LOWEST, false, false);
            } else {
                // Check if string comparison needed
                const AstTypeRef* subj_tr = infer_expr_type_ref(ctx, expr->as.match_expr.subject);
                Str subj_base = get_base_type_name(subj_tr);
                bool is_string = str_eq_cstr(subj_base, "String") || str_eq_cstr(subj_base, "rae_String");
                if (is_string) {
                    fprintf(out, "rae_ext_rae_str_eq(");
                    emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST, false, false);
                    fprintf(out, ", ");
                    emit_expr(ctx, arm->pattern, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "(");
                    emit_expr(ctx, expr->as.match_expr.subject, out, PREC_LOWEST, false, false);
                    fprintf(out, " == ");
                    emit_expr(ctx, arm->pattern, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                }
                fprintf(out, " ? ");
                emit_expr(ctx, arm->value, out, PREC_LOWEST, false, false);
                fprintf(out, " : ");
            }
            arm = arm->next;
        }
        fprintf(out, ")");
        break;
    }
    case AST_EXPR_NONE: fprintf(out, "rae_any_none()"); break;
    default: break;
  }
  return true;
}

