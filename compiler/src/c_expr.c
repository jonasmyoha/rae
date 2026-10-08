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
#include <limits.h>
#include <errno.h>
#include "lexer.h"

// Emit `"file", line` for a runtime diagnostic that points back at Rae source
// (e.g. the Array bounds check). The file path is written as an escaped C string
// literal so a path containing a quote or backslash cannot break the generated
// C. The line is the indexing expression's own line.
/* An expression built only from integer literals, negation and arithmetic —
 * a C constant expression. Such an expression keeps the bare C operator
 * instead of the checked Int helper: a helper call is not a constant
 * expression, so it cannot initialise a module-level `let` (a C static), and
 * a constant divisor of zero is already a sema error so nothing is lost. */
static bool expr_is_const_int(const AstExpr* expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case AST_EXPR_INTEGER: return true;
        case AST_EXPR_UNARY: return expr->as.unary.op == AST_UNARY_NEG && expr_is_const_int(expr->as.unary.operand);
        case AST_EXPR_BINARY:
            return expr->as.binary.op >= AST_BIN_ADD && expr->as.binary.op <= AST_BIN_MOD
                   && expr_is_const_int(expr->as.binary.lhs) && expr_is_const_int(expr->as.binary.rhs);
        default: return false;
    }
}

static void emit_c_source_location(CFuncContext* ctx, const AstExpr* expr,
                                   FILE* out) {
    const char* file = (ctx && ctx->module && ctx->module->file_path)
                           ? ctx->module->file_path
                           : "<unknown>";
    fputc('"', out);
    for (const char* p = file; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', out);
        fputc(*p, out);
    }
    fprintf(out, "\", %d", (int)expr->line);
}

// The base type name of an expression AFTER applying the function's active
// generic substitution. In a specialized generic body `a: view T` infers as
// `T`; substituting `T -> String` lets the String / struct `is` lowering fire
// (otherwise `a is b` with T=String falls through to `rae_String == ...`, a C
// error — the reason a String `equals` helper existed).
static Str eff_base_name(CFuncContext* ctx, const AstExpr* e) {
    const AstTypeRef* tr = infer_expr_type_ref(ctx, e);
    if (tr && ctx && ctx->generic_params && ctx->generic_args) {
        AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params,
                                              ctx->generic_args, tr);
        if (sub) tr = sub;
    }
    return get_base_type_name(tr);
}

// A c_struct field whose real C name is a Rae reserved keyword is escaped by the
// bindings generator with a single trailing '_' (WGPU*.view -> view_). safe_name
// appends '_' ONLY for exact keyword matches, so stripping one trailing '_' when
// the remainder is a keyword recovers the true C field name exactly, and can
// never disturb a genuine field ending in '_' (its base is not a keyword).
// `is_c_struct` gates this so ordinary Rae structs are untouched.
static Str c_struct_field_c_name(Str name, bool is_c_struct) {
    if (is_c_struct && name.len >= 2 && name.data[name.len - 1] == '_') {
        Str base = { name.data, name.len - 1 };
        if (lookup_keyword(base) != TOK_IDENT) return base;
    }
    return name;
}

// True when a type ref denotes a c_struct (its declaration carries `c_struct`).
static bool type_ref_is_c_struct(CFuncContext* ctx, const AstTypeRef* tr) {
    if (!tr || !ctx) return false;
    if (tr->resolved_type && tr->resolved_type->kind == TYPE_STRUCT
        && tr->resolved_type->as.structure.decl
        && tr->resolved_type->as.structure.decl->kind == AST_DECL_TYPE)
        return has_property(tr->resolved_type->as.structure.decl->as.type_decl.properties, "c_struct");
    Str base = {0};
    if (tr->parts) base = tr->parts->text;
    else if (tr->resolved_type) base = tr->resolved_type->name;
    if (base.len == 0 || !ctx->module) return false;
    const AstDecl* decl = find_type_decl(ctx, ctx->module, base);
    return decl && decl->kind == AST_DECL_TYPE
        && has_property(decl->as.type_decl.properties, "c_struct");
}

// From c_stmt.c — counts identifier references to `name` in the
// body of `fd`. Used by Phase 2 deep-copy to decide whether a
// parameter source can be moved (count==1) or must be copied
// (count>=2).
extern int rae_func_count_param_refs(const AstFuncDecl* fd, Str name);

// Emit "rae_ext_rae_str(X)" for primitives, "rae_to_str_<Type>_(&X)" for user
// structs. The _Generic-based macro can't be extended from generated code,
// so user types route through the per-type function emitted in c_backend.c.
// The runtime formatter of an integer type, by its Rae name (NULL for any
// other type): rae_str_<width> in rae_runtime.h, which also accept a pointer
// to the value (how a view/mod binding may be emitted).
static const char* rae_int_formatter(Str base) {
    if (str_eq_cstr(base, "Int8")) return "rae_str_int8";
    if (str_eq_cstr(base, "Int16")) return "rae_str_int16";
    if (str_eq_cstr(base, "Int32")) return "rae_str_int32";
    if (str_eq_cstr(base, "UInt8")) return "rae_str_uint8";
    if (str_eq_cstr(base, "UInt16")) return "rae_str_uint16";
    if (str_eq_cstr(base, "UInt32")) return "rae_str_uint32";
    if (str_eq_cstr(base, "UInt64")) return "rae_str_uint64";
    return NULL;
}

