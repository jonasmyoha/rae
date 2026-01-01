/* pretty.c - Rae pretty printer implementation */

#include "pretty.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  FILE* out;
  int indent;
  int start_of_line;
} PrettyPrinter;

static void pp_write_indent(PrettyPrinter* pp) {
  for (int i = 0; i < pp->indent; ++i) {
    fputs("  ", pp->out);
  }
}

static void pp_write(PrettyPrinter* pp, const char* text) {
  if (pp->start_of_line) {
    pp_write_indent(pp);
    pp->start_of_line = 0;
  }
  fputs(text, pp->out);
}

static void pp_write_char(PrettyPrinter* pp, char ch) {
  if (pp->start_of_line) {
    pp_write_indent(pp);
    pp->start_of_line = 0;
  }
  fputc(ch, pp->out);
}

static void pp_write_str(PrettyPrinter* pp, Str s) {
  if (!s.data || s.len == 0) {
    return;
  }
  if (pp->start_of_line) {
    pp_write_indent(pp);
    pp->start_of_line = 0;
  }
  fwrite(s.data, 1, s.len, pp->out);
}

static void pp_newline(PrettyPrinter* pp) {
  fputc('\n', pp->out);
  pp->start_of_line = 1;
}

static void pp_space(PrettyPrinter* pp) {
  pp_write_char(pp, ' ');
}

static void pp_begin_block(PrettyPrinter* pp) {
  pp_write_char(pp, '{');
  pp_newline(pp);
  pp->indent += 1;
}

static void pp_end_block(PrettyPrinter* pp) {
  pp->indent -= 1;
  pp_write_char(pp, '}');
}

static void pp_write_type(PrettyPrinter* pp, const AstTypeRef* type) {
  if (!type || !type->parts) {
    pp_write(pp, "<type>");
    return;
  }
  const AstIdentifierPart* part = type->parts;
  int first = 1;
  while (part) {
    if (!first) {
      pp_space(pp);
    }
    pp_write_str(pp, part->text);
    first = 0;
    part = part->next;
  }
}

static void pp_write_properties(PrettyPrinter* pp, const AstProperty* prop) {
  const AstProperty* current = prop;
  int first = 1;
  while (current) {
    if (!first) {
      pp_space(pp);
    }
    pp_write_str(pp, current->name);
    first = 0;
    current = current->next;
  }
}

typedef enum {
  PREC_LOWEST = 0,
  PREC_OR = 1,
  PREC_AND = 2,
  PREC_IS = 3,
  PREC_COMPARE = 4,
  PREC_ADD = 5,
  PREC_MUL = 6,
  PREC_UNARY = 7,
  PREC_CALL = 8,
  PREC_PRIMARY = 9
} Precedence;

static int binary_precedence(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_OR:
      return PREC_OR;
    case AST_BIN_AND:
      return PREC_AND;
    case AST_BIN_IS:
      return PREC_IS;
    case AST_BIN_LT:
    case AST_BIN_GT:
    case AST_BIN_LE:
    case AST_BIN_GE:
      return PREC_COMPARE;
    case AST_BIN_ADD:
    case AST_BIN_SUB:
      return PREC_ADD;
    case AST_BIN_MUL:
    case AST_BIN_DIV:
    case AST_BIN_MOD:
      return PREC_MUL;
  }
  return PREC_LOWEST;
}

static const char* binary_op_text(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_ADD:
      return "+";
    case AST_BIN_SUB:
      return "-";
    case AST_BIN_MUL:
      return "*";
    case AST_BIN_DIV:
      return "/";
    case AST_BIN_MOD:
      return "%";
    case AST_BIN_LT:
      return "<";
    case AST_BIN_GT:
      return ">";
    case AST_BIN_LE:
      return "<=";
    case AST_BIN_GE:
      return ">=";
    case AST_BIN_IS:
      return "is";
    case AST_BIN_AND:
      return "and";
    case AST_BIN_OR:
      return "or";
  }
  return "?";
}

