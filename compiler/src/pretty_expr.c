/* pretty_expr.c - types, expressions and the ONE list policy of the Rae
 * formatter (#916): every parenthesised/braced/bracketed list — call
 * arguments, object fields, collection and list elements, and (from
 * pretty_decl.c) parameters, return items, type fields and enum members —
 * goes through pp_print_list, which decides one-line vs vertical from the
 * item count and the EXACT one-line width. */

#include "pretty_internal.h"

#include <string.h>

/* ---- the list policy ---- */

void pp_print_list(PrettyPrinter* pp, const void* items, int count, PpListStyle style,
                   PpItemsFn oneline, PpItemsFn vertical) {
  if (count == 0) {
    pp_write(pp, style.open);
    pp_write(pp, style.close);
    return;
  }
  bool wrap = count >= style.threshold && !style.never_vertical;
  if (pp->measuring) {
    /* Inside an enclosing dry run the enclosing width check covers this list;
     * only the count rule matters here (and a nested dry run per level would
     * make the measurement exponential in nesting depth). */
    if (wrap) pp->forced_break = true;
    pp_write(pp, style.open);
    if (style.pad) pp_space(pp);
    oneline(pp, items);
    if (style.pad) pp_space(pp);
    pp_write(pp, style.close);
    return;
  }
  if (!wrap && !style.never_vertical) {
    PrettyPrinter measure = pp_measure_begin(pp);
    pp_write(&measure, style.open);
    if (style.pad) pp_space(&measure);
    oneline(&measure, items);
    if (style.pad) pp_space(&measure);
    pp_write(&measure, style.close);
    if (count == 1 && measure.forced_break) {
      /* A lone item that is itself a vertical list hugs the delimiters:
       * `ret Point {` ... `}` and `foo(a: bar(` ... `))` — never
       * `foo(\n  a: bar(\n` — so a single nested literal costs no extra level. */
      wrap = false;
    } else if (measure.forced_break || measure.current_col + style.tail_width > PP_MAX_WIDTH) {
      wrap = true;
    }
  }
  if (wrap) {
    /* Tell an enclosing dry run that this list cannot share its line. */
    pp->forced_break = true;
    pp_write(pp, style.open_vertical ? style.open_vertical : style.open);
    pp_newline(pp);
    pp->indent++;
    vertical(pp, items);
    pp->indent--;
    if (!pp->start_of_line) pp_newline(pp);
    pp_write(pp, style.close);
  } else {
    pp_write(pp, style.open);
    if (style.pad) pp_space(pp);
    oneline(pp, items);
    if (style.pad) pp_space(pp);
    pp_write(pp, style.close);
  }
}

/* ---- types ---- */

void pp_write_type(PrettyPrinter* pp, const AstTypeRef* type) {
  if (!type) {
    pp_write(pp, "<type>");
    return;
  }
  if (type->is_value_arg) {
    /* `cap: 16` — a compile-time value generic argument, printed as
     * `name: expr` so the formatter round-trips it exactly as written. */
    pp_write_str(pp, type->value_name);
    pp_write(pp, ": ");
    pp_expr(pp, type->value_expr);
    return;
  }
  if (type->is_opt) pp_write(pp, "opt ");
  if (type->is_view) pp_write(pp, "view ");
  if (type->is_mod) pp_write(pp, "mod ");
  if (type->is_val) pp_write(pp, "val ");
  if (type->is_own) pp_write(pp, "own ");
  if (type->is_copy) pp_write(pp, "copy ");

  if (!type->parts) {
    pp_write(pp, "<base>");
  } else {
    const AstIdentifierPart* part = type->parts;
    bool first = true;
    while (part) {
      if (!first) pp_space(pp);
      pp_write_str(pp, part->text);
      first = false;
      part = part->next;
    }
  }
  if (type->generic_args) {
    /* Generic arguments are always one line: a type is an unbreakable name. */
    pp_write_char(pp, '(');
    const AstTypeRef* arg = type->generic_args;
    bool first = true;
    while (arg) {
      if (!first) pp_write(pp, ", ");
      pp_write_type(pp, arg);
      first = false;
      arg = arg->next;
    }
    pp_write_char(pp, ')');
  }
}