void emit_to_string_expr(CFuncContext* ctx, const AstExpr* operand, FILE* out) {
    const AstTypeRef* tr = infer_expr_type_ref(ctx, operand);
    // In a generic body the operand's type may be the parameter `T`: the
    // instantiation being emitted knows the concrete type, so substitute it
    // first — then the rules below dispatch per instantiation (an enum to its
    // member name, a struct / List to its generated toString) instead of
    // falling into the String arm of the _Generic macro.
    AstTypeRef substituted_local;
    if (tr && ctx->generic_params && ctx->generic_args) {
        const AstTypeRef* substituted = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
        if (substituted) {
            // T = opt X through a `view T` parameter: the instantiation passes
            // the struct-rep opt by value, so format it as the value it is.
            substituted_local = *substituted;
            if (substituted_local.is_opt && (substituted_local.is_view || substituted_local.is_mod)
                && rae_opt_is_struct_rep(ctx, &substituted_local)) {
                substituted_local.is_view = false; substituted_local.is_mod = false;
            }
            tr = &substituted_local;
        }
    }
    Str base = get_base_type_name(tr);
    // A direct enum member access `Enum.member` may not infer to the enum type;
    // recover the enum name from the object so it stringifies to the name too.
    if (base.len == 0 && operand->kind == AST_EXPR_MEMBER
        && operand->as.member.object->kind == AST_EXPR_IDENT
        && find_enum_decl(ctx, ctx->module, operand->as.member.object->as.ident) != NULL) {
        base = operand->as.member.object->as.ident;
    }
    // Enum value -> its member NAME (auto-generated rae_enum_toString_<Enum>),
    // so ClipKind.walk.toString() is "walk", not the ordinal "1".
    if (base.len > 0 && !(tr && tr->is_opt) && find_enum_decl(ctx, ctx->module, base) != NULL) {
        fprintf(out, "rae_enum_toString_%.*s((int64_t)(", (int)base.len, base.data);
        emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
        fprintf(out, "))");
        return;
    }
    // #651: a value-opt (`opt <scalar/String/struct>`) is now a `struct
    // rae_opt_<T>`, which the _Generic `rae_ext_rae_str` cannot dispatch on.
    // Format `.value` when present (via its concrete formatter), else "none"
    // — matching the Live VM's rendering of a none optional.
    if (tr && tr->is_opt && !(tr->is_view || tr->is_mod)
        && rae_opt_is_struct_rep(ctx, tr)) {
        const AstDecl* pd = (base.len > 0) ? find_type_decl(ctx, ctx->module, base) : NULL;
        bool payload_is_user_struct = pd && pd->kind == AST_DECL_TYPE
            && !has_property(pd->as.type_decl.properties, "c_struct")
            && !pd->as.type_decl.generic_params;
        int oid = ctx->temp_counter++;
        fprintf(out, "(__extension__ ({ %s __ostr%d = (", rae_opt_type_name(ctx, tr), oid);
        // Capture the whole `rae_opt_<T>` — suppress the call emitter's auto
        // `.value` unbox, which would hand back the payload and mistype the temp.
        bool saved_unbox = ctx->suppress_opt_unbox;
        ctx->suppress_opt_unbox = true;
        emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
        ctx->suppress_opt_unbox = saved_unbox;
        fprintf(out, "); __ostr%d.has ? ", oid);
        (void)payload_is_user_struct;
        {
            AstTypeRef payload = *tr; payload.is_opt = false; payload.next = NULL;
            char inner[96];
            // An `opt String` made by a call (`"{list.copyAt(index: 0)}"`)
            // owns its String, and unlike a plain String return it is not in
            // the statement's pool: register it there so the statement's
            // flush frees it instead of leaking one String per format.
            bool owned_call = (operand->kind == AST_EXPR_CALL
                || operand->kind == AST_EXPR_METHOD_CALL)
                && str_eq_cstr(base, "String");
            if (owned_call) {
                snprintf(inner, sizeof inner, "rae_string_pool_register_owned(__ostr%d.value)", oid);
            } else {
                snprintf(inner, sizeof inner, "__ostr%d.value", oid);
            }
            rae_value_to_str_expr(ctx->compiler_ctx, ctx->module, &payload, inner, false, out);
        }
        fprintf(out, " : (rae_String){(uint8_t*)\"none\", 4}; }))");
        return;
    }
    // A struct or List VALUE made by a call (`"{makeWords()}"`) is a temporary
    // this expression owns: hold it in a named temp, format it, release it.
    // Taking `&(call())` did not compile for a plain struct, and the
    // List / generic path below captured the value but never released it.
    // A literal (`Named { name: "a {n}" }`, `List(Int) { 1, 2 }`) owns its
    // fields the same way.
    if (tr && !tr->is_opt && !tr->is_view && !tr->is_mod
        && (operand->kind == AST_EXPR_CALL || operand->kind == AST_EXPR_METHOD_CALL
            || operand->kind == AST_EXPR_OBJECT || operand->kind == AST_EXPR_COLLECTION_LITERAL)) {
        AstTypeRef value_type = *tr;
        value_type.next = NULL;
        const AstDecl* vd = (base.len > 0) ? find_type_decl(ctx, ctx->module, base) : NULL;
        bool plain_struct = vd && vd->kind == AST_DECL_TYPE
            && !has_property(vd->as.type_decl.properties, "c_struct")
            && !vd->as.type_decl.generic_params && !tr->generic_args;
        bool formatted = (str_eq_cstr(base, "List") && tr->generic_args)
            || generic_struct_template(ctx->compiler_ctx, &value_type) || plain_struct;
        const char* mangled = formatted
            ? rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, &value_type) : NULL;
        if (mangled) {
            int vid = ctx->temp_counter++;
            char tname[48];
            snprintf(tname, sizeof tname, "__vstr%d", vid);
            fprintf(out, "(__extension__ ({ ");
            emit_type_ref_as_c_type(ctx, &value_type, out, false);
            fprintf(out, " %s = ", tname);
            emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
            fprintf(out, "; rae_String __vs%d = rae_to_str_%s_(&%s);", vid, mangled, tname);
            emit_drop_for_value(ctx, out, &value_type, tname, true);
            fprintf(out, " __vs%d; }))", vid);
            return;
        }
    }
    // A List(T): its generated toString (elements recursively). The operand
    // is captured into a temp so the formatter can take its address.
    // A generic struct instance (Pair(String)) the same way: its generated
    // rae_to_str_<instance>_ (c_struct_shapes.c).
    const AstTypeRef* generic_sub = NULL;
    if (tr && !tr->is_opt && tr->generic_args && !str_eq_cstr(base, "List")) {
        generic_sub = tr;
        if (ctx->generic_params && ctx->generic_args) {
            const AstTypeRef* s2 = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
            if (s2) generic_sub = s2;
        }
        AstTypeRef plain = *generic_sub; plain.is_view = false; plain.is_mod = false;
        if (!generic_struct_template(ctx->compiler_ctx, &plain)) generic_sub = NULL;
    }
    if (tr && !tr->is_opt && ((str_eq_cstr(base, "List") && tr->generic_args) || generic_sub)) {
        const AstTypeRef* sub = tr;
        if (ctx->generic_params && ctx->generic_args) {
            const AstTypeRef* s2 = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, tr);
            if (s2) sub = s2;
        }
        AstTypeRef sub_plain = *sub; sub_plain.is_view = false; sub_plain.is_mod = false;
        sub = &sub_plain;
        const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, (AstTypeRef*)sub);
        if (mangled) {
            // The operand may be the list itself or a pointer to it (a `view`
            // alias such as a field-loop binding is emitted undereferenced);
            // _Generic picks the address either way, without copying the list.
            int lid = ctx->temp_counter++;
            fprintf(out, "(__extension__ ({ __typeof__(");
            emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
            fprintf(out, ") __lstr%d = (", lid);
            emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
            fprintf(out, "); rae_to_str_%s_(_Generic((__lstr%d), %s*: *(%s**)&__lstr%d, const %s*: *(const %s**)&__lstr%d, default: &__lstr%d)); }))",
                mangled, lid, mangled, mangled, lid, mangled, mangled, lid, lid);
            return;
        }
    }
    const AstDecl* d = (base.len > 0) ? find_type_decl(ctx, ctx->module, base) : NULL;
    bool is_user_struct = d && d->kind == AST_DECL_TYPE
        && !has_property(d->as.type_decl.properties, "c_struct")
        && !d->as.type_decl.generic_params
        && !(tr && tr->is_opt);
    // Integers by their Rae type (rae_int_to_str_expr): the _Generic below sees
    // only C types, where UInt32 is Char's uint32_t and Int8 may be Bool's
    // int8_t.
    const char* int_formatter = (tr && !tr->is_opt) ? rae_int_formatter(base) : NULL;
    if (is_user_struct) {
        const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, &(AstTypeRef){.parts = &(AstIdentifierPart){.text = base}});
        fprintf(out, "rae_to_str_%s_(&(", mangled);
        emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
        fprintf(out, "))");
    } else if (int_formatter) {
        fprintf(out, "%s((", int_formatter);
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


// #32786425: emit `expr` — a member chain whose innermost object is `base`, a
// call — with `base` evaluated once into statement temporary `tmp_id` (raised
// `_set` so the statement's drop list releases it) and the fields read off
// that temporary.
static void emit_member_chain_on_temp(CFuncContext* ctx, const AstExpr* expr, const AstExpr* base,
                                      int tmp_id, FILE* out) {
    if (expr == base) {
        // `(*(..., &tmp))`, not `(..., tmp)`: a comma expression is not an
        // lvalue in C, a dereferenced pointer is. The chain read off it is
        // therefore a PLACE inside the temporary, which is what lets every
        // consumer keep its own rule: a `let`/assignment deep-copies the field
        // out, a `view` argument borrows it for the call, `own` moves it out
        // and zeroes the slot — and the temporary's drop after the statement
        // releases whatever is left (#45158908).
        fprintf(out, "(*((__rae_stmt_tmp%d = (", tmp_id);
        emit_expr(ctx, base, out, PREC_LOWEST, false, false);
        fprintf(out, ")), (__rae_stmt_tmp%d_set = 1), &__rae_stmt_tmp%d))", tmp_id, tmp_id);
        return;
    }
    const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.member.object);
    bool use_arrow = expr->as.member.object != base && obj_tr && (obj_tr->is_view || obj_tr->is_mod);
    emit_member_chain_on_temp(ctx, expr->as.member.object, base, tmp_id, out);
    Str fld = c_struct_field_c_name(expr->as.member.member, type_ref_is_c_struct(ctx, obj_tr));
    fprintf(out, "%s%.*s", use_arrow ? "->" : ".", (int)fld.len, fld.data);
}


// `list.copyAtDefault(index:)`, `list.copyAtFallback(index:fallback:)` and
// `list.set(index:value:)` on a List whose element owns no heap (Int, Float,
// Bool, a struct of those) lower to a length check plus one load or store,
// exactly like the narrowed `if let ... = copyAt` / `modAt` forms
// (emit_list_if_let). As library calls they cost 3-4x those forms in a hot
// loop: copyAtDefault built an `opt T` through copyAt and unwrapped it, and set
// carried its interpolated warning in the hot path (benchmarks/list_access,
// "Scatter update"). The semantics are the library's: an index out of range
// reads the type's zero value / the fallback, and a store is ignored with the
// same one-line warning. Arguments are evaluated once, in source order. An
// element that owns heap (String, List, a struct with them) keeps the library
// call, which deep-copies a read and drops the overwritten value.
static bool emit_list_fast_access(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  Str method = expr->as.method_call.method_name;
  bool read_default = str_eq_cstr(method, "copyAtDefault");
  bool read_fallback = str_eq_cstr(method, "copyAtFallback");
  bool write = str_eq_cstr(method, "set");
  if (!read_default && !read_fallback && !write) return false;
  const AstTypeRef* list_type = infer_expr_type_ref(ctx, expr->as.method_call.object);
  if (!list_type || list_type->is_opt || !str_eq_cstr(get_base_type_name(list_type), "List")
      || !list_type->generic_args || list_type->generic_args->next) return false;
  const AstTypeRef* element = list_type->generic_args;
  if (ctx->generic_params && ctx->generic_args)
    element = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args,
                                  (AstTypeRef*)element);
  if (!element || element->is_opt || element->is_view || element->is_mod) return false;
  Str element_base = get_base_type_name(element);
  if (element_base.len == 0 || str_eq_cstr(element_base, "Any") || str_eq_cstr(element_base, "RaeAny")
      || str_eq_cstr(element_base, "String")) return false;
  for (const AstIdentifierPart* gp = ctx->generic_params; gp; gp = gp->next)
    if (str_eq(gp->text, element_base)) return false;  // still abstract
  if (type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, element, 0)) return false;

  const AstExpr* index_value = NULL;
  const AstExpr* extra_value = NULL;  // the fallback, or the value stored
  for (const AstCallArg* a = expr->as.method_call.args; a; a = a->next) {
    if (str_eq_cstr(a->name, "index")) index_value = a->value;
    else if (read_fallback && str_eq_cstr(a->name, "fallback")) extra_value = a->value;
    else if (write && str_eq_cstr(a->name, "value")) extra_value = a->value;
    else return false;
  }
  if (!index_value || ((read_fallback || write) && !extra_value)) return false;
  // The list must be a PLACE (a local, a field, an element): the fast path
  // takes its address. A list a call returns is an rvalue the caller owns
  // (`grid.copyAtDefault(index: 0).copyAtDefault(index: 0)` took
  // `&(copyAtDefault(...))`, which is not C, and would leak the copy); the
  // library call binds and drops such a temporary.
  const AstExpr* list_object = expr->as.method_call.object;
  if (list_object->kind != AST_EXPR_IDENT && list_object->kind != AST_EXPR_MEMBER
      && list_object->kind != AST_EXPR_INDEX) return false;

  int id = ctx->temp_counter++;
  bool list_is_ref = list_type->is_view || list_type->is_mod;
  fprintf(out, "(__extension__ ({ __auto_type __rae_flist%d = ", id);
  if (!list_is_ref) fprintf(out, "&(");
  emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, true);
  if (!list_is_ref) fprintf(out, ")");
  fprintf(out, "; int64_t __rae_findex%d = ", id);
  emit_expr(ctx, index_value, out, PREC_LOWEST, false, false);
  fprintf(out, "; ");
  if (extra_value) {
    bool had_exp = ctx->has_expected_type;
    AstTypeRef saved_exp = ctx->expected_type;
    ctx->expected_type = *element;
    ctx->has_expected_type = true;
    emit_type_ref_as_c_type(ctx, element, out, false);
    fprintf(out, " __rae_fvalue%d = ", id);
    emit_expr(ctx, extra_value, out, PREC_LOWEST, false, false);
    fprintf(out, "; ");
    ctx->has_expected_type = had_exp;
    ctx->expected_type = saved_exp;
  }
  if (write) {
    fprintf(out, "if ((uint64_t)__rae_findex%d < (uint64_t)__rae_flist%d->length) "
                 "__rae_flist%d->data[__rae_findex%d] = __rae_fvalue%d; "
                 "else rae_list_set_out_of_range(__rae_findex%d, __rae_flist%d->length); (void)0; }))",
            id, id, id, id, id, id, id);
  } else {
    // A typed result, not `?:`: the conditional operator promotes a Bool to
    // int, and interpolation would then print 1 instead of true.
    emit_type_ref_as_c_type(ctx, element, out, false);
    if (read_fallback) {
      fprintf(out, " __rae_fresult%d = __rae_fvalue%d; ", id, id);
    } else {
      fprintf(out, " __rae_fresult%d = (", id);
      emit_type_ref_as_c_type(ctx, element, out, false);
      fprintf(out, "){0}; ");
    }
    fprintf(out, "if ((uint64_t)__rae_findex%d < (uint64_t)__rae_flist%d->length) "
                 "__rae_fresult%d = __rae_flist%d->data[__rae_findex%d]; __rae_fresult%d; }))",
            id, id, id, id, id, id);
  }
  return true;
}

