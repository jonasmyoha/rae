// c_expr.c — Expression emission for the C backend.
//
// `emit_expr` is the per-AST-node switch that turns Rae expressions into C
// expressions. Method calls are lowered here to plain calls and forwarded to
// `emit_call_expr` (in c_call.c).

#include "c_backend.h"
#include "c_backend_internal.h"
#include "diag.h"
#include "mangler.h"
#include "sema.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// From c_stmt.c — counts identifier references to `name` in the
// body of `fd`. Used by Phase 2 deep-copy to decide whether a
// parameter source can be moved (count==1) or must be copied
// (count>=2).
extern int rae_func_count_param_refs(const AstFuncDecl* fd, Str name);

// Emit "rae_ext_rae_str(X)" for primitives, "rae_to_str_<Type>_(&X)" for user
// structs. The _Generic-based macro can't be extended from generated code,
// so user types route through the per-type function emitted in c_backend.c.
static void emit_to_string_expr(CFuncContext* ctx, const AstExpr* operand, FILE* out) {
    const AstTypeRef* tr = infer_expr_type_ref(ctx, operand);
    Str base = get_base_type_name(tr);
    const AstDecl* d = (base.len > 0) ? find_type_decl(ctx, ctx->module, base) : NULL;
    bool is_user_struct = d && d->kind == AST_DECL_TYPE
        && !has_property(d->as.type_decl.properties, "c_struct")
        && !d->as.type_decl.generic_params
        && !(tr && tr->is_opt);
    if (is_user_struct) {
        const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = base}});
        fprintf(out, "rae_to_str_%s_(&(", mangled);
        emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
        fprintf(out, "))");
    } else {
        fprintf(out, "rae_ext_rae_str((");
        emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
        fprintf(out, "))");
    }
}