static const char* unary_op_text(AstUnaryOp op) {
  switch (op) {
    case AST_UNARY_NEG:
      return "-";
    case AST_UNARY_NOT:
      return "not";
    case AST_UNARY_SPAWN:
      return "spawn";
  }
  return "?";
}

static void pp_expr_prec(PrettyPrinter* pp, const AstExpr* expr, int parent_prec);

static void pp_call_args(PrettyPrinter* pp, const AstCallArg* args) {
  const AstCallArg* current = args;
  int first = 1;
  while (current) {
    if (!first) {
      pp_write(pp, ", ");
    }
    pp_write_str(pp, current->name);
    pp_write(pp, ": ");
    pp_expr_prec(pp, current->value, PREC_LOWEST);
    first = 0;
    current = current->next;
  }
}

static void pp_object_fields(PrettyPrinter* pp, const AstObjectField* fields) {
  const AstObjectField* current = fields;
  int first = 1;
  while (current) {
    if (!first) {
      pp_write(pp, ", ");
    }
    pp_write_str(pp, current->name);
    pp_write(pp, ": ");
    pp_expr_prec(pp, current->value, PREC_LOWEST);
    first = 0;
    current = current->next;
  }
}

static void pp_expr_prec(PrettyPrinter* pp, const AstExpr* expr, int parent_prec) {
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
    case AST_EXPR_STRING:
      pp_write_str(pp, expr->as.string_lit);
      break;
    case AST_EXPR_BOOL:
      pp_write(pp, expr->as.boolean ? "true" : "false");
      break;
    case AST_EXPR_NONE:
      pp_write(pp, "none");
      break;
    case AST_EXPR_OBJECT:
      pp_write_char(pp, '(');
      pp_object_fields(pp, expr->as.object);
      pp_write_char(pp, ')');
      break;
    case AST_EXPR_MATCH: {
      pp_write(pp, "match ");
      pp_expr_prec(pp, expr->as.match_expr.subject, PREC_LOWEST);
      pp_write(pp, " { ");
      AstMatchArm* arm = expr->as.match_expr.arms;
      while (arm) {
        pp_write(pp, "case ");
        if (arm->pattern) {
          pp_expr_prec(pp, arm->pattern, PREC_LOWEST);
        } else {
          pp_write(pp, "_");
        }
        pp_write(pp, " => ");
        pp_expr_prec(pp, arm->value, PREC_LOWEST);
        if (arm->next) {
          pp_write(pp, ", ");
        }
        arm = arm->next;
      }
      pp_write(pp, " }");
      break;
    }
    case AST_EXPR_MEMBER: {
      int prec = PREC_CALL;
      int need_paren = prec < parent_prec;
      if (need_paren) {
        pp_write_char(pp, '(');
      }
      pp_expr_prec(pp, expr->as.member.object, PREC_CALL);
      pp_write_char(pp, '.');
      pp_write_str(pp, expr->as.member.member);
      if (need_paren) {
        pp_write_char(pp, ')');
      }
      break;
    }
    case AST_EXPR_CALL: {
      int prec = PREC_CALL;
      int need_paren = prec < parent_prec;
      if (need_paren) {
        pp_write_char(pp, '(');
      }
      pp_expr_prec(pp, expr->as.call.callee, PREC_CALL);
      pp_write_char(pp, '(');
      pp_call_args(pp, expr->as.call.args);
      pp_write_char(pp, ')');
      if (need_paren) {
        pp_write_char(pp, ')');
      }
      break;
    }
    case AST_EXPR_UNARY: {
      int prec = PREC_UNARY;
      int need_paren = prec < parent_prec;
      if (need_paren) {
        pp_write_char(pp, '(');
      }
      const char* op_text = unary_op_text(expr->as.unary.op);
      pp_write(pp, op_text);
      if (expr->as.unary.op == AST_UNARY_NOT || expr->as.unary.op == AST_UNARY_SPAWN) {
        pp_space(pp);
      }
      pp_expr_prec(pp, expr->as.unary.operand, PREC_UNARY);
      if (need_paren) {
        pp_write_char(pp, ')');
      }
      break;
    }
    case AST_EXPR_BINARY: {
      int prec = binary_precedence(expr->as.binary.op);
      int need_paren = prec < parent_prec;
      if (need_paren) {
        pp_write_char(pp, '(');
      }
      pp_expr_prec(pp, expr->as.binary.lhs, prec);
      pp_space(pp);
      pp_write(pp, binary_op_text(expr->as.binary.op));
      pp_space(pp);
      pp_expr_prec(pp, expr->as.binary.rhs, prec + 1);
      if (need_paren) {
        pp_write_char(pp, ')');
      }
      break;
    }
  }
}