bool emit_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out, int parent_prec, bool is_lvalue, bool suppress_deref) {
  if (!expr) return true;
  switch (expr->kind) {
    case AST_EXPR_INTEGER: {
        // #817: a literal above INT64_MAX (18446744073709551615) or any integer
        // literal in a UInt64-typed context is emitted as uint64 — `%sLL` would
        // be out of range (clang silently reads it unsigned then the
        // (int64_t) cast makes it -1) and `4294967295 * 4294967295` would
        // overflow in int64 before the assignment converts.
        char buf[64]; size_t n = expr->as.integer.len < 63 ? expr->as.integer.len : 63;
        memcpy(buf, expr->as.integer.data, n); buf[n] = '\0';
        errno = 0;
        unsigned long long u = strtoull(buf, NULL, 0);
        bool big = errno != ERANGE && u > (unsigned long long)LLONG_MAX;
        bool want_u64 = ctx->has_expected_type && !ctx->expected_type.is_opt
                     && str_eq_cstr(get_base_type_name(&ctx->expected_type), "UInt64");
        if (big || want_u64) fprintf(out, "((uint64_t)%.*sULL)", (int)expr->as.integer.len, expr->as.integer.data);
        else fprintf(out, "((int64_t)%.*sLL)", (int)expr->as.integer.len, expr->as.integer.data);
        break;
    }
    case AST_EXPR_FLOAT:
        // An f32-context literal (sema_mark_f32_literal) is a C float literal.
        fprintf(out, "%.*s%s", (int)expr->as.floating.len, expr->as.floating.data,
                expr->is_f32_literal ? "f" : "");
        break;
    case AST_EXPR_BOOL: fprintf(out, "(bool)%s", expr->as.boolean ? "true" : "false"); break;
    case AST_EXPR_STRING: emit_string_literal(out, expr->as.string_lit); break;
    case AST_EXPR_CHAR: fprintf(out, "(uint32_t)%uU", (uint32_t)expr->as.char_value); break;
    case AST_EXPR_IDENT: {
        const AstTypeRef* tr = infer_expr_type_ref(ctx, expr);
        const Str cn = ident_c_name(ctx, expr);  // #816: prefixed symbol for a module global
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
            // Stage 6: view-on-numeric-primitive is pass-by-value at the
            // C level (not a struct view), so the IDENT should be read
            // directly without dereferencing. Only non-primitive views
            // are real `T*` references. When the param is a generic T
            // bound to a concrete primitive (e.g. T=Int in a List(Int)
            // specialisation), check the substituted base too.
            Str vb_concrete = vb;
            if (ctx->generic_params && ctx->generic_args) {
                const AstIdentifierPart* gp = ctx->generic_params;
                const AstTypeRef* ga = ctx->generic_args;
                while (gp && ga) {
                    if (str_eq(gp->text, vb)) {
                        vb_concrete = get_base_type_name(ga);
                        break;
                    }
                    gp = gp->next; ga = ga->next;
                }
            }
            bool is_num_prim = is_scalar_primitive_type(vb_concrete);
            if (!str_eq_cstr(vb, "Buffer") && !str_eq_cstr(vb, "List") && !str_eq_cstr(vb, "Any") && !is_num_prim) {
                is_struct_view = true;
            }
            // `mod` of a fixed-width integer (UInt64, Int32, ...) has no
            // `{ .ptr }` wrapper: it is a plain `T*`, read through `(*name)`.
            if (is_num_prim && tr->is_mod && !c_primitive_ref_has_wrapper(vb_concrete)) {
                is_struct_view = true;
            }
        }

        // A `view`/`mod` LOCAL of numeric primitive type is a real reference,
        // even though the same type as a PARAMETER is passed by value.
        //
        // The two are not inconsistent. Spec 2.3 permits implementing a view as
        // a copy exactly where no change to the source is observable during the
        // binding's lifetime — true for a parameter across a call, false for a
        // local whose source sits in the same scope and can be assigned on the
        // next line. So the local keeps a pointer (the declaration already
        // emits `{ .ptr = &x }`) and reads through it must dereference.
        //
        // Without this, `let v: view Int => a` bound fine and then failed to
        // compile the moment it was read: rae_View_Int64 where int64_t was
        // wanted. Locals are those at or after func_first_let_idx; anything
        // before that index is a parameter.
        bool is_local_prim_view = false;
        if (!is_prim_ref && tr && tr->is_view && ctx->func_first_let_idx != (size_t)-1) {
            Str vb = get_base_type_name(tr);
            bool num = str_eq_cstr(vb, "Int") || str_eq_cstr(vb, "Int64") ||
                       str_eq_cstr(vb, "Float") || str_eq_cstr(vb, "Float32") ||
                       str_eq_cstr(vb, "Float64") || str_eq_cstr(vb, "Bool") ||
                       str_eq_cstr(vb, "Char") || str_eq_cstr(vb, "Char32");
            if (num) {
                for (size_t li = ctx->func_first_let_idx; li < ctx->local_count; li++) {
                    if (str_eq(ctx->locals[li], expr->as.ident)) { is_local_prim_view = true; break; }
                }
            }
        }

        if ((is_prim_ref || is_local_prim_view) && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s.ptr)", (int)cn.len, cn.data);
        } else if (is_struct_view && !is_lvalue && !suppress_deref) {
            fprintf(out, "(*%.*s)", (int)cn.len, cn.data);
        } else if (is_ptr && !is_lvalue && !suppress_deref) {
            // Check if it's a Buffer or List - they are pointers but shouldn't be dereferenced here
            // if we are just passing them or accessing members via ->
            Str base = get_base_type_name(tr);
            if (str_eq_cstr(base, "Buffer") || str_eq_cstr(base, "List")) {
                fprintf(out, "%.*s", (int)cn.len, cn.data);
            } else {
                fprintf(out, "(*%.*s)", (int)cn.len, cn.data);
            }
        } else {
            fprintf(out, "%.*s", (int)cn.len, cn.data);
        }
        break;
    }
    case AST_EXPR_CAST: {
      /* `value as Type` lowers to a plain C cast. The type system already
       * verified the conversion is a supported numeric one, and Float /
       * Float32 resolve to the same TypeInfo so that spelling is a no-op. */
      const char* cname = NULL;
      if (expr->resolved_type) {
        switch (expr->resolved_type->kind) {
          case TYPE_FLOAT:   cname = "float";   break;
          case TYPE_FLOAT64: cname = "double";  break;
          /* Every fixed-width integer is TYPE_INT; its width and signedness
           * pick the C type, so `x as UInt64` is a uint64_t cast. */
          case TYPE_INT:
            cname = rae_int_c_name(expr->resolved_type->as.integer.bits,
                                   expr->resolved_type->as.integer.is_unsigned);
            break;
          default: break;
        }
      }
      if (cname) fprintf(out, "((%s)(", cname); else fprintf(out, "((");
      emit_expr(ctx, expr->as.cast.operand, out, PREC_LOWEST, false, false);
      fprintf(out, "))");
      break;
    }
    case AST_EXPR_BINARY: {
      // `x is none` / `x is not none`: emit a runtime tag check rather than
      // `==` / `!=`, because RaeAny is a struct and struct equality is invalid
      // C. Run this before String equality: `opt String` has base name String
      // but is physically represented as RaeAny.
      bool none_compare = (expr->as.binary.op == AST_BIN_IS || expr->as.binary.op == AST_BIN_NEQ) &&
          (expr->as.binary.rhs->kind == AST_EXPR_NONE || expr->as.binary.lhs->kind == AST_EXPR_NONE);
      bool saved_unbox = ctx->suppress_opt_unbox;
      if (none_compare) {
          const AstExpr* operand = (expr->as.binary.lhs->kind == AST_EXPR_NONE)
              ? expr->as.binary.rhs : expr->as.binary.lhs;
          ctx->suppress_opt_unbox = true;
          // An OPTIONAL REFERENCE is a nullable pointer, not a box (spec 4.1),
          // so its emptiness test is a null check rather than rae_any_is_none.
          const AstTypeRef* otr = infer_expr_type_ref(ctx, operand);
          if (otr && otr->is_opt && (otr->is_view || otr->is_mod)) {
              fprintf(out, "((bool)(");
              if (operand->kind == AST_EXPR_IDENT
                  && c_primitive_ref_has_wrapper(get_base_type_name(otr))) {
                  fprintf(out, "%.*s.ptr", (int)operand->as.ident.len,
                          operand->as.ident.data);
              } else {
                  emit_expr(ctx, operand, out, PREC_LOWEST, true, true);
              }
              fprintf(out, expr->as.binary.op == AST_BIN_NEQ ? " != NULL))" : " == NULL))");
              ctx->suppress_opt_unbox = saved_unbox;
              break;
          }
          // A struct-rep value optional is `struct rae_opt_<T>`: emptiness is
          // the `.has` flag, not rae_any_is_none (which is for the RaeAny box).
          if (otr && otr->is_opt && !(otr->is_view || otr->is_mod)
              && rae_opt_is_struct_rep(ctx, otr)) {
              fprintf(out, "((bool)((");
              emit_expr(ctx, operand, out, PREC_LOWEST, false, false);
              fprintf(out, ").has%s))",
                      expr->as.binary.op == AST_BIN_NEQ ? "" : " == 0");
              ctx->suppress_opt_unbox = saved_unbox;
              break;
          }
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
      // Special case: string equality — use rae_ext_rae_str_eq instead of ==
      // (and its negation for `is not`).
      if (expr->as.binary.op == AST_BIN_IS || expr->as.binary.op == AST_BIN_NEQ) {
          // #758: resolve a generic param (`T` in a `List(T)`/`view T` template)
          // to its concrete type, so `cur is value` with T=String takes the
          // rae_str_eq path instead of falling through to C `==` on two structs.
          Str lhs_base = eff_base_name(ctx, expr->as.binary.lhs);
          bool lhs_is_string = str_eq_cstr(lhs_base, "String") || str_eq_cstr(lhs_base, "rae_String");
          bool rhs_is_string_lit = expr->as.binary.rhs->kind == AST_EXPR_STRING;
          // Also detect toString() / toJson() calls — they always return String
          bool lhs_is_tostring = expr->as.binary.lhs->kind == AST_EXPR_METHOD_CALL &&
              (str_eq_cstr(expr->as.binary.lhs->as.method_call.method_name, "toString")
               || str_eq_cstr(expr->as.binary.lhs->as.method_call.method_name, "toJson"));
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
      // Struct value equality (#703): `a is b` / `a is not b` on a user value
      // struct is field-wise equality — C forbids `struct == struct`, so emit
      // the synthesized rae_eq_<T>. Only value-comparable structs (scalar/enum/
      // String/nested-value-struct fields) qualify; others fall through to `==`.
      if (expr->as.binary.op == AST_BIN_IS || expr->as.binary.op == AST_BIN_NEQ) {
          const AstTypeRef* ltr = infer_expr_type_ref(ctx, expr->as.binary.lhs);
          Str lbase = eff_base_name(ctx, expr->as.binary.lhs);
          // A `view`/`mod` struct operand is a pointer here; emit_expr
          // dereferences it to the value, which rae_eq_<T> takes by value.
          // `opt` is excluded (its emptiness is the none path above).
          if (ltr && !ltr->is_opt
              && ctx->module && rae_named_type_value_comparable(ctx->module, lbase)) {
              const char* m = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args,
                  &(AstTypeRef){.parts = &(AstIdentifierPart){.text = lbase}});
              fprintf(out, expr->as.binary.op == AST_BIN_NEQ ? "((bool)(!rae_eq_%s(" : "((bool)(rae_eq_%s(", m);
              emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
              fprintf(out, ", ");
              emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
              fprintf(out, ")))");
              break;
          }
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
          // #651: value (in)equality between TWO optionals of the same scalar
          // payload — `back.copyAt(i) is not data.copyAt(i)`. Neither side is a
          // bare primitive, so drive both to unbox to `.value` by expecting the
          // payload type. (A none-vs-value compare took the none_compare branch
          // above and never reaches here.)
          if (!picked && (expr->as.binary.op == AST_BIN_IS || expr->as.binary.op == AST_BIN_NEQ)) {
              const AstTypeRef* opt_side = (lhs_ti && lhs_ti->is_opt) ? lhs_ti
                                         : ((rhs_ti && rhs_ti->is_opt) ? rhs_ti : NULL);
              if (opt_side && !(opt_side->is_view || opt_side->is_mod)) {
                  AstTypeRef payload = *opt_side;
                  payload.is_opt = false; payload.is_view = false; payload.is_mod = false; payload.next = NULL;
                  Str b = get_base_type_name(&payload);
                  if (is_primitive_type(b) && !str_eq_cstr(b, "Any")) {
                      ctx->expected_type = payload; ctx->has_expected_type = true;
                  }
              }
          }
          // #817: an integer literal infers as plain `Int`; inside a UInt64-typed
          // context (`let y: UInt64 = 4294967295 * 4294967295`) that must not
          // demote the expectation, or the literals are emitted as int64 and the
          // arithmetic overflows before the assignment converts.
          if (picked && had_exp_bin && !saved_exp_bin.is_opt
              && str_eq_cstr(get_base_type_name(&saved_exp_bin), "UInt64")) {
              Str pb = get_base_type_name(picked);
              if (str_eq_cstr(pb, "Int") || str_eq_cstr(pb, "Int64")) picked = NULL;
          }
          if (picked) { ctx->expected_type = *picked; ctx->has_expected_type = true; }
      }
      int prec = binary_op_precedence(expr->as.binary.op); bool is_bool_op = expr->as.binary.op >= AST_BIN_LT && expr->as.binary.op <= AST_BIN_OR;
      /* A SHIFT parenthesises its operands further than C strictly requires.
       * `(a + b) shl 1` is correctly emitted as `a + b << 1` -- in C, `+`
       * binds tighter than `<<`, so the grouping survives -- but clang's
       * -Wshift-op-parentheses flags it, on the reasonable grounds that a
       * human reading `a + b << 1` will guess wrong. Every compiled example
       * that inflates or deflates printed two of these, and a warning nobody
       * can act on is a warning that hides real ones.
       *
       * Raising the operands' parent precedence to PREC_MUL adds parens
       * without changing meaning: parens are always semantically safe, and
       * the grouping being emitted is the AST's own. */
      bool is_shift_op = expr->as.binary.op == AST_BIN_SHL || expr->as.binary.op == AST_BIN_SHR;
      int operand_prec = is_shift_op ? PREC_MUL : prec;
      /* `+ - * / %` on Int (signed 64-bit) go through the runtime's checked
       * helpers (rae_runtime.h "Int arithmetic"): division by zero traps in
       * every profile, overflow traps in dev and wraps in release. Other
       * widths, UInt64 and floats keep the bare C operator. The type is the
       * one the expression settled on above (`picked`, when it is a
       * primitive); an untyped operand pair (rare — `Any`) stays bare. */
      {
        const char* int_helper = NULL;
        bool int_typed = ctx->has_expected_type && !ctx->expected_type.is_opt
                         && (str_eq_cstr(get_base_type_name(&ctx->expected_type), "Int")
                             || str_eq_cstr(get_base_type_name(&ctx->expected_type), "Int64"));
        if (int_typed && is_arith_or_cmp && !expr_is_const_int(expr)) {
          switch (expr->as.binary.op) {
            case AST_BIN_ADD: int_helper = "rae_int_add"; break;
            case AST_BIN_SUB: int_helper = "rae_int_sub"; break;
            case AST_BIN_MUL: int_helper = "rae_int_mul"; break;
            case AST_BIN_DIV: int_helper = "rae_int_div"; break;
            case AST_BIN_MOD: int_helper = "rae_int_mod"; break;
            default: break;
          }
        }
        if (int_helper) {
          fprintf(out, "%s(", int_helper);
          emit_expr(ctx, expr->as.binary.lhs, out, PREC_LOWEST, false, false);
          fprintf(out, ", ");
          emit_expr(ctx, expr->as.binary.rhs, out, PREC_LOWEST, false, false);
          fprintf(out, ", ");
          emit_c_source_location(ctx, expr, out);
          fprintf(out, ")");
          ctx->has_expected_type = had_exp_bin;
          ctx->expected_type = saved_exp_bin;
          ctx->suppress_opt_unbox = saved_unbox;
          break;
        }
      }
      if (is_bool_op) fprintf(out, "(bool)("); if (prec < parent_prec) fprintf(out, "(");
      emit_expr(ctx, expr->as.binary.lhs, out, operand_prec, false, false);
      switch (expr->as.binary.op) {
        case AST_BIN_ADD: fprintf(out, " + "); break; case AST_BIN_SUB: fprintf(out, " - "); break;
        case AST_BIN_MUL: fprintf(out, " * "); break; case AST_BIN_DIV: fprintf(out, " / "); break;
        case AST_BIN_MOD: fprintf(out, " %% "); break; case AST_BIN_LT: fprintf(out, " < "); break;
        case AST_BIN_GT: fprintf(out, " > "); break; case AST_BIN_LE: fprintf(out, " <= "); break;
        case AST_BIN_GE: fprintf(out, " >= "); break; case AST_BIN_IS: fprintf(out, " == "); break;
        case AST_BIN_NEQ: fprintf(out, " != "); break;
        case AST_BIN_AND: fprintf(out, " && "); break; case AST_BIN_OR: fprintf(out, " || "); break;
        case AST_BIN_BITAND: fprintf(out, " & "); break; case AST_BIN_BITOR: fprintf(out, " | "); break;
        case AST_BIN_BITXOR: fprintf(out, " ^ "); break;
        case AST_BIN_SHL: fprintf(out, " << "); break; case AST_BIN_SHR: fprintf(out, " >> "); break;
      }
      /* RHS gets prec + 1: Rae's binary operators are LEFT-associative,
       * so a right operand of EQUAL precedence must keep its parens —
       * `a - (b + c)` re-emitted as `a - b + c` silently flips signs
       * (the gpu3d mat4LookAt dot-product bug), `a / (b * c)` becomes
       * `(a / b) * c`. Equal-precedence LHS stays unparenthesized. */
      emit_expr(ctx, expr->as.binary.rhs, out, is_shift_op ? PREC_MUL : prec + 1, false, false);
      if (prec < parent_prec) fprintf(out, ")"); if (is_bool_op) fprintf(out, ")");
      ctx->has_expected_type = had_exp_bin;
      ctx->expected_type = saved_exp_bin;
      ctx->suppress_opt_unbox = saved_unbox;
      break;
    }
    case AST_EXPR_UNARY: {
        switch (expr->as.unary.op) {
            case AST_UNARY_NOT: fprintf(out, "((bool)!("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, "))"); break;
            case AST_UNARY_BITNOT: fprintf(out, "(~("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, "))"); break;
            case AST_UNARY_NEG: fprintf(out, "-("); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); fprintf(out, ")"); break;
            case AST_UNARY_VIEW: case AST_UNARY_MOD: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, false, false); break;
            case AST_UNARY_PRE_INC: fprintf(out, "++"); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); break;
            case AST_UNARY_PRE_DEC: fprintf(out, "--"); emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); break;
            case AST_UNARY_POST_INC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "++"); break;
            case AST_UNARY_POST_DEC: emit_expr(ctx, expr->as.unary.operand, out, PREC_UNARY, true, false); fprintf(out, "--"); break;
            case AST_UNARY_SPAWN: {
                const AstExpr* callexpr = expr->as.unary.operand;
                TypeInfo* resT = (expr->resolved_type && expr->resolved_type->kind == TYPE_TASK)
                                     ? expr->resolved_type->as.task.base : NULL;
                bool is_void = !resT || resT->kind == TYPE_VOID;
                const AstFuncDecl* callee =
                    (callexpr->kind == AST_EXPR_CALL && callexpr->decl_link &&
                     callexpr->decl_link->kind == AST_DECL_FUNC)
                        ? &callexpr->decl_link->as.func_decl : NULL;
                if (callee && c_spawn_threadable(ctx, callee)) {
                    // Real OS thread: pack args by value into the per-function
                    // struct and start the thunk (rae_task_start: a thread, or the
                    // caller when no thread can be had). get() joins it.
                    const char* mangled = rae_mangle_function(ctx->compiler_ctx, callee);
                    fprintf(out, "({ __raespawn_args_%s* __s = (__raespawn_args_%s*)malloc(sizeof(__raespawn_args_%s)); ",
                            mangled, mangled, mangled);
                    int k = 0;
                    const AstParam* pp = callee->params;
                    for (const AstCallArg* a = callexpr->as.call.args; a; a = a->next, k++) {
                        // Heap params are captured so the worker owns a stable
                        // heap independent of the parent (the parent keeps and
                        // drops its own originals). Scalars/enums copy by value.
                        bool str_param = pp && pp->type
                            && !pp->type->is_view && !pp->type->is_mod
                            && str_eq_cstr(get_base_type_name(pp->type), "String");
                        bool agg_param = pp && pp->type
                            && c_spawn_arg_deepcopy_aggregate(ctx, pp->type);
                        // A deep-copy only applies to aliasing lvalue args
                        // (IDENT/MEMBER/INDEX); a fresh rvalue (call/object)
                        // already owns its heap and just moves to the worker.
                        bool agg_lvalue = agg_param && a->value
                            && (a->value->kind == AST_EXPR_IDENT
                                || a->value->kind == AST_EXPR_MEMBER
                                || a->value->kind == AST_EXPR_INDEX);
                        // Drive type-directed emission (object literals,
                        // collection literals) from the param's declared type,
                        // exactly as a normal call arg does — without this an
                        // object-literal arg picks up a stale expected type and
                        // emits the wrong C compound-literal type.
                        bool saved_has_exp = ctx->has_expected_type;
                        AstTypeRef saved_exp = ctx->expected_type;
                        if (pp && pp->type) { ctx->expected_type = *pp->type; ctx->has_expected_type = true; }
                        fprintf(out, "__s->f%d = ", k);
                        if (str_param) {
                            fprintf(out, "rae_string_copy((");
                            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
                            fprintf(out, "))");
                        } else if (agg_lvalue) {
                            const char* tn = rae_mangle_type_specialized(
                                ctx->compiler_ctx, ctx->generic_params,
                                ctx->generic_args, pp->type);
                            int tid = ctx->temp_counter++;
                            fprintf(out, "(__extension__ ({ %s __cpy%d; rae_deep_copy_%s(&__cpy%d, ",
                                    tn, tid, tn, tid);
                            emit_deep_copy_source(ctx, a->value, out);
                            fprintf(out, "); __cpy%d; }))", tid);
                        } else {
                            fprintf(out, "(");
                            emit_expr(ctx, a->value, out, PREC_LOWEST, false, false);
                            fprintf(out, ")");
                        }
                        fprintf(out, "; ");
                        ctx->has_expected_type = saved_has_exp;
                        ctx->expected_type = saved_exp;
                        if (pp) pp = pp->next;
                    }
                    fprintf(out, "RaeTask* __t = rae_task_new(");
                    if (is_void) fprintf(out, "0");
                    else { fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, resT, out); fprintf(out, ")"); }
                    fprintf(out, "); __s->__task = __t; rae_task_start(__t, __raespawn_thunk_%s, __s); __t; })", mangled);
                    break;
                }
                // Sequential fallback (heap/mod/view-enum args, or an
                // unresolved callee): run synchronously into a completed task.
                fprintf(out, "({ RaeTask* __raet = rae_task_new(");
                if (is_void) fprintf(out, "0");
                else { fprintf(out, "sizeof("); emit_type_info_as_c_type(ctx, resT, out); fprintf(out, ")"); }
                fprintf(out, "); ");
                if (!is_void) { fprintf(out, "*("); emit_type_info_as_c_type(ctx, resT, out); fprintf(out, "*)__raet->result = "); }
                emit_expr(ctx, callexpr, out, PREC_LOWEST, false, false);
                fprintf(out, "; __raet->done = 1; __raet->joined = 1; __raet; })");
                break;
            }
            default: break;
        }
        break;
    }
    case AST_EXPR_CALL: emit_call_expr(ctx, expr, out); break;
    case AST_EXPR_METHOD_CALL: {
        // Built-in Task(T).get(): join (no-op for a sequential task) and
        // read the result buffer, cast back to T.
        if (str_eq_cstr(expr->as.method_call.method_name, "get") &&
            expr->as.method_call.object->resolved_type &&
            expr->as.method_call.object->resolved_type->kind == TYPE_TASK) {
            TypeInfo* resT = expr->as.method_call.object->resolved_type->as.task.base;
            if (!resT || resT->kind == TYPE_VOID) {
                fprintf(out, "(rae_task_await(");
                emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, false);
                fprintf(out, "), (void)0)");
            } else {
                /* A String result follows the String-return convention: it
                 * is registered in this statement's pool, so a binding
                 * claims it (rae_string_pool_take) and an unclaimed
                 * temporary (`log(task.get())`, boxed to Any) is freed at
                 * the statement's flush instead of leaking */
                bool string_result = resT->kind == TYPE_STRING;
                if (string_result) fprintf(out, "rae_string_pool_register_owned(");
                fprintf(out, "(*(");
                emit_type_info_as_c_type(ctx, resT, out);
                fprintf(out, "*)rae_task_await(");
                emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, false);
                fprintf(out, "))");
                if (string_result) fprintf(out, ")");
            }
            break;
        }
        // Built-in Task(T).isDone() / tryGet() (docs/concurrency-model.md).
        // isDone reads the worker's atomic done flag; tryGet hands the result
        // out once — claim, join (instant: the worker has finished), move the
        // value into `opt T` — and is none otherwise. A `view`/`mod` Task is
        // the same RaeTask* (c_backend.c), so a reference unwraps for free.
        {
            const TypeInfo* task_type = expr->as.method_call.object->resolved_type;
            // A `view`/`mod` Task parameter reads as `(*task)`: the RaeTask*.
            if (task_type && task_type->kind == TYPE_REF) task_type = task_type->as.ref.base;
            bool is_done = str_eq_cstr(expr->as.method_call.method_name, "isDone");
            bool try_get = str_eq_cstr(expr->as.method_call.method_name, "tryGet");
            if (task_type && task_type->kind == TYPE_TASK && (is_done || try_get)) {
                if (is_done) {
                    fprintf(out, "((rae_Bool)rae_task_is_done((");
                    emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, false);
                    fprintf(out, ")))");
                    break;
                }
                TypeInfo* resT = task_type->as.task.base;
                const AstTypeRef* task_ref = infer_expr_type_ref(ctx, expr->as.method_call.object);
                AstTypeRef opt_ref = {0};
                if (task_ref && task_ref->generic_args) {
                    opt_ref = *task_ref->generic_args;
                    opt_ref.next = NULL;
                }
                opt_ref.is_opt = true; opt_ref.is_view = false; opt_ref.is_mod = false;
                opt_ref.resolved_type = expr->resolved_type;
                int id = ctx->temp_counter++;
                fprintf(out, "(__extension__ ({ RaeTask* __tt%d = (", id);
                emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, false);
                fprintf(out, "); %s __tr%d = {0}; if (rae_task_claim(__tt%d)) { __tr%d.has = 1; __tr%d.value = *(",
                        rae_opt_type_name(ctx, &opt_ref), id, id, id, id);
                emit_type_info_as_c_type(ctx, resT, out);
                fprintf(out, "*)rae_task_await(__tt%d); } __tr%d; }))", id, id);
                break;
            }
        }
        // Built-in method: toString() → rae_ext_rae_str(object) or
        // rae_to_str_TYPE_(&object) for user structs.
        if (str_eq_cstr(expr->as.method_call.method_name, "toString") && !expr->as.method_call.args) {
            emit_to_string_expr(ctx, expr->as.method_call.object, out);
            break;
        }
        // An `unsafe` List write in a parallel body (docs/parallel-stages-
        // design.md §3): report the element first, then the write itself.
        if (ctx->parallel_unsafe_depth > 0 && ctx->parallel_check_emitting != expr
            && (str_eq_cstr(expr->as.method_call.method_name, "set")
                || str_eq_cstr(expr->as.method_call.method_name, "modAt"))) {
            const AstTypeRef* list_type = infer_expr_type_ref(ctx, expr->as.method_call.object);
            const AstCallArg* index_arg = expr->as.method_call.args;
            while (index_arg && !str_eq_cstr(index_arg->name, "index")) index_arg = index_arg->next;
            if (list_type && str_eq_cstr(get_base_type_name(list_type), "List") && index_arg) {
                const AstExpr* object = expr->as.method_call.object;
                Str root = object->kind == AST_EXPR_IDENT ? object->as.ident : str_from_cstr("a List");
                // A view/mod List is a pointer in C, a local List a struct.
                bool list_is_ref = list_type->is_view || list_type->is_mod;
                fprintf(out, "(rae_parallel_check_write((const void*)(");
                emit_expr(ctx, object, out, PREC_LOWEST, false, false);
                fprintf(out, ")%sdata, (int64_t)(", list_is_ref ? "->" : ".");
                emit_expr(ctx, index_arg->value, out, PREC_LOWEST, false, false);
                fprintf(out, "), (int64_t)(%.*s), \"%.*s\"), ", (int)ctx->parallel_index.len,
                        ctx->parallel_index.data, (int)root.len, root.data);
                const void* saved = ctx->parallel_check_emitting;
                ctx->parallel_check_emitting = expr;
                emit_expr(ctx, expr, out, PREC_LOWEST, false, false);
                ctx->parallel_check_emitting = saved;
                fprintf(out, ")");
                break;
            }
        }
        if (emit_list_fast_access(ctx, expr, out)) break;
        // Built-in method: toJson() → rae_toJson_TYPE_(&object)
        if (str_eq_cstr(expr->as.method_call.method_name, "toJson") && !expr->as.method_call.args) {
            const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.method_call.object);
            const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, obj_tr);
            if (obj_tr && (obj_tr->is_view || obj_tr->is_mod)) {
                // A view/mod receiver already IS a pointer to the value: pass it
                // as is. Taking its address passed a pointer-to-pointer, and the
                // serializer printed the address as the field values (a view of
                // a generic T in lib/ecs/Serialize, #32786425 follow-up).
                fprintf(out, "rae_toJson_%s_((%s*)(", mangled, mangled);
                emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, false, true);
                fprintf(out, "))");
                break;
            }
            fprintf(out, "rae_toJson_%s_(&", mangled);
            emit_expr(ctx, expr->as.method_call.object, out, PREC_LOWEST, true, false);
            fprintf(out, ")");
            break;
        }
        // Built-in static method (#959): Type.default() → the zero value of the
        // type, `((rae_TYPE){0})`: every field zero / false / empty String /
        // empty List / first enum case / none-opt — the same starting point
        // fromJson fills. One opaque value primitive (not field-by-field
        // construction reflection; #774). Substituted through the enclosing
        // generic context like fromJson, so `T.default()` inside a function
        // generic over T names the concrete instantiation.
        if (str_eq_cstr(expr->as.method_call.method_name, "default")
            && !expr->as.method_call.args
            && expr->as.method_call.object->kind == AST_EXPR_IDENT) {
            Str type_name = expr->as.method_call.object->as.ident;
            AstTypeRef tmp = {0}; AstIdentifierPart part = {0}; part.text = type_name; tmp.parts = &part;
            const AstTypeRef* sub = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &tmp);
            const AstTypeRef* eff = sub ? sub : &tmp;
            // #961: a non-generic user struct has a generated default that
            // honours declared field defaults; everything else is the zero
            // value, through the C type printer so builtins (Int, String,
            // List(T)) spell correctly.
            Str effbase = get_base_type_name(eff);
            bool user_struct = false;
            if (!eff->generic_args && !eff->is_opt) {
                const AstDecl* td = NULL;
                for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                    const AstDecl* dd = ctx->compiler_ctx->all_decls[i];
                    if (dd->kind == AST_DECL_TYPE && !dd->as.type_decl.generic_params
                        && !dd->as.type_decl.specialization_args
                        && !has_property(dd->as.type_decl.properties, "c_struct")
                        && str_eq(dd->as.type_decl.name, effbase)) { td = dd; break; }
                }
                user_struct = td != NULL && !find_enum_decl(ctx, ctx->module, effbase);
            }
            if (user_struct) {
                fprintf(out, "rae_default_%s_()", rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, (AstTypeRef*)eff));
            } else {
                fprintf(out, "((");
                emit_type_ref_as_c_type(ctx, eff, out, false);
                fprintf(out, "){0})");
            }
            break;
        }
        // Built-in static method: Type.fromJson(json: str) → rae_fromJson_TYPE_(str)
        if (str_eq_cstr(expr->as.method_call.method_name, "fromJson")) {
            Str type_name = {0};
            if (expr->as.method_call.object->kind == AST_EXPR_IDENT) type_name = expr->as.method_call.object->as.ident;
            if (type_name.len > 0) {
                // Substitute the type name through the enclosing generic context
                // (like toJson above), so `T.fromJson` inside a function generic
                // over T mangles to the concrete instantiation instead of the
                // literal `rae_T`. A non-generic `Pos.fromJson` is unchanged.
                AstTypeRef tmp = {0}; AstIdentifierPart part = {0}; part.text = type_name; tmp.parts = &part;
                const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &tmp);
                fprintf(out, "rae_fromJson_%s_(", mangled);
                if (expr->as.method_call.args) emit_expr(ctx, expr->as.method_call.args->value, out, PREC_LOWEST, false, false);
                fprintf(out, ")");
                break;
            }
            // A generic instance spelled as sema reads the qualifier
            // (sema_type_qualifier_type): `Pair(Int).fromJson(json:)`, a call
            // whose callee and arguments are type names.
            const AstExpr* q = expr->as.method_call.object;
            if (q->kind == AST_EXPR_CALL && q->as.call.callee && q->as.call.callee->kind == AST_EXPR_IDENT
                && q->as.call.args) {
                AstTypeRef tmp = {0}; AstIdentifierPart part = {0};
                part.text = q->as.call.callee->as.ident; tmp.parts = &part;
                AstTypeRef* tail = NULL;
                bool all_types = true;
                for (const AstCallArg* ta = q->as.call.args; ta; ta = ta->next) {
                    if (!ta->value || ta->value->kind != AST_EXPR_IDENT || ta->name.len > 0) { all_types = false; break; }
                    AstTypeRef* ga = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstTypeRef));
                    memset(ga, 0, sizeof(*ga));
                    ga->parts = arena_alloc(ctx->compiler_ctx->ast_arena, sizeof(AstIdentifierPart));
                    memset(ga->parts, 0, sizeof(*ga->parts));
                    ga->parts->text = ta->value->as.ident;
                    if (!tmp.generic_args) tmp.generic_args = ga; else tail->next = ga;
                    tail = ga;
                }
                if (all_types && generic_struct_template(ctx->compiler_ctx, &tmp)) {
                    const char* mangled = rae_mangle_type_specialized(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, &tmp);
                    fprintf(out, "rae_fromJson_%s_(", mangled);
                    if (expr->as.method_call.args) emit_expr(ctx, expr->as.method_call.args->value, out, PREC_LOWEST, false, false);
                    fprintf(out, ")");
                    break;
                }
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
                (is_primitive_type(obj_name) || str_eq_cstr(obj_name, "Ptr")
                 || (ctx->module && find_type_decl(ctx, ctx->module, obj_name) != NULL));
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
        // #32786425 / #45158908: a field chain read off a call's OWNED result —
        // `componentCopyAtDefault(this: t, i: i).rows.length`,
        // `makeWrapper().inner.label` — used to leak that whole result (a deep
        // copy) every time: nothing ever dropped it. The call is captured in a
        // statement temporary and the chain is read off it as a place (see
        // emit_member_chain_on_temp), so the statement's drop list releases
        // the result afterwards, whatever the chain ends in. A heap-valued
        // field is safe because every consumer already treats a member read
        // as a LOCATION: `let`/`=` copy it out, an argument borrows it only
        // for the call, `own` moves it out and zeroes the slot. (A `view`/`mod`
        // BINDING to it would outlive the temporary; sema rejects that.)
        if (ctx->stmt_temps) {
            const AstExpr* base = expr->as.member.object;
            while (base->kind == AST_EXPR_MEMBER) base = base->as.member.object;
            // The buffer-get intrinsics are typed as returning the element by
            // value but are LOWERED to a dereference of the buffer's storage (a
            // place, c_call.c is_buf_get): `ret view rae_ext_rae_buf_get(buf:
            // this.data, index: idx).value` in the hash maps aliases the map's
            // own slot. Capturing that in a temporary would hand back a pointer
            // into a copy that is dropped at the end of the statement.
            bool base_is_storage_place = base->kind == AST_EXPR_CALL && base->as.call.callee
                && base->as.call.callee->kind == AST_EXPR_IDENT
                && (str_eq_cstr(base->as.call.callee->as.ident, "rae_ext_rae_buf_get")
                    || str_eq_cstr(base->as.call.callee->as.ident, "rae_ext___buf_get")
                    || str_eq_cstr(base->as.call.callee->as.ident, "__buf_get"));
            const AstTypeRef* base_tr = !base_is_storage_place
                && (base->kind == AST_EXPR_CALL || base->kind == AST_EXPR_METHOD_CALL)
                ? infer_expr_type_ref(ctx, base) : NULL;
            bool base_is_owned = base_tr && !base_tr->is_view && !base_tr->is_mod && !base_tr->is_opt
                && type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, base_tr, 0);
            if (base_is_owned) {
                const AstTypeRef* base_concrete = base_tr;
                if (ctx->generic_params && ctx->generic_args)
                    base_concrete = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, base_tr);
                int tmp_id = register_stmt_temp(ctx, base_concrete, true);
                if (tmp_id >= 0) {
                    emit_member_chain_on_temp(ctx, expr, base, tmp_id, out);
                    break;
                }
            }
        }
        const AstTypeRef* obj_tr = infer_expr_type_ref(ctx, expr->as.member.object);
        bool use_arrow = (obj_tr && (obj_tr->is_view || obj_tr->is_mod));
        // A call that RETURNS a reference (`ret view T` / `ret mod T`) yields a
        // pointer even when inference lost that — e.g. the explicit-type
        // spelling `componentView(Rect, this: t, entityId: e).position`.
        if (!use_arrow && expr->as.member.object->kind == AST_EXPR_CALL
            && expr->as.member.object->decl_link
            && expr->as.member.object->decl_link->kind == AST_DECL_FUNC) {
            const AstFuncDecl* cfd = &expr->as.member.object->decl_link->as.func_decl;
            if (cfd->returns && cfd->returns->type
                && (cfd->returns->type->is_view || cfd->returns->type->is_mod)) use_arrow = true;
        }
        // The explicit-type spelling is rewritten before emission and can reach
        // here unresolved: fall back to the callee NAME when every function of
        // that name returns a reference.
        if (!use_arrow && expr->as.member.object->kind == AST_EXPR_CALL
            && !expr->as.member.object->decl_link
            && expr->as.member.object->as.call.callee
            && expr->as.member.object->as.call.callee->kind == AST_EXPR_IDENT) {
            Str cname = expr->as.member.object->as.call.callee->as.ident;
            bool any = false, all_ref = true;
            for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
                const AstDecl* d = ctx->compiler_ctx->all_decls[i];
                if (d->kind != AST_DECL_FUNC || !str_eq(d->as.func_decl.name, cname)) continue;
                any = true;
                const AstFuncDecl* cfd = &d->as.func_decl;
                if (!(cfd->returns && cfd->returns->type
                      && (cfd->returns->type->is_view || cfd->returns->type->is_mod))) all_ref = false;
            }
            if (any && all_ref) use_arrow = true;
        }
        /* The expected type belongs to the FIELD, not to the object: a
         * generic call as the object (`points.copyAtFallback(...).x` passed
         * as an Int argument) must not re-infer its T from that Int. */
        bool member_saved_has_exp = ctx->has_expected_type;
        ctx->has_expected_type = false;
        emit_expr(ctx, expr->as.member.object, out, PREC_CALL, true, false);
        ctx->has_expected_type = member_saved_has_exp;
        Str fld = c_struct_field_c_name(expr->as.member.member, type_ref_is_c_struct(ctx, obj_tr));
        fprintf(out, "%s%.*s", use_arrow ? "->" : ".", (int)fld.len, fld.data);
        break;
    }
    case AST_EXPR_INDEX: {
        // List(T) is a struct, not a raw array; lower `xs[i]` to a typed
        // buffer access on the data pointer.
        /* No List branch: sema rejects `xs[i]` on a List and points at
         * .get(index:) / .at(index:), so nothing reaches codegen. `[]` is
         * for Array(T, cap: N) and the internal Buffer. */
        const AstTypeRef* tgt_tr = infer_expr_type_ref(ctx, expr->as.index.target);
        Str tgt_base = get_base_type_name(tgt_tr);
        (void)tgt_base;
        /* Array(T, cap: N) is a struct wrapping T[N] (see §1.5), so indexing
         * goes through the `.v` member. A view/mod parameter is a pointer to
         * that struct, hence `->`. */
        {
            /* Prefer the target's own resolved type: for a nested access
             * like grid[1][0] the inner `grid[1]` is an INDEX expression
             * whose element type sema already recorded, and which
             * infer_expr_type_ref does not reconstruct. */
            const TypeInfo* ti = expr->as.index.target->resolved_type;
            if (!ti && tgt_tr) ti = tgt_tr->resolved_type;
            if (ti && ti->kind == TYPE_REF) ti = ti->as.ref.base;
            if ((ti && ti->kind == TYPE_ARRAY) || str_eq_cstr(tgt_base, "Array")) {
                /* Always `.v` — emit_expr already dereferences a view/mod
                 * parameter, so the target is a value here, never a pointer. */
                emit_expr(ctx, expr->as.index.target, out, PREC_CALL, false, false);
                fprintf(out, ".v[");
                /* The raw slot: sema admits an Array subscript only inside
                 * `unsafe` (the synthesized core/Array wrappers, which
                 * bounds-check before reaching it), so no runtime check here. */
                emit_expr(ctx, expr->as.index.index, out, PREC_LOWEST, false, false);
                fprintf(out, "]");
                break;
            }
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
        //
        // #965: a move out of a PLACE inside a live owner (`own this.events`,
        // `own item.list`) has no such bit — the owner keeps living and will
        // release that field again at its own drop, and a whole-value store
        // into it now releases the previous value first (c_stmt.c). So the
        // move must leave the source EMPTY: read the value out, zero the
        // place (a zeroed String / List / Map / struct is the valid empty
        // value every drop path treats as "nothing to free"), yield the
        // value. This is what makes the swap idiom (`var t = own a.x; a.x =
        // own a.y; a.y = own t`) both leak-free and double-free-free.
        // An optional place is moved the same way: a zeroed opt is `none`
        // (has = 0). Copying it without zeroing left the source still owning
        // the value, so the next store into the source (or the owner's drop)
        // freed what the destination had just received.
        {
            const AstExpr* place = expr->as.unary.operand;
            if (place && (place->kind == AST_EXPR_MEMBER || place->kind == AST_EXPR_INDEX)) {
                const AstTypeRef* ptr = infer_expr_type_ref(ctx, place);
                if (ptr && !ptr->is_view && !ptr->is_mod
                    && (type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, ptr, 0)
                        || str_eq_cstr(get_base_type_name(ptr), "String"))) {
                    int mvn = ctx->temp_counter++;
                    fprintf(out, "(__extension__ ({ ");
                    emit_type_ref_as_c_type(ctx, ptr, out, false);
                    fprintf(out, "* __rae_mvp%d = &(", mvn);
                    emit_expr(ctx, place, out, PREC_LOWEST, true, false);
                    fprintf(out, "); ");
                    emit_type_ref_as_c_type(ctx, ptr, out, false);
                    fprintf(out, " __rae_mv%d = *__rae_mvp%d; memset(__rae_mvp%d, 0, sizeof(*__rae_mvp%d)); __rae_mv%d; }))",
                            mvn, mvn, mvn, mvn, mvn);
                    break;
                }
            }
            /* The same for a `mod` parameter or alias (`own queries` where
             * `queries: mod List(Int)`): the value lives in the caller, so the
             * move reads it through the pointer and leaves the caller's place
             * empty. Emitted as a plain identifier it was the pointer itself,
             * which C rejected as a value. */
            if (place && place->kind == AST_EXPR_IDENT) {
                const AstTypeRef* ptr = infer_expr_type_ref(ctx, place);
                if (ptr && ptr->is_mod && !ptr->is_opt) {
                    AstTypeRef value_type = *ptr;
                    value_type.is_mod = false;
                    int mvn = ctx->temp_counter++;
                    fprintf(out, "(__extension__ ({ ");
                    emit_type_ref_as_c_type(ctx, &value_type, out, false);
                    fprintf(out, "* __rae_mvp%d = ", mvn);
                    emit_expr(ctx, place, out, PREC_LOWEST, false, true);
                    fprintf(out, "; ");
                    emit_type_ref_as_c_type(ctx, &value_type, out, false);
                    fprintf(out, " __rae_mv%d = *__rae_mvp%d; memset(__rae_mvp%d, 0, sizeof(*__rae_mvp%d)); __rae_mv%d; }))",
                            mvn, mvn, mvn, mvn, mvn);
                    break;
                }
            }
        }
        {
            // A local moved on some paths only clears its live flag as the
            // move is evaluated (#55453128); every other local is marked moved.
            const AstExpr* moved = expr->as.unary.operand;
            int li = (moved && moved->kind == AST_EXPR_IDENT)
                ? local_index_by_name(ctx, moved->as.ident) : -1;
            if (li >= 0 && ctx->local_drop_flag[li] && !is_lvalue) {
                fprintf(out, "(__rae_live_%.*s = 0, ", (int)moved->as.ident.len, moved->as.ident.data);
                emit_expr(ctx, moved, out, PREC_LOWEST, is_lvalue, suppress_deref);
                fprintf(out, ")");
                break;
            }
        }
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
                // The unbox result may be typed as the payload directly, or (in
                // ref/`if let` narrowing) still as the `opt` itself — unwrap so
                // the representation decision looks at the payload either way.
                const TypeInfo* pay = (t->kind == TYPE_OPT) ? t->as.opt.base : t;
                fprintf(out, "("); emit_expr(ctx, expr->as.unary.operand, out, PREC_LOWEST, false, false);
                // #651: every non-`Any` opt payload is struct-rep -> unbox is
                // `.value`. Check struct-rep FIRST so scalars/String (now
                // struct-rep) read `.value`, not the removed RaeAny `.as.*`.
                if (rae_typeinfo_opt_is_struct_rep(pay)) fprintf(out, ").value"); else if (pay->kind == TYPE_INT || pay->kind == TYPE_CHAR) fprintf(out, ").as.i"); else if (pay->kind == TYPE_FLOAT || pay->kind == TYPE_FLOAT64) fprintf(out, ").as.f"); else if (pay->kind == TYPE_BOOL) fprintf(out, ").as.b"); else if (pay->kind == TYPE_STRING) fprintf(out, ").as.s"); else fprintf(out, ").as.ptr");
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
        // A COPY of the expected type, not a pointer to it: the field loop
        // below overwrites ctx->expected_type with each field's type, so a
        // pointer made every field after the first substitute its generic
        // arguments from the previous FIELD's type — in `Pair(Pair(Int))`,
        // `second: { ... }` was typed with Pair(Int)'s argument, Int.
        AstTypeRef obj_expected;
        if (!obj_tr && ctx->has_expected_type) {
            obj_expected = ctx->expected_type;
            obj_tr = &obj_expected;
        }
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
            // A compound literal is always a VALUE, never a pointer — even when
            // the expected type is a `view`/`mod` param (pointer). The caller's
            // address-of-temp wrapper (`(T[1]){ ... }` in c_call.c) provides the
            // pointer, so strip view/mod here or we'd emit an invalid
            // `(rae_T*){ .field = ... }`.
            AstTypeRef exp_val = ctx->expected_type;
            exp_val.is_view = false;
            exp_val.is_mod = false;
            fprintf(out, "(");
            emit_type_ref_as_c_type(ctx, &exp_val, out, false);
            fprintf(out, ")");
        }
        fprintf(out, "{ ");
        bool saved_has_exp = ctx->has_expected_type;
        AstTypeRef saved_exp = ctx->expected_type;
        bool lit_is_c_struct = struct_decl && struct_decl->kind == AST_DECL_TYPE
            && has_property(struct_decl->as.type_decl.properties, "c_struct");
        for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
            Str litfld = c_struct_field_c_name(f->name, lit_is_c_struct);
            fprintf(out, ".%.*s = ", (int)litfld.len, litfld.data);
            // Look up the field's declared type and use it as expected_type.
            const AstTypeRef* field_tr = NULL;
            if (struct_decl && struct_decl->kind == AST_DECL_TYPE) {
                for (const AstTypeField* td = struct_decl->as.type_decl.fields; td; td = td->next) {
                    if (str_eq(td->name, f->name)) { field_tr = td->type; break; }
                }
            }
            // Substitute the field's declared type with the concrete generic
            // arguments so ALL ownership/drop decisions below see the REAL
            // field type: `Wrapper(String).value` is a `String`, not the
            // generic `T`. Without this an owned-String generic field was
            // treated as raw `T`, so the move-into-field went untracked and the
            // owned source was freed twice -> double-free / malloc abort (#220).
            const AstTypeRef* eff_field_tr = field_tr;
            if (field_tr && struct_decl && struct_decl->kind == AST_DECL_TYPE) {
                eff_field_tr = substitute_type_ref(ctx->compiler_ctx,
                    struct_decl->as.type_decl.generic_params,
                    obj_tr ? obj_tr->generic_args : NULL, field_tr);
            }
            // Inside a specialization the literal's own arguments may still be
            // the function's type parameters (`let box: Box(V) = { value:
            // value }`): resolve those too, so `value` is the concrete Holder
            // and gets the deep copy `=` promises instead of a shallow alias.
            if (eff_field_tr && ctx->generic_params && ctx->generic_args)
                eff_field_tr = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params,
                    ctx->generic_args, eff_field_tr);
            if (eff_field_tr) {
                // An `opt T` FIELD is a RaeAny and needs the same boxing an
                // `opt T` return gets. Without this the raw payload was
                // assigned straight into the box -- `.nowPlaying = (rae_Track){…}`
                // into a RaeAny field, which does not compile.
                if (eff_field_tr->is_opt && !(eff_field_tr->is_view || eff_field_tr->is_mod)
                    && f->value && f->value->kind != AST_EXPR_NONE) {
                    emit_optional_boxed_expr(ctx, (AstTypeRef*)eff_field_tr, f->value, out);
                    if (f->next) fprintf(out, ", ");
                    continue;
                }
                ctx->expected_type = *eff_field_tr;
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
            if (eff_field_tr && !eff_field_tr->is_mod && !eff_field_tr->is_opt) {
                Str fbase = get_base_type_name(eff_field_tr);
                if (str_eq_cstr(fbase, "String")) {
                    if (eff_field_tr->is_view) field_is_view_string = true;
                    else                       field_is_owned_string = true;
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
                        /* Stage 3a — the move-when-safe path is only safe
                         * for OWNING parameters. A `view T` / `mod T`
                         * param is a borrow; the caller still owns the
                         * underlying heap, so transferring it into the
                         * field would cause a double-free at the
                         * caller's scope-exit (see the mobile UI
                         * `setText(text: view String)` repro). Restrict
                         * the move to params whose declared type has
                         * NO view/mod modifier. */
                        const AstTypeRef* ptr = ctx->local_type_refs[li];
                        bool is_owning_param =
                            ptr && !ptr->is_view && !ptr->is_mod;
                        if (is_owning_param) {
                            int n = rae_func_count_param_refs(ctx->func_decl, name);
                            if (n == 1) rhs_is_param_ident = true;
                        }
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
            // #798: MOVE a bare owning LOCAL into an owning field when THIS object
            // literal is the function's return value (`ret Struct { f: local }`).
            // In return position the local is at its last use, so transferring
            // ownership (bare emit + mark moved, no scope-exit drop) is correct and
            // skips the deep-copy — whose synthesised helper silently shallow-copies
            // a nested un-deep-copyable field (e.g. a ComponentTable inside a
            // World3d), then double-frees against the local's drop. Guarded to this
            // exact literal, an owning (non view/mod) local, no `own`, and a single
            // occurrence across the literal's fields (two fields aliasing the same
            // heap would double-free the returned value on its own drop).
            bool rhs_is_move_local = false;
            if (ctx->return_value_expr == expr
                && f->value && f->value->kind == AST_EXPR_IDENT
                && eff_field_tr && !eff_field_tr->is_view && !eff_field_tr->is_mod && !eff_field_tr->is_opt
                && !field_is_owned_string && !field_is_view_string) {
                Str mv = f->value->as.ident;
                size_t start = ctx->func_first_let_idx;
                if (start != (size_t)-1) {
                    for (size_t li = start; li < ctx->local_count; li++) {
                        if (str_eq(ctx->locals[li], mv)) {
                            const AstTypeRef* lt = ctx->local_type_refs[li];
                            if (lt && !lt->is_view && !lt->is_mod) {
                                int occ = 0;
                                for (const AstObjectField* g = expr->as.object_literal.fields; g; g = g->next)
                                    if (g->value && g->value->kind == AST_EXPR_IDENT
                                        && str_eq(g->value->as.ident, mv)) occ++;
                                if (occ == 1) rhs_is_move_local = true;
                            }
                            break;
                        }
                    }
                }
            }
            bool field_needs_deep_copy_nonstring = false;
            if (eff_field_tr
                && !eff_field_tr->is_view && !eff_field_tr->is_mod && !eff_field_tr->is_opt
                && !field_is_owned_string && !field_is_view_string
                && rhs_is_aliasing && !rhs_is_own && !rhs_is_move_local
                && type_needs_deep_copy(ctx->compiler_ctx, ctx->module,
                                        (AstTypeRef*)eff_field_tr, 0)) {
                Str fbase = get_base_type_name(eff_field_tr);
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
                    ctx->generic_args, (AstTypeRef*)eff_field_tr);
                int tmp_id = ctx->temp_counter++;
                fprintf(out, "(__extension__ ({ %s __fdc%d; rae_deep_copy_%s(&__fdc%d, ",
                        tn_dc, tmp_id, tn_dc, tmp_id);
                emit_deep_copy_source(ctx, f->value, out);
                fprintf(out, "); __fdc%d; }))", tmp_id);
            } else if (rhs_is_move_local) {
                // #798: transfer ownership — emit the bare local (shallow byte
                // copy of the struct value) and mark it moved so the return
                // epilogue's implicit-drop pass skips it. The returned struct now
                // solely owns the heap; no deep-copy, no double-free.
                emit_expr(ctx, f->value, out, PREC_LOWEST, false, false);
                mark_local_moved_by_name(ctx, f->value->as.ident);
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
                if (eff_field_tr && !eff_field_tr->is_view && !eff_field_tr->is_mod
                    && f->value && f->value->kind == AST_EXPR_IDENT) {
                    const AstTypeRef* rhs_tr = infer_expr_type_ref(ctx, f->value);
                    if (rhs_tr && (rhs_tr->is_view || rhs_tr->is_mod)
                        && !is_primitive_ref(ctx, rhs_tr)) {
                        // Stage 6: view-on-numeric-primitive is a value at
                        // the C level, not a pointer — no deref needed.
                        Str rhs_base = get_base_type_name(rhs_tr);
                        bool rhs_is_num_prim = is_scalar_primitive_type(rhs_base);
                        if (!rhs_is_num_prim) needs_view_deref = true;
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
    case AST_EXPR_COLLECTION_LITERAL: {
        // Unlike a binding initializer, a returned struct field needs a C
        // expression. Build an owning List in a statement expression and move
        // its value into the field; the enclosing struct owns its cleanup.
        // The expected type comes from the declared field, never its elements.
        bool savedHasExpected = ctx->has_expected_type;
        AstTypeRef savedExpected = ctx->expected_type;
        // The literal's own written type (`ret List(Point) { ... }`) wins;
        // otherwise the declared type of the place it initializes. Rae never
        // takes it from anywhere else (no inference).
        const AstTypeRef* writtenType = expr->as.collection.type;
        const AstTypeRef* listType = writtenType
            ? substitute_type_ref(ctx->compiler_ctx, ctx->generic_params,
                                  ctx->generic_args, (AstTypeRef*)writtenType)
            : savedHasExpected
            ? substitute_type_ref(ctx->compiler_ctx, ctx->generic_params,
                                  ctx->generic_args, &savedExpected) : NULL;
        const AstTypeRef* elementType = listType ? listType->generic_args : NULL;
        const AstFuncDecl* createFunction = NULL;
        const AstFuncDecl* addFunction = NULL;
        for (size_t i = 0; i < ctx->compiler_ctx->all_decl_count; i++) {
            const AstDecl* declaration = ctx->compiler_ctx->all_decls[i];
            if (declaration->kind != AST_DECL_FUNC) continue;
            const AstFuncDecl* function = &declaration->as.func_decl;
            if (!function->generic_params) continue;
            if (str_eq_cstr(function->name, "createList")) createFunction = function;
            if (str_eq_cstr(function->name, "add")) addFunction = function;
        }
        if (!elementType || !createFunction || !addFunction) {
            diag_error(ctx->module ? ctx->module->file_path : "<unknown>",
                       expr->line, expr->column,
                       "a list literal here needs its type written on it: `List(T) { ... }` "
                       "(e.g. `ret List(Point) { ... }`); Rae does not infer it");
            break;
        }
        register_generic_type(ctx->compiler_ctx, listType);
        register_function_specialization(ctx->compiler_ctx, createFunction, elementType);
        register_function_specialization(ctx->compiler_ctx, addFunction, elementType);
        const char* createName = rae_mangle_specialized_function(ctx->compiler_ctx, createFunction, elementType);
        const char* addName = rae_mangle_specialized_function(ctx->compiler_ctx, addFunction, elementType);
        int count = 0;
        for (const AstCollectionElement* element = expr->as.collection.elements; element; element = element->next) count++;
        int temporaryId = ctx->temp_counter++;
        fprintf(out, "({ ");
        AstTypeRef valueType = *listType;
        valueType.is_view = false;
        valueType.is_mod = false;
        emit_type_ref_as_c_type(ctx, &valueType, out, false);
        fprintf(out, " __collection%d = %s(((int64_t)%dLL)); ", temporaryId, createName, count);
        Str elementBase = get_base_type_name(elementType);
        bool elementIsAny = str_eq_cstr(elementBase, "Any") || str_eq_cstr(elementBase, "RaeAny");
        // As the `let` lowering does: a String element moves into the list,
        // so it is detached from the statement's string pool (an inner
        // `{ "a {x}" }` otherwise kept an interpolation the pool flush freed),
        // and a heap-owning local element is moved, not also dropped.
        bool elementIsString = !elementIsAny && str_eq_cstr(elementBase, "String")
            && !elementType->is_view && !elementType->is_mod && !elementType->is_opt;
        bool elementOwnsHeap = type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, elementType, 0);
        for (const AstCollectionElement* element = expr->as.collection.elements; element; element = element->next) {
            ctx->has_expected_type = true;
            ctx->expected_type = *elementType;
            fprintf(out, "%s(&__collection%d, ", addName, temporaryId);
            if (elementIsAny) fprintf(out, "rae_any((");
            if (elementIsString) fprintf(out, "rae_string_pool_take(");
            emit_expr(ctx, element->value, out, PREC_LOWEST, false, false);
            if (elementIsString) fprintf(out, ")");
            if (elementIsAny) fprintf(out, "))");
            fprintf(out, "); ");
            if (element->value && element->value->kind == AST_EXPR_IDENT && elementOwnsHeap)
                mark_expr_moved_if_local(ctx, element->value);
        }
        fprintf(out, "__collection%d; })", temporaryId);
        ctx->has_expected_type = savedHasExpected;
        ctx->expected_type = savedExpected;
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
    case AST_EXPR_NONE:
        // `none` for an optional REFERENCE is a null pointer; for an optional
        // value it is the empty box. The expected type decides which.
        if (ctx->has_expected_type && ctx->expected_type.is_opt
            && (ctx->expected_type.is_view || ctx->expected_type.is_mod)) {
            fprintf(out, "NULL");
        } else if (ctx->has_expected_type && ctx->expected_type.is_opt
                   && rae_opt_is_struct_rep(ctx, &ctx->expected_type)) {
            fprintf(out, "(%s){0}", rae_opt_type_name(ctx, &ctx->expected_type));
        } else {
            fprintf(out, "rae_any_none()");
        }
        break;
    default: break;
  }
  return true;
}