// Does this expression evaluate to a `String` (or `view String` after
// the existing IDENT-deref) value? Used to drive the `+` → concat
// lowering — infer_expr_type_ref doesn't always pin .toString() /
// .concat() return types, so check shape first and fall back to type
// inference. The recursion through nested `+` lets chains like
// `a + b + c + d` resolve top-down without re-inferring at every node.
static bool expr_is_string_typed(CFuncContext* ctx, const AstExpr* e) {
    if (!e) return false;
    if (e->kind == AST_EXPR_STRING) return true;
    if (e->kind == AST_EXPR_INTERP) return true;
    if (e->kind == AST_EXPR_METHOD_CALL) {
        if (str_eq_cstr(e->as.method_call.method_name, "toString")) return true;
        if (str_eq_cstr(e->as.method_call.method_name, "concat")) return true;
    }
    if (e->kind == AST_EXPR_BINARY && e->as.binary.op == AST_BIN_ADD) {
        return expr_is_string_typed(ctx, e->as.binary.lhs) &&
               expr_is_string_typed(ctx, e->as.binary.rhs);
    }
    const AstTypeRef* tr = infer_expr_type_ref(ctx, e);
    if (tr) {
        Str b = get_base_type_name(tr);
        if (str_eq_cstr(b, "String")) return true;
    }
    return false;
}

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
        // `view T` / `mod T` for a non-primitive non-Buffer/List/Any
        // type lowers to a raw `T*` at the C level. When the IDENT
        // is read as a value (not an lvalue and no upstream
        // suppress_deref), emit `(*name)` so the c_struct or user-
        // struct field reads see a `T`, not a `T*`. Matches the
        // behaviour for List/Buffer pointer IDENTs below.
        bool is_struct_view = false;
        if (tr && (tr->is_view || tr->is_mod) && !is_prim_ref) {
            Str vb = get_base_type_name(tr);
            if (!str_eq_cstr(vb, "Buffer") && !str_eq_cstr(vb, "List") && !str_eq_cstr(vb, "Any")) {
                is_struct_view = true;
            }
        }

        if (is_prim_ref && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s.ptr)", (int)expr->as.ident.len, expr->as.ident.data);
        } else if (is_struct_view && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s)", (int)expr->as.ident.len, expr->as.ident.data);
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
      // (and its negation for `is not`).
      if (expr->as.binary.op == AST_BIN_IS || expr->as.binary.op == AST_BIN_NEQ) {
          const AstTypeRef* lhs_tr = infer_expr_type_ref(ctx, expr->as.binary.lhs);
          Str lhs_base = get_base_type_name(lhs_tr);
          bool lhs_is_string = str_eq_cstr(lhs_base, "String") || str_eq_cstr(lhs_base, "rae_String");
          bool rhs_is_string_lit = expr->as.binary.rhs->kind == AST_EXPR_STRING;
          // Also detect toString() calls — they always return String
          bool lhs_is_tostring = expr->as.binary.lhs->kind == AST_EXPR_METHOD_CALL &&
              str_eq_cstr(expr->as.binary.lhs->as.method_call.method_name, "toString");
          if (lhs_is_string || rhs_is_string_lit || lhs_is_tostring) {
              if (expr->as.binary.op == AST_BIN_NEQ) fprintf(out, "(bool)(!rae_ext_rae_str_eq(");
              else fprintf(out, "(bool)rae_ext_rae_str_eq(");
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              if (expr->as.binary.op == AST_BIN_NEQ) fprintf(out, "))");
              else fprintf(out, ")");
              break;
          }
      }
      // `x is none` / `x is not none`: emit a runtime tag check rather than
      // `==` / `!=`, because RaeAny is a struct and struct equality is invalid
      // C. The non-NONE side keeps its RaeAny value (suppress_opt_unbox).
      bool none_compare = (expr->as.binary.op == AST_BIN_IS || expr->as.binary.op == AST_BIN_NEQ) &&
          (expr->as.binary.rhs->kind == AST_EXPR_NONE || expr->as.binary.lhs->kind == AST_EXPR_NONE);
      bool saved_unbox = ctx->suppress_opt_unbox;
      if (none_compare) {
          const AstExpr* operand = (expr->as.binary.lhs->kind == AST_EXPR_NONE)
              ? expr->as.binary.rhs : expr->as.binary.lhs;
          ctx->suppress_opt_unbox = true;
          // Wrap the result in (bool) so the `_Generic` rae_ext_rae_str macro
          // matches the rae_Bool branch in interpolation contexts.
          if (expr->as.binary.op == AST_BIN_NEQ) fprintf(out, "((bool)(!rae_any_is_none(");
          else fprintf(out, "((bool)rae_any_is_none(");
          emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
          if (expr->as.binary.op == AST_BIN_NEQ) fprintf(out, ")))");
          else fprintf(out, "))");
          ctx->suppress_opt_unbox = saved_unbox;
          break;
      }
      // String concatenation: `lhs + rhs` lowers to a direct runtime
      // call when both sides are String (any combination of owned
      // String and view String). Mirrors what `.concat(other: ...)`
      // produces, so DX-friendly `dir + "/" + fileName` is equivalent
      // to the explicit method form. The runtime helper takes
      // rae_String by value; view-String identifiers already emit as
      // `(*x.ptr)` via the IDENT path above, so no extra wrapping is
      // needed here. After #197 the helper pool-registers its result
      // and after #198 the surrounding flush / pool_take handles
      // ownership, so nested chains like `(a + b) + c` don't leak.
      // Cross-type concat (e.g. `"count: " + count`) is deliberately
      // NOT supported — the user is expected to write `"count: " +
      // count.toString()` or, better, the interp form `"count: {count}"`.
      if (expr->as.binary.op == AST_BIN_ADD) {
          if (expr_is_string_typed(ctx, expr->as.binary.lhs) &&
              expr_is_string_typed(ctx, expr->as.binary.rhs)) {
              fprintf(out, "rae_ext_rae_str_concat(");
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              fprintf(out, ")");
              ctx->suppress_opt_unbox = saved_unbox;
              break;
          }
      }

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
                             expr->as.binary.op == AST_BIN_IS ||
                             expr->as.binary.op == AST_BIN_NEQ;
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
        case AST_BIN_NEQ: fprintf(out, " != "); break;
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
        // Built-in method: toString() → rae_ext_rae_str(object) or
        // rae_to_str_TYPE_(&object) for user structs.
        if (str_eq_cstr(expr->as.method_call.method_name, "toString") && !expr->as.method_call.args) {
            emit_to_string_expr(ctx, expr->as.method_call.object, out);
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
            // Type-on-LHS dot-call (new generic-call syntax): `Type.fn(...)`
            // where `Type` is a primitive or user-defined type and no local
            // binding shadows it. Lowers to `fn(<type-arg>, args)` so the
            // hoist pass in c_call.c picks Type up as the generic arg.
            // Mirrors the three accepted spellings:
            //   createList(type: String, initialCap: 4)
            //   createList(String, initialCap: 4)
            //   String.createList(initialCap: 4)
            bool obj_is_type = !obj_has_value && obj_name.len > 0 &&
                (is_primitive_type(obj_name) || (ctx->module && find_type_decl(ctx, ctx->module, obj_name) != NULL));
            if (obj_is_type && fn_exists) {
                AstExpr call = { .kind = AST_EXPR_CALL, .line = expr->line, .column = expr->column, .decl_link = expr->decl_link };
                call.as.call.callee = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstExpr));
                call.as.call.callee->kind = AST_EXPR_IDENT;
                call.as.call.callee->as.ident = expr->as.method_call.method_name;
                // Prepend the type as the first positional arg. The hoist
                // pass in emit_call_expr will move it into generic_args.
                AstCallArg* type_arg = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstCallArg));
                type_arg->name = (Str){0};
                type_arg->value = expr->as.method_call.object;
                type_arg->next = expr->as.method_call.args;
                call.as.call.args = type_arg;
                call.as.call.generic_args = expr->as.method_call.generic_args;
                emit_call_expr(ctx, &call, out); break;
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
    case AST_EXPR_OWN: {
        // `own x` — explicit ownership transfer. Mark the local moved
        // so emit_implicit_drops_for_body skips it, then emit the
        // inner expression's value as usual. At the C level the
        // wrapper is a pass-through; the bit lives in
        // ctx->local_moved.
        mark_expr_moved_if_local(ctx, expr->as.unary.operand);
        emit_expr(ctx, expr->as.unary.operand, out, parent_prec, is_lvalue, suppress_deref);
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
            // Stage 2 (owning field deep-copy):
            //
            // Before Stage 2, a bare IDENT source for a non-String
            // owning field (List/Map/struct) was handled by MARKING
            // the source local as moved (mark_expr_moved_if_local
            // below) and emitting the bare ident — a shallow byte
            // copy. That left h.<field> aliasing the source's heap
            // and made any later mutation of either side a use-
            // after-free or double-free hazard.
            //
            // The Stage 2 rule (matching Stage 1's let-stmt rule):
            //   - `field: ident` where the field's type needs deep
            //     copy → DEEP-COPY. Source stays live and unchanged;
            //     the field gets a private heap. No move-track.
            //   - `field: own ident`                → MOVE (handled
            //     by AST_EXPR_OWN's own emit which marks the source
            //     and forwards the bare value).
            //   - `field: ident` for a view-T field → BORROW
            //     (pass-through; no copy, no move).
            //
            // The String special-case is preserved further below.
            // For containers and nested owning structs we emit a
            // statement-expression that allocates a temp, calls the
            // synthesised rae_deep_copy_<MangledFieldType>, and
            // evaluates to the temp. The helpers are synthesised in
            // c_backend.c for every discovered List(E)/StringMap(V)/
            // IntMap(V) specialisation plus every non-generic user
            // struct that transitively owns heap.
            //
            // Also extends Stage 1's source-side handling from
            // IDENT-only to {IDENT, MEMBER, INDEX} since member-
            // access (`o.field`) and index (`xs.get(i)` style) are
            // structural aliases too.
            //
            // No move-track at this site. The source local is left
            // alive and end-of-scope auto-drop frees its own heap.
            //
            //   { f: src }           — `f: String` field gets a
            //                          fresh deep-copy via
            //                          rae_string_copy. `src` is
            //                          unchanged and still owns its
            //                          heap; the struct owns the
            //                          new copy. Dropping the struct
            //                          cannot free the caller's src.
            //
            //   { f: own src }       — explicit move. AST_EXPR_OWN's
            //                          emit marks `src` (when an
            //                          IDENT) as moved and forwards
            //                          the inner value. We wrap with
            //                          pool_take to detach a pool-
            //                          registered RHS (e.g. concat
            //                          result) so the surrounding
            //                          flush doesn't sweep it; locals
            //                          aren't in the pool so the take
            //                          is a no-op for them. The struct
            //                          field now exclusively owns the
            //                          heap.
            //
            //   `f: view String`     — borrow. Pass the value through;
            //                          the field holds a non-owning
            //                          view and dropping the struct
            //                          must not free the source.
            //
            // The previous Stage 8 behaviour of pool_take-only for
            // non-own RHS caused a chain of shallow aliases (the
            // JsonParser{ source: source } pattern) that auto-drop
            // could double-free once String field drops are enabled.
            // Deep-copy here gives every owned-String struct field a
            // private heap so the field's drop is always safe.
            bool field_is_owned_string = false;
            bool field_is_view_string  = false;
            if (field_tr && !field_tr->is_mod && !field_tr->is_opt) {
                Str fbase = get_base_type_name(field_tr);
                if (str_eq_cstr(fbase, "String")) {
                    if (field_tr->is_view) field_is_view_string = true;
                    else                   field_is_owned_string = true;
                }
            }
            bool rhs_is_own = (f->value && f->value->kind == AST_EXPR_OWN);
            // RHS is a call / interp / non-aliasing expression: the
            // value is an owned heap with no other live reference, so
            // we can MOVE (pool_take) instead of deep-copying. Saves
            // one allocation per struct-literal String field when the
            // field is initialised from a function return — e.g.
            // `Name { label: optString(...) }` would otherwise leak
            // the optString result.
            bool rhs_is_owning_temp = f->value && (
                f->value->kind == AST_EXPR_CALL ||
                f->value->kind == AST_EXPR_METHOD_CALL ||
                f->value->kind == AST_EXPR_INTERP ||
                f->value->kind == AST_EXPR_BINARY);
            // Detect "RHS is a function parameter used exactly
            // once". Parameters live in locals[0..func_first_let_idx).
            // Phase 2 deep-copy on a SINGLE-USE parameter can safely
            // MOVE the heap into the struct field — there's no later
            // read of the param. Multi-use params must deep-copy so
            // the source stays readable after this struct literal.
            bool rhs_is_param_ident = false;
            if (f->value && f->value->kind == AST_EXPR_IDENT &&
                ctx->func_first_let_idx != (size_t)-1 &&
                ctx->func_decl && ctx->func_decl->body) {
                Str name = f->value->as.ident;
                for (size_t li = 0; li < ctx->func_first_let_idx; li++) {
                    if (str_eq(ctx->locals[li], name)) {
                        int n = rae_func_count_param_refs(ctx->func_decl, name);
                        if (n == 1) rhs_is_param_ident = true;
                        break;
                    }
                }
            }
            // Stage 2: deep-copy non-String owning fields when the
            // RHS is an aliasing expression (IDENT / MEMBER / INDEX).
            // `own ident` keeps the move path. Owning-temp rvalues
            // (call / method-call / binary / interp / object) own
            // their heap with no other live ref — pass-through.
            bool rhs_is_aliasing = f->value && (
                f->value->kind == AST_EXPR_IDENT ||
                f->value->kind == AST_EXPR_MEMBER ||
                f->value->kind == AST_EXPR_INDEX);
            bool field_needs_deep_copy_nonstring = false;
            if (field_tr
                && !field_tr->is_view && !field_tr->is_mod && !field_tr->is_opt
                && !field_is_owned_string && !field_is_view_string
                && rhs_is_aliasing && !rhs_is_own
                && type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                        (AstTypeRef*)field_tr, 0)) {
                Str fbase = get_base_type_name(field_tr);
                // Only emit deep-copy when a synthesised helper exists
                // for the field's type. The helpers cover:
                //   - non-generic user structs (rae_deep_copy_<T>)
                //   - List(E) / StringMap(V) / IntMap(V)
                // If the type doesn't match those shapes we fall
                // through to the legacy pass-through (today no other
                // owning type can pass type_needs_deep_copy anyway —
                // see classifier in c_stmt.c).
                bool is_container = str_eq_cstr(fbase, "List") ||
                                    str_eq_cstr(fbase, "StringMap") ||
                                    str_eq_cstr(fbase, "IntMap");
                bool is_user_struct = false;
                if (!is_container) {
                    const AstDecl* fd = find_type_decl(ctx, ctx->module, fbase);
                    is_user_struct = fd && fd->kind == AST_DECL_TYPE
                        && !has_property(fd->as.type_decl.properties, "c_struct")
                        && !fd->as.type_decl.generic_params;
                }
                if (is_container || is_user_struct) {
                    field_needs_deep_copy_nonstring = true;
                } else {
                    // Stage 2 hard error: an owning field type we can't
                    // synthesise a deep-copy helper for. Today none
                    // exist (the classifier returns false otherwise),
                    // but this future-proofs the path so a new owning
                    // type can't silently shallow-alias.
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                        "cannot copy owning field '%.*s' of type '%.*s' because deep-copy is not implemented; use `own` to move or `view` to borrow",
                        (int)f->name.len, f->name.data,
                        (int)fbase.len, fbase.data);
                    diag_error(ctx->module ? ctx->module->file_path : "<unknown>",
                               (int)expr->line, (int)expr->column, msg);
                }
            }

            if (field_needs_deep_copy_nonstring) {
                // Emit a GCC statement-expression: declare a temp of
                // the field's mangled C type, call rae_deep_copy_<T>,
                // evaluate to the temp. Mirrors the pattern Stage 1
                // uses for `let b: T = a` in c_stmt.c. The synthesised
                // helper allocates a fresh backing buffer / recursive
                // sub-copies so the field's heap is independent of
                // the source.
                const char* tn_dc = rae_mangle_type_specialized(
                    ctx->compiler_ctx, ctx->generic_params,
                    ctx->generic_args, (AstTypeRef*)field_tr);
                int tmp_id = ctx->temp_counter++;
                fprintf(out, "(__extension__ ({ %s __fdc%d; rae_deep_copy_%s(&__fdc%d, &(",
                        tn_dc, tmp_id, tn_dc, tmp_id);
                emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
                fprintf(out, ")); __fdc%d; }))", tmp_id);
            } else if (field_is_owned_string && (rhs_is_own || rhs_is_owning_temp)) {
                fprintf(out, "rae_string_pool_take(");
                emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
                fprintf(out, ")");
            } else if (field_is_owned_string && rhs_is_param_ident) {
                // Move-when-safe: the parameter's heap is NOT owned
                // by anything visible after this function returns
                // (the caller passed it owning-temp via pool_take,
                // or it's borrowed/literal — checked at runtime).
                // Transfer ownership into the field; deep-copy only
                // when the source is still pool-registered.
                fprintf(out, "rae_string_move_or_copy(&(");
                emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
                fprintf(out, "))");
            } else if (field_is_owned_string) {
                fprintf(out, "rae_string_copy(");
                emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
                fprintf(out, ")");
            } else {
                // View String / non-String fields — pass-through.
                (void)field_is_view_string;
                // Value-T field receiving a `view T` IDENT source
                // (e.g. `.fill = fillParam` where fillParam is `view
                // RgbaColor`): the IDENT emits as `rae_RgbaColor*`
                // for non-primitive views (is_primitive_ref returns
                // false), but the field wants a value. Auto-deref
                // here so c_struct / user-struct view params can be
                // forwarded into struct literals without callers
                // having to drop the `view`.
                bool needs_view_deref = false;
                if (field_tr && !field_tr->is_view && !field_tr->is_mod
                    && f->value && f->value->kind == AST_EXPR_IDENT) {
                    const AstTypeRef* rhs_tr = infer_expr_type_ref(ctx, f->value);
                    if (rhs_tr && (rhs_tr->is_view || rhs_tr->is_mod)
                        && !is_primitive_ref(ctx, rhs_tr)) {
                        needs_view_deref = true;
                    }
                }
                if (needs_view_deref) fprintf(out, "(*");
                emit_expr(ctx, f->value, out, PREC_LOWEST, false, needs_view_deref);
                if (needs_view_deref) fprintf(out, ")");
            }
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
        // Stage 4: interpolation lowers to a single varargs runtime
        // call that concatenates all parts into one owned String,
        // frees any owned input parts (the str_i64/str_f64/etc.
        // conversions that produce heap data), and registers the
        // result with the per-statement string pool. The result
        // gets flushed at end-of-statement unless a let/assign/etc.
        // explicitly takes ownership via rae_string_pool_take(...).
        //
        // For each part:
        //   - AST_EXPR_STRING (literal)  → emit as borrowed (is_owned=0)
        //   - AST_EXPR_IDENT of String   → wrap with rae_string_borrow
        //                                  to clear is_owned (the local
        //                                  still owns the data; we don't
        //                                  want str_interp to free it)
        //   - everything else            → emit_to_string_expr, which
        //                                  produces an owned heap result
        //                                  that str_interp consumes.
        AstInterpPart* part = expr->as.interp.parts;
        if (!part) { fprintf(out, "(rae_String){(uint8_t*)\"\", 0, 0, 0}"); break; }
        int count = 0;
        for (AstInterpPart* p = part; p; p = p->next) count++;

        fprintf(out, "rae_ext_rae_str_interp(%d", count);
        for (AstInterpPart* p = part; p; p = p->next) {
            fprintf(out, ", ");
            if (p->value->kind == AST_EXPR_STRING) {
                emit_expr(ctx, p->value, out, PREC_LOWEST, false, false);
            } else if (p->value->kind == AST_EXPR_IDENT) {
                // Identifier reference — wrap in rae_string_borrow so
                // str_interp does NOT free the user's local at the end
                // of interpolation. Skip the wrap when the identifier
                // is `opt String` (a RaeAny in C, not a rae_String) or
                // any view/mod ref — the standard emit_to_string_expr
                // path through `rae_ext_rae_str(...)` handles those
                // via _Generic dispatch.
                const AstTypeRef* ptr = infer_expr_type_ref(ctx, p->value);
                Str pbase = get_base_type_name(ptr);
                bool plain_string = ptr && !ptr->is_opt && !ptr->is_view && !ptr->is_mod
                                    && str_eq_cstr(pbase, "String");
                if (plain_string) {
                    fprintf(out, "rae_string_borrow(");
                    emit_expr(ctx, p->value, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                } else {
                    emit_to_string_expr(ctx, p->value, out);
                }
            } else {
                emit_to_string_expr(ctx, p->value, out);
            }
        }
        fprintf(out, ")");
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