static void pp_expr(PrettyPrinter* pp, const AstExpr* expr) {
  pp_expr_prec(pp, expr, PREC_LOWEST);
}

static void pp_print_stmt(PrettyPrinter* pp, const AstStmt* stmt);

static void pp_print_block_body(PrettyPrinter* pp, const AstBlock* block) {
  const AstStmt* current = (block ? block->first : NULL);
  while (current) {
    pp_print_stmt(pp, current);
    current = current->next;
  }
}

static void pp_print_def_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_write(pp, "def ");
  pp_write_str(pp, stmt->as.def_stmt.name);
  pp_write(pp, ": ");
  pp_write_type(pp, stmt->as.def_stmt.type);
  if (stmt->as.def_stmt.is_move) {
    pp_write(pp, " => ");
  } else {
    pp_write(pp, " = ");
  }
  pp_expr(pp, stmt->as.def_stmt.value);
  pp_newline(pp);
}

static void pp_print_destruct_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  const AstDestructureBinding* binding = stmt->as.destruct_stmt.bindings;
  int first = 1;
  while (binding) {
    if (!first) {
      pp_write(pp, ", ");
    }
    pp_write(pp, "def ");
    pp_write_str(pp, binding->local_name);
    pp_write(pp, ": ");
    pp_write_str(pp, binding->return_label);
    first = 0;
    binding = binding->next;
  }
  pp_write(pp, " = ");
  pp_expr(pp, stmt->as.destruct_stmt.call);
  pp_newline(pp);
}

static void pp_print_return_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_write(pp, "ret");
  AstReturnArg* arg = stmt->as.ret_stmt.values;
  if (arg) {
    pp_space(pp);
    int first = 1;
    while (arg) {
      if (!first) {
        pp_write(pp, ", ");
      }
      if (arg->has_label) {
        pp_write_str(pp, arg->label);
        pp_write(pp, ": ");
      }
      pp_expr(pp, arg->value);
      first = 0;
      arg = arg->next;
    }
  }
  pp_newline(pp);
}

static void pp_print_if_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_write(pp, "if ");
  pp_expr(pp, stmt->as.if_stmt.condition);
  pp_space(pp);
  pp_begin_block(pp);
  pp_print_block_body(pp, stmt->as.if_stmt.then_block);
  pp_end_block(pp);
  if (stmt->as.if_stmt.else_block) {
    pp_write(pp, " else ");
    pp_begin_block(pp);
    pp_print_block_body(pp, stmt->as.if_stmt.else_block);
    pp_end_block(pp);
  }
  pp_newline(pp);
}

static void pp_print_match_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_write(pp, "match ");
  pp_expr(pp, stmt->as.match_stmt.subject);
  pp_space(pp);
  pp_begin_block(pp);
  AstMatchCase* current = stmt->as.match_stmt.cases;
  while (current) {
    pp_write(pp, "case ");
    pp_expr(pp, current->pattern);
    pp_space(pp);
    pp_begin_block(pp);
    pp_print_block_body(pp, current->block);
    pp_end_block(pp);
    pp_newline(pp);
    current = current->next;
  }
  pp_end_block(pp);
  pp_newline(pp);
}