void pp_write_properties(PrettyPrinter* pp, const AstProperty* prop, const char* extern_symbol) {
  const AstProperty* current = prop;
  bool first = true;
  while (current) {
    if (!first) pp_space(pp);
    if (extern_symbol && str_eq_cstr(current->name, "extern")) {
      pp_write(pp, "extern(\"");
      pp_write(pp, extern_symbol);
      pp_write(pp, "\")");
    } else {
      pp_write_str(pp, current->name);
    }
    first = false;
    current = current->next;
  }
}

/* ---- operators ---- */

typedef enum {
  PREC_LOWEST = 0,
  PREC_OR = 1,
  PREC_AND = 2,
  PREC_IS = 3,
  PREC_COMPARE = 4,
  PREC_ADD = 5,
  PREC_MUL = 6,
  PREC_BITWISE = 7,  /* bitand/bitor/bitxor/shl/shr — bind tighter than arithmetic */
  PREC_CAST = 8,     /* `value as Type` — tighter than every binary operator */
  PREC_UNARY = 9,
  PREC_CALL = 10,
  PREC_ATOMIC = 11
} Precedence;

static int binary_precedence(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_OR: return PREC_OR;
    case AST_BIN_AND: return PREC_AND;
    case AST_BIN_IS:
    case AST_BIN_NEQ: return PREC_IS;
    case AST_BIN_LT:
    case AST_BIN_GT:
    case AST_BIN_LE:
    case AST_BIN_GE: return PREC_COMPARE;
    case AST_BIN_ADD:
    case AST_BIN_SUB: return PREC_ADD;
    case AST_BIN_MUL:
    case AST_BIN_DIV:
    case AST_BIN_MOD: return PREC_MUL;
    case AST_BIN_BITAND:
    case AST_BIN_BITOR:
    case AST_BIN_BITXOR:
    case AST_BIN_SHL:
    case AST_BIN_SHR: return PREC_BITWISE;
  }
  return PREC_LOWEST;
}

static const char* binary_op_text(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_ADD: return "+";
    case AST_BIN_SUB: return "-";
    case AST_BIN_MUL: return "*";
    case AST_BIN_DIV: return "/";
    case AST_BIN_MOD: return "%";
    case AST_BIN_LT: return "<";
    case AST_BIN_GT: return ">";
    case AST_BIN_LE: return "<=";
    case AST_BIN_GE: return ">=";
    case AST_BIN_IS: return "is";
    case AST_BIN_NEQ: return "is not";
    case AST_BIN_AND: return "and";
    case AST_BIN_OR: return "or";
    case AST_BIN_BITAND: return "bitand";
    case AST_BIN_BITOR: return "bitor";
    case AST_BIN_BITXOR: return "bitxor";
    case AST_BIN_SHL: return "shl";
    case AST_BIN_SHR: return "shr";
  }
  return "?";
}

static const char* unary_op_text(AstUnaryOp op) {
  switch (op) {
    case AST_UNARY_NEG: return "-";
    case AST_UNARY_NOT: return "not ";
    case AST_UNARY_SPAWN: return "spawn ";
    case AST_UNARY_PRE_INC: return "++";
    case AST_UNARY_PRE_DEC: return "--";
    case AST_UNARY_POST_INC: return "++";
    case AST_UNARY_POST_DEC: return "--";
    case AST_UNARY_VIEW: return "view ";
    case AST_UNARY_MOD: return "mod ";
    case AST_UNARY_BITNOT: return "bitnot ";
  }
  return "?";
}

/* ---- list item printers ---- */

