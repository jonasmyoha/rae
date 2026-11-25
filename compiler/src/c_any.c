/* An `Any`: a number, a Bool, a Char, a String or none, with its kind. It owns
 * what it holds: a String put in it is copied (or moved, `own text`), copied with
 * the Any and freed with it, through the same paths as any type that owns memory
 * (rae_drop_struct_RaeAny and rae_deep_copy_RaeAny in the runtime's header have
 * the names those paths call). A parameter that borrows, as `log(value: Any)`,
 * gets a box that only points at the value. */
#include <stdio.h>

#include "c_backend_internal.h"
#include "diag.h"
#include "mangler.h"
#include "sema.h"

bool c_is_any(const AstTypeRef* type) {
  if (!type) return false;
  Str base = get_base_type_name(type);
  return str_eq_cstr(base, "Any") || str_eq_cstr(base, "RaeAny");
}

static const AstTypeRef* concrete_type(CFuncContext* ctx, const AstExpr* value) {
  const AstTypeRef* type = infer_expr_type_ref(ctx, value);
  if (type && ctx->generic_params && ctx->generic_args)
    type = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, (AstTypeRef*)type);
  return type;
}

/* `value` as an Any: one of its own (ANY_OWNED: a place is copied), one that
 * borrows for this statement (ANY_BORROWED), or the value as it is, moved or made
 * for it (ANY_TAKEN) */
void c_emit_any(CFuncContext* ctx, const AstExpr* value, AnyMode mode, FILE* out) {
  bool owned = mode == ANY_OWNED;
  while (value->kind == AST_EXPR_BOX) value = value->as.unary.operand;
  if (value->kind == AST_EXPR_NONE) {
    fprintf(out, "rae_any_none()");
    return;
  }
  if (value->kind == AST_EXPR_MATCH) {
    /* Each arm is made an Any of its own: they may be of several types */
    AstTypeRef any = { .parts = &(AstIdentifierPart){ .text = str_from_cstr("Any") } };
    int held = mode == ANY_BORROWED ? register_stmt_temp(ctx, &any, true) : -1;
    if (held >= 0) fprintf(out, "(__rae_stmt_tmp%d = ", held);
    ctx->match_any = true;
    emit_expr(ctx, value, out, PREC_LOWEST, false, false);
    if (held >= 0) fprintf(out, ", __rae_stmt_tmp%d_set = 1, __rae_stmt_tmp%d)", held, held);
    return;
  }
  const AstExpr* source = value->kind == AST_EXPR_OWN ? value->as.unary.operand : value;
  const AstTypeRef* slot = expr_is_raw_slot_read(source) ? c_raw_slot_type(ctx, source) : NULL;
  if (slot && ctx->generic_params && ctx->generic_args)
    slot = substitute_type_ref(ctx->compiler_ctx, ctx->generic_params, ctx->generic_args, (AstTypeRef*)slot);
  if (c_is_any(slot)) {
    /* a slot of a list: the list keeps it */
    fprintf(out, mode == ANY_OWNED ? "rae_any_own(" : "(");
    emit_expr(ctx, source, out, PREC_LOWEST, false, false);
    fprintf(out, ")");
    return;
  }
  const AstTypeRef* type = concrete_type(ctx, source);
  Str base = type ? get_base_type_name(type) : (Str){0};
  bool reference = type && (type->is_view || type->is_mod);
  bool had_expected = ctx->has_expected_type;
  ctx->has_expected_type = false;
  if (c_is_any(type)) {
    bool call = source->kind == AST_EXPR_CALL || source->kind == AST_EXPR_METHOD_CALL;
    bool made = call && !reference && (type->is_opt || c_stmt_temp_value_owns(ctx, source));
    bool copied = owned && value->kind != AST_EXPR_OWN && !made;
    int held = -1;
    if (made && mode == ANY_BORROWED) {
      AstTypeRef any = *type;
      any.next = NULL;
      held = register_stmt_temp(ctx, &any, true);
    }
    if (held >= 0) {
      fprintf(out, "(__rae_stmt_tmp%d = (", held);
      emit_expr(ctx, value, out, PREC_LOWEST, false, false);
      fprintf(out, "), __rae_stmt_tmp%d_set = 1, __rae_stmt_tmp%d)", held, held);
      ctx->has_expected_type = had_expected;
      return;
    }
    fprintf(out, copied ? "rae_any_own(rae_any((" : "rae_any((");
    emit_expr(ctx, value, out, PREC_LOWEST, false, reference);
    fprintf(out, copied ? ")))" : "))");
    ctx->has_expected_type = had_expected;
    return;
  }
  bool text = str_eq_cstr(base, "String") || source->kind == AST_EXPR_STRING || source->kind == AST_EXPR_INTERP;
  if (type && !text && !is_primitive_type(base) && !type->is_opt && !find_enum_decl(ctx, ctx->module, base)
      && base.len > 0) {
    char message[256];
    snprintf(message, sizeof message,
             "an Any holds a number, a Bool, a Char or a String; '%.*s' is none of them", (int)base.len, base.data);
    diag_error(c_source_file(ctx), (int)value->line, (int)value->column, message);
  }
  /* A number's kind by its Rae type: in C a UInt32 is a Char's type */
  bool narrow = !reference && (str_eq_cstr(base, "Int8") || str_eq_cstr(base, "Int16") || str_eq_cstr(base, "Int32")
                               || str_eq_cstr(base, "UInt8") || str_eq_cstr(base, "UInt16")
                               || str_eq_cstr(base, "UInt32") || find_enum_decl(ctx, ctx->module, base) != NULL);
  bool copied = owned && value->kind != AST_EXPR_OWN;
  fprintf(out, copied ? "rae_any_own(rae_any((" : "rae_any((");
  if (narrow) fprintf(out, "(int64_t)(");
  emit_expr(ctx, value, out, PREC_LOWEST, false, reference && is_primitive_type(base));
  if (narrow) fprintf(out, ")");
  fprintf(out, copied ? ")))" : "))");
  ctx->has_expected_type = had_expected;
}