static void pp_print_while_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_write(pp, "while ");
  pp_expr(pp, stmt->as.while_stmt.condition);
  pp_space(pp);
  pp_begin_block(pp);
  pp_print_block_body(pp, stmt->as.while_stmt.body);
  pp_end_block(pp);
  pp_newline(pp);
}

static void pp_print_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  switch (stmt->kind) {
    case AST_STMT_DEF:
      pp_print_def_stmt(pp, stmt);
      break;
    case AST_STMT_DESTRUCT:
      pp_print_destruct_stmt(pp, stmt);
      break;
    case AST_STMT_EXPR:
      pp_expr(pp, stmt->as.expr_stmt);
      pp_newline(pp);
      break;
    case AST_STMT_RET:
      pp_print_return_stmt(pp, stmt);
      break;
    case AST_STMT_IF:
      pp_print_if_stmt(pp, stmt);
      break;
    case AST_STMT_WHILE:
      pp_print_while_stmt(pp, stmt);
      break;
    case AST_STMT_MATCH:
      pp_print_match_stmt(pp, stmt);
      break;
  }
}

static void pp_print_type_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "type ");
  pp_write_str(pp, decl->as.type_decl.name);
  if (decl->as.type_decl.properties) {
    pp_write(pp, ": ");
    pp_write_properties(pp, decl->as.type_decl.properties);
  }
  pp_space(pp);
  pp_begin_block(pp);
  const AstTypeField* field = decl->as.type_decl.fields;
  while (field) {
    pp_write_str(pp, field->name);
    pp_write(pp, ": ");
    pp_write_type(pp, field->type);
    pp_newline(pp);
    field = field->next;
  }
  pp_end_block(pp);
  pp_newline(pp);
}

static void pp_print_params(PrettyPrinter* pp, const AstParam* param) {
  const AstParam* current = param;
  int first = 1;
  while (current) {
    if (!first) {
      pp_write(pp, ", ");
    }
    pp_write_str(pp, current->name);
    pp_write(pp, ": ");
    pp_write_type(pp, current->type);
    first = 0;
    current = current->next;
  }
}

static void pp_print_return_items(PrettyPrinter* pp, const AstReturnItem* item) {
  const AstReturnItem* current = item;
  int first = 1;
  while (current) {
    if (!first) {
      pp_write(pp, ", ");
    }
    if (current->has_name) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
    }
    pp_write_type(pp, current->type);
    first = 0;
    current = current->next;
  }
}

static void pp_print_func_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "func ");
  pp_write_str(pp, decl->as.func_decl.name);
  pp_write(pp, "(");
  pp_print_params(pp, decl->as.func_decl.params);
  pp_write(pp, ")");
  int has_props = decl->as.func_decl.properties != NULL;
  int has_returns = decl->as.func_decl.returns != NULL;
  if (has_props || has_returns) {
    pp_write(pp, ": ");
    if (has_props) {
      pp_write_properties(pp, decl->as.func_decl.properties);
    }
    if (has_returns) {
      if (has_props) {
        pp_space(pp);
      }
      pp_write(pp, "ret ");
      pp_print_return_items(pp, decl->as.func_decl.returns);
    }
  }
  pp_space(pp);
  pp_begin_block(pp);
  pp_print_block_body(pp, decl->as.func_decl.body);
  pp_end_block(pp);
  pp_newline(pp);
}

static void pp_print_decl(PrettyPrinter* pp, const AstDecl* decl) {
  if (decl->kind == AST_DECL_TYPE) {
    pp_print_type_decl(pp, decl);
  } else {
    pp_print_func_decl(pp, decl);
  }
}

void pretty_print_module(const AstModule* module, FILE* out) {
  PrettyPrinter pp = {
      .out = out,
      .indent = 0,
      .start_of_line = 1,
  };

  if (!module || !module->decls) {
    pp_newline(&pp);
    return;
  }

  const AstDecl* decl = module->decls;
  int first = 1;
  while (decl) {
    if (!first) {
      pp_newline(&pp);
    }
    pp_print_decl(&pp, decl);
    first = 0;
    decl = decl->next;
  }
}