static void pp_expr_prec(PrettyPrinter* pp, const AstExpr* expr, int parent_prec);

static void call_args_oneline(PrettyPrinter* pp, const void* items) {
  const AstCallArg* current = items;
  bool first = true;
  while (current) {
    if (!first) pp_write(pp, ", ");
    if (current->name.len > 0) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
    }
    pp_expr(pp, current->value);
    first = false;
    current = current->next;
  }
}

static void call_args_vertical(PrettyPrinter* pp, const void* items) {
  const AstCallArg* current = items;
  while (current) {
    if (current->value) pp_flush_comments_before(pp, current->value->line);
    size_t lines_before = pp->lines_written;
    if (current->name.len > 0) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
    }
    pp_expr(pp, current->value);
    if (current->value) pp_flush_trailing_comment(pp, current->value->line, lines_before);
    pp_newline(pp);
    current = current->next;
  }
}

static void obj_fields_oneline(PrettyPrinter* pp, const void* items) {
  const AstObjectField* field = items;
  bool first = true;
  while (field) {
    if (!first) pp_write(pp, ", ");
    pp_write_str(pp, field->name);
    pp_write(pp, ": ");
    pp_expr(pp, field->value);
    first = false;
    field = field->next;
  }
}

static void obj_fields_vertical(PrettyPrinter* pp, const void* items) {
  const AstObjectField* field = items;
  while (field) {
    if (field->value) pp_flush_comments_before(pp, field->value->line);
    size_t lines_before = pp->lines_written;
    pp_write_str(pp, field->name);
    pp_write(pp, ": ");
    pp_expr(pp, field->value);
    if (field->value) pp_flush_trailing_comment(pp, field->value->line, lines_before);
    pp_newline(pp);
    field = field->next;
  }
}

static void coll_elements_oneline(PrettyPrinter* pp, const void* items) {
  const AstCollectionElement* current = items;
  bool first = true;
  while (current) {
    if (!first) pp_write(pp, ", ");
    if (current->key) {
      pp_write_str(pp, *current->key);
      pp_write(pp, ": ");
    }
    pp_expr(pp, current->value);
    first = false;
    current = current->next;
  }
}

static void coll_elements_vertical(PrettyPrinter* pp, const void* items) {
  const AstCollectionElement* current = items;
  while (current) {
    if (current->value) pp_flush_comments_before(pp, current->value->line);
    size_t lines_before = pp->lines_written;
    if (current->key) {
      pp_write_str(pp, *current->key);
      pp_write(pp, ": ");
    }
    pp_expr(pp, current->value);
    if (current->value) pp_flush_trailing_comment(pp, current->value->line, lines_before);
    pp_newline(pp);
    current = current->next;
  }
}

static void list_elements_oneline(PrettyPrinter* pp, const void* items) {
  const AstExprList* current = items;
  bool first = true;
  while (current) {
    if (!first) pp_write(pp, ", ");
    pp_expr(pp, current->value);
    first = false;
    current = current->next;
  }
}

static void list_elements_vertical(PrettyPrinter* pp, const void* items) {
  const AstExprList* current = items;
  while (current) {
    if (current->value) pp_flush_comments_before(pp, current->value->line);
    size_t lines_before = pp->lines_written;
    pp_expr(pp, current->value);
    if (current->value) pp_flush_trailing_comment(pp, current->value->line, lines_before);
    pp_newline(pp);
    current = current->next;
  }
}

static int count_call_args(const AstCallArg* args) {
  int n = 0;
  for (; args; args = args->next) n++;
  return n;
}

void pp_call_args(PrettyPrinter* pp, const AstCallArg* args) {
  PpListStyle style = { PP_WRAP_ARGS, "(", ")", false, 0, NULL, false };
  pp_print_list(pp, args, count_call_args(args), style, call_args_oneline, call_args_vertical);
}

/* ---- expressions ---- */