/* The argument of a `view Any` / `mod Any` parameter: a pointer to an Any */
void c_emit_any_reference(CFuncContext* ctx, const AstExpr* value, FILE* out) {
  while (value->kind == AST_EXPR_BOX) value = value->as.unary.operand;
  if (value->kind == AST_EXPR_UNARY && (value->as.unary.op == AST_UNARY_VIEW || value->as.unary.op == AST_UNARY_MOD))
    value = value->as.unary.operand;
  const AstTypeRef* type = concrete_type(ctx, value);
  bool call = value->kind == AST_EXPR_CALL || value->kind == AST_EXPR_METHOD_CALL;
  if (c_is_any(type) && (type->is_view || type->is_mod) && (call || value->kind == AST_EXPR_IDENT)) {
    emit_expr(ctx, value, out, PREC_LOWEST, false, true);
    return;
  }
  if (c_is_any(type) && (value->kind == AST_EXPR_IDENT || value->kind == AST_EXPR_MEMBER
                         || value->kind == AST_EXPR_INDEX)) {
    fprintf(out, "&(");
    emit_expr(ctx, value, out, PREC_LOWEST, true, false);
    fprintf(out, ")");
    return;
  }
  if (c_is_any(type) && call && c_stmt_temp_value_owns(ctx, value)) {
    AstTypeRef any = *type;
    any.next = NULL;
    int held = register_stmt_temp(ctx, &any, true);
    if (held >= 0) {
      fprintf(out, "(__rae_stmt_tmp%d = (", held);
      emit_expr(ctx, value, out, PREC_LOWEST, false, false);
      fprintf(out, "), __rae_stmt_tmp%d_set = 1, &__rae_stmt_tmp%d)", held, held);
      return;
    }
  }
  fprintf(out, "(RaeAny[1]){ ");
  c_emit_any(ctx, value, ANY_BORROWED, out);
  fprintf(out, " }");
}