static void pp_interp_string(PrettyPrinter* pp, const AstExpr* expr) {
  pp_write_char(pp, '"');
  const AstInterpPart* part = expr->as.interp.parts;
  while (part) {
    if (!part->value) {
      /* parser placeholder after an error */
    } else if (part->value->kind == AST_EXPR_STRING) {
      /* the same escapes as a plain literal, minus the surrounding quotes */
      pp_write_string_body(pp, part->value->as.string_lit);
    } else {
      pp_write_char(pp, '{');
      pp_expr(pp, part->value);
      pp_write_char(pp, '}');
    }
    part = part->next;
  }
  pp_write_char(pp, '"');
}

static void pp_object_literal(PrettyPrinter* pp, const AstExpr* expr) {
  if (expr->as.object_literal.type) {
    pp_write_type(pp, expr->as.object_literal.type);
    pp_space(pp);
  }
  int count = 0;
  for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) count++;
  PpListStyle style = { PP_WRAP_FIELDS, "{", "}", true, 0, NULL, false };
  pp_print_list(pp, expr->as.object_literal.fields, count, style, obj_fields_oneline, obj_fields_vertical);
}

static void pp_collection_literal(PrettyPrinter* pp, const AstExpr* expr) {
  if (expr->as.collection.type) {
    pp_write_type(pp, expr->as.collection.type);
    pp_space(pp);
  }
  int count = 0;
  for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) count++;
  PpListStyle style = { PP_WRAP_FIELDS, "{", "}", true, 0, NULL, false };
  if (expr->as.collection.is_bracketed) {
    style.open = "[";
    style.close = "]";
    style.pad = false;
  }
  pp_print_list(pp, expr->as.collection.elements, count, style, coll_elements_oneline, coll_elements_vertical);
}

static void pp_list_literal(PrettyPrinter* pp, const AstExpr* expr) {
  int count = 0;
  for (const AstExprList* e = expr->as.list; e; e = e->next) count++;
  PpListStyle style = { PP_WRAP_FIELDS, "[", "]", false, 0, NULL, false };
  pp_print_list(pp, expr->as.list, count, style, list_elements_oneline, list_elements_vertical);
}

static void pp_match_expr(PrettyPrinter* pp, const AstExpr* expr) {
  pp_write(pp, "match ");
  pp_expr(pp, expr->as.match_expr.subject);
  pp_write(pp, " { ");
  const AstMatchArm* arm = expr->as.match_expr.arms;
  while (arm) {
    if (arm->pattern) {
      pp_write(pp, "case ");
      pp_expr(pp, arm->pattern);
    } else {
      pp_write(pp, "default");
    }
    pp_write(pp, " => ");
    pp_expr(pp, arm->value);
    if (arm->next) pp_write(pp, ", ");
    arm = arm->next;
  }
  pp_write(pp, " }");
}

static void pp_expr_inner(PrettyPrinter* pp, const AstExpr* expr, int parent_prec);

/* Parentheses the source wrote are kept (the parser records them; the
 * mixed-bitwise rule even requires them); missing ones are added only where
 * precedence demands. */
static void pp_expr_prec(PrettyPrinter* pp, const AstExpr* expr, int parent_prec) {
  if (expr && expr->is_parenthesized) {
    pp_write_char(pp, '(');
    pp_expr_inner(pp, expr, PREC_LOWEST);
    pp_write_char(pp, ')');
    return;
  }
  pp_expr_inner(pp, expr, parent_prec);
}

static void pp_expr_inner(PrettyPrinter* pp, const AstExpr* expr, int parent_prec) {
  if (!expr) {
    pp_write(pp, "<expr>");
    return;
  }
  switch (expr->kind) {
    case AST_EXPR_IDENT:
      pp_write_str(pp, expr->as.ident);
      break;
    case AST_EXPR_INTEGER:
      pp_write_str(pp, expr->as.integer);
      break;
    case AST_EXPR_FLOAT:
      pp_write_str(pp, expr->as.floating);
      break;
    case AST_EXPR_STRING:
      if (expr->is_raw) {
        pp_write_raw_string_literal(pp, expr->as.string_lit);
      } else {
        pp_write_string_literal(pp, expr->as.string_lit);
      }
      break;
    case AST_EXPR_INTERP:
      pp_interp_string(pp, expr);
      break;
    case AST_EXPR_CHAR:
      /* char_lit is the source spelling between the quotes (escapes intact) */
      pp_write_char(pp, '\'');
      pp_write_str(pp, expr->as.char_lit);
      pp_write_char(pp, '\'');
      break;
    case AST_EXPR_BOOL:
      pp_write(pp, expr->as.boolean ? "true" : "false");
      break;
    case AST_EXPR_NONE:
      pp_write(pp, "none");
      break;
    case AST_EXPR_OBJECT:
      pp_object_literal(pp, expr);
      break;
    case AST_EXPR_COLLECTION_LITERAL:
      pp_collection_literal(pp, expr);
      break;
    case AST_EXPR_LIST:
      pp_list_literal(pp, expr);
      break;
    case AST_EXPR_MATCH:
      pp_match_expr(pp, expr);
      break;
    case AST_EXPR_MEMBER: {
      bool need_paren = PREC_CALL < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.member.object, PREC_CALL);
      pp_write_char(pp, '.');
      pp_write_str(pp, expr->as.member.member);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_CALL: {
      bool need_paren = PREC_CALL < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.call.callee, PREC_CALL);
      pp_call_args(pp, expr->as.call.args);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_METHOD_CALL: {
      bool need_paren = PREC_CALL < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.method_call.object, PREC_CALL);
      pp_write_char(pp, '.');
      pp_write_str(pp, expr->as.method_call.method_name);
      pp_call_args(pp, expr->as.method_call.args);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_INDEX:
      pp_expr_prec(pp, expr->as.index.target, PREC_CALL);
      pp_write_char(pp, '[');
      pp_expr(pp, expr->as.index.index);
      pp_write_char(pp, ']');
      break;
    case AST_EXPR_UNARY: {
      bool need_paren = PREC_UNARY < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      bool is_post = expr->as.unary.op == AST_UNARY_POST_INC || expr->as.unary.op == AST_UNARY_POST_DEC;
      if (!is_post) pp_write(pp, unary_op_text(expr->as.unary.op));
      pp_expr_prec(pp, expr->as.unary.operand, PREC_UNARY);
      if (is_post) pp_write(pp, unary_op_text(expr->as.unary.op));
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_OWN:
      /* `own expr` — the move marker; parses as a unary prefix */
      pp_write(pp, "own ");
      pp_expr_prec(pp, expr->as.unary.operand, PREC_UNARY);
      break;
    case AST_EXPR_BOX:
      pp_write(pp, "box(");
      pp_expr(pp, expr->as.unary.operand);
      pp_write_char(pp, ')');
      break;
    case AST_EXPR_UNBOX:
      pp_write(pp, "unbox(");
      pp_expr(pp, expr->as.unary.operand);
      pp_write_char(pp, ')');
      break;
    case AST_EXPR_CAST: {
      bool need_paren = PREC_CAST < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.cast.operand, PREC_CAST);
      pp_write(pp, " as ");
      pp_write_type(pp, expr->as.cast.target);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_BINARY: {
      int prec = binary_precedence(expr->as.binary.op);
      bool need_paren = prec < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.binary.lhs, prec);
      pp_space(pp);
      pp_write(pp, binary_op_text(expr->as.binary.op));
      pp_space(pp);
      pp_expr_prec(pp, expr->as.binary.rhs, prec + 1);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
  }
}

void pp_expr(PrettyPrinter* pp, const AstExpr* expr) {
  pp_expr_prec(pp, expr, PREC_LOWEST);
}
