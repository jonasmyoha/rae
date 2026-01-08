/* pretty.c - Rae pretty printer implementation */

#include "pretty.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "lexer.h"

typedef struct {
  FILE* out;
  int indent;
  int start_of_line;
  int current_col;
  const Token* comments;
  size_t comment_count;
  size_t next_comment_idx;
} PrettyPrinter;

static void pp_write_indent(PrettyPrinter* pp) {
  for (int i = 0; i < pp->indent; ++i) {
    fputs("  ", pp->out);
    pp->current_col += 2;
  }
}

static void pp_newline(PrettyPrinter* pp) {
  fputc('\n', pp->out);
  pp->start_of_line = 1;
  pp->current_col = 0;
}

static void pp_write_raw(PrettyPrinter* pp, const char* text, size_t len) {
  if (pp->start_of_line) {
    pp_write_indent(pp);
    pp->start_of_line = 0;
  }
  fwrite(text, 1, len, pp->out);
  pp->current_col += (int)len;
}

static void pp_write(PrettyPrinter* pp, const char* text) {
  pp_write_raw(pp, text, strlen(text));
}

static void pp_write_char(PrettyPrinter* pp, char ch) {
  pp_write_raw(pp, &ch, 1);
}

static void pp_write_str(PrettyPrinter* pp, Str s) {
  if (!s.data || s.len == 0) {
    return;
  }
  pp_write_raw(pp, s.data, s.len);
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

static void pp_check_comments(PrettyPrinter* pp, size_t line) {
  while (pp->next_comment_idx < pp->comment_count &&
         pp->comments[pp->next_comment_idx].line <= line) {
    const Token* comment = &pp->comments[pp->next_comment_idx++];
    if (!pp->start_of_line) {
        pp_newline(pp);
    }
    
    // Split long comments (120 chars)
    if (comment->kind == TOK_COMMENT) {
        const char* text = comment->lexeme.data;
        size_t len = comment->lexeme.len;
        // Skip leading #
        if (len > 0 && text[0] == '#') { text++; len--; } 
        
        // Simple wrapping
        size_t pos = 0;
        while (pos < len) {
            pp_write(pp, "#");
            size_t remaining = len - pos;
            size_t to_write = remaining;
            if (pp->indent * 2 + 1 + remaining > 120) {
                to_write = 120 - (pp->indent * 2 + 1);
                // Find last space
                size_t last_space = to_write;
                while (last_space > 0 && !isspace((unsigned char)text[pos + last_space])) {
                    last_space--;
                }
                if (last_space > 0) to_write = last_space;
            }
            pp_write_raw(pp, text + pos, to_write);
            pp_newline(pp);
            pos += to_write;
            while (pos < len && isspace((unsigned char)text[pos])) pos++;
        }
    } else {
        // Block comment - just write it but maybe handle 120 limit?
        // User said: "no # need to be added inside block comments, when moving to a new line"
        pp_write_str(pp, comment->lexeme);
        pp_newline(pp);
    }
  }
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
    case AST_BIN_OR: return PREC_OR;
    case AST_BIN_AND: return PREC_AND;
    case AST_BIN_IS: return PREC_IS;
    case AST_BIN_LT:
    case AST_BIN_GT:
    case AST_BIN_LE:
    case AST_BIN_GE: return PREC_COMPARE;
    case AST_BIN_ADD:
    case AST_BIN_SUB: return PREC_ADD;
    case AST_BIN_MUL:
    case AST_BIN_DIV:
    case AST_BIN_MOD: return PREC_MUL;
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
    case AST_BIN_AND: return "and";
    case AST_BIN_OR: return "or";
  }
  return "?";
}

static const char* unary_op_text(AstUnaryOp op) {
  switch (op) {
    case AST_UNARY_NEG: return "-";
    case AST_UNARY_NOT: return "not";
    case AST_UNARY_SPAWN: return "spawn";
    case AST_UNARY_PRE_INC: return "++";
    case AST_UNARY_PRE_DEC: return "--";
  }
  return "?";
}

static void pp_expr_prec(PrettyPrinter* pp, const AstExpr* expr, int parent_prec);

static void pp_call_args(PrettyPrinter* pp, const AstCallArg* args) {
  if (!args) return;
  const AstCallArg* current = args;
  
  bool wrap = false;
  int count = 0;
  int estimated_len = pp->current_col;
  while (current) { 
    count++; 
    estimated_len += (int)current->name.len + 2 + 20; 
    current = current->next; 
  }
  if (count > 3 || estimated_len > 120) wrap = true;
  
  current = args;
  if (wrap) {
    pp_newline(pp);
    pp->indent++;
    while (current) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
      pp_expr_prec(pp, current->value, PREC_LOWEST);
      pp_write(pp, ",");
      pp_newline(pp);
      current = current->next;
    }
    pp->indent--;
    pp_write_indent(pp);
    pp->start_of_line = 0;
  } else {
    int first = 1;
    while (current) {
      if (!first) { pp_write(pp, ", "); } 
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
      pp_expr_prec(pp, current->value, PREC_LOWEST);
      first = 0;
      current = current->next;
    }
  }
}

static void pp_object_fields(PrettyPrinter* pp, const AstObjectField* fields) {
  if (!fields) return;
  const AstObjectField* current = fields;
  
  bool wrap = false;
  int count = 0;
  int estimated_len = pp->current_col;
  while (current) {
    count++;
    estimated_len += (int)current->name.len + 2 + 20;
    current = current->next;
  }
  if (count > 3 || estimated_len > 100) wrap = true;

  current = fields;
  if (wrap) {
    pp_newline(pp);
    pp->indent++;
    while (current) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
      pp_expr_prec(pp, current->value, PREC_LOWEST);
      pp_write(pp, ",");
      pp_newline(pp);
      current = current->next;
    }
    pp->indent--;
    pp_write_indent(pp);
    pp->start_of_line = 0;
  } else {
    int first = 1;
    while (current) {
      if (!first) { pp_write(pp, ", "); }
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
      pp_expr_prec(pp, current->value, PREC_LOWEST);
      first = 0;
      current = current->next;
    }
  }
}

static int count_required_hashes(Str s) {
  int n = 0;
  while (1) {
    bool found = false;
    const char* data = s.data;
    for (size_t i = 0; i < s.len; i++) {
      if (data[i] == '"') {
        int hashes = 0;
        size_t j = i + 1;
        while (j < s.len && data[j] == '#') {
          hashes++;
          j++;
        }
        if (hashes == n) {
          found = true;
          break;
        }
      }
    }
    if (!found) return n;
    n++;
  }
}

static void pp_write_string_literal(PrettyPrinter* pp, Str s) {
  pp_write_char(pp, '"');
  for (size_t i = 0; i < s.len; i++) {
    char c = s.data[i];
    switch (c) {
      case '"': pp_write(pp, "\\\""); break;
      case '\\': pp_write(pp, "\\\\"); break;
      case '\n': pp_write(pp, "\\n"); break;
      case '\r': pp_write(pp, "\\r"); break;
      case '\t': pp_write(pp, "\\t"); break;
      default: pp_write_char(pp, c); break;
    }
  }
  pp_write_char(pp, '"');
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
    case AST_EXPR_FLOAT:
      pp_write_str(pp, expr->as.floating);
      break;
    case AST_EXPR_STRING:
      if (expr->is_raw) {
        int hashes = count_required_hashes(expr->as.string_lit);
        pp_write(pp, "r");
        for (int i = 0; i < hashes; i++) pp_write(pp, "#");
        pp_write(pp, "\"");
        pp_write_str(pp, expr->as.string_lit);
        pp_write(pp, "\"");
        for (int i = 0; i < hashes; i++) pp_write(pp, "#");
      } else {
        pp_write_string_literal(pp, expr->as.string_lit);
      }
      break;
    case AST_EXPR_CHAR:
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
        if (arm->pattern) {
          pp_write(pp, "case ");
          pp_expr_prec(pp, arm->pattern, PREC_LOWEST);
        } else {
          pp_write(pp, "default");
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
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.member.object, PREC_CALL);
      pp_write_char(pp, '.');
      pp_write_str(pp, expr->as.member.member);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_CALL: {
      int prec = PREC_CALL;
      int need_paren = prec < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.call.callee, PREC_CALL);
      pp_write_char(pp, '(');
      pp_call_args(pp, expr->as.call.args);
      pp_write_char(pp, ')');
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_UNARY: {
      int prec = PREC_UNARY;
      int need_paren = prec < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      const char* op_text = unary_op_text(expr->as.unary.op);
      pp_write(pp, op_text);
      if (expr->as.unary.op == AST_UNARY_NOT || expr->as.unary.op == AST_UNARY_SPAWN) {
        pp_space(pp);
      }
      pp_expr_prec(pp, expr->as.unary.operand, PREC_UNARY);
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_BINARY: {
      int prec = binary_precedence(expr->as.binary.op);
      int need_paren = prec < parent_prec;
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
  pp_check_comments(pp, stmt->line);
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
  pp_check_comments(pp, stmt->line);
  const AstDestructureBinding* binding = stmt->as.destruct_stmt.bindings;
  int first = 1;
  while (binding) {
    if (!first) { pp_write(pp, ", "); } 
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
  pp_check_comments(pp, stmt->line);
  pp_write(pp, "ret");
  AstReturnArg* arg = stmt->as.ret_stmt.values;
  if (!arg) {
    pp_newline(pp);
    return;
  }

  bool wrap = false;
  int count = 0;
  int estimated_len = pp->current_col;
  AstReturnArg* current = arg;
  while (current) {
    count++;
    estimated_len += (current->has_label ? (int)current->label.len + 2 : 0) + 20;
    current = current->next;
  }
  if (count > 3 || estimated_len > 100) wrap = true;

  current = arg;
  if (wrap) {
    pp_newline(pp);
    pp->indent++;
    while (current) {
      if (current->has_label) {
        pp_write_str(pp, current->label);
        pp_write(pp, ": ");
      }
      pp_expr(pp, current->value);
      pp_write(pp, ",");
      pp_newline(pp);
      current = current->next;
    }
    pp->indent--;
  } else {
    pp_space(pp);
    int first = 1;
    while (current) {
      if (!first) { pp_write(pp, ", "); } 
      if (current->has_label) {
        pp_write_str(pp, current->label);
        pp_write(pp, ": ");
      }
      pp_expr(pp, current->value);
      first = 0;
      current = current->next;
    }
    pp_newline(pp);
  }
}

static void pp_print_if_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_check_comments(pp, stmt->line);
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
  pp_check_comments(pp, stmt->line);
  pp_write(pp, "match ");
  pp_expr(pp, stmt->as.match_stmt.subject);
  pp_space(pp);
  pp_begin_block(pp);
  AstMatchCase* current = stmt->as.match_stmt.cases;
  while (current) {
    if (current->pattern) {
      pp_write(pp, "case ");
      pp_expr(pp, current->pattern);
    } else {
      pp_write(pp, "default");
    }
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

static void pp_print_loop_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_check_comments(pp, stmt->line);
  pp_write(pp, "loop ");
  if (stmt->as.loop_stmt.init) {
    if (stmt->as.loop_stmt.init->kind == AST_STMT_DEF) {
         pp_write_str(pp, stmt->as.loop_stmt.init->as.def_stmt.name);
         pp_write(pp, ": ");
         pp_write_type(pp, stmt->as.loop_stmt.init->as.def_stmt.type);
         if (stmt->as.loop_stmt.is_range) {
             pp_write(pp, " in ");
         } else {
             pp_write(pp, " = ");
             pp_expr(pp, stmt->as.loop_stmt.init->as.def_stmt.value);
             pp_write(pp, ", ");
         }
    } else if (stmt->as.loop_stmt.init->kind == AST_STMT_EXPR) {
         pp_expr(pp, stmt->as.loop_stmt.init->as.expr_stmt);
         pp_write(pp, ", ");
    }
  }

  if (stmt->as.loop_stmt.condition) {
    pp_expr(pp, stmt->as.loop_stmt.condition);
  }

  if (stmt->as.loop_stmt.increment) {
    pp_write(pp, ", ");
    pp_expr(pp, stmt->as.loop_stmt.increment);
  }

  pp_space(pp);
  pp_begin_block(pp);
  pp_print_block_body(pp, stmt->as.loop_stmt.body);
  pp_end_block(pp);
  pp_newline(pp);
}

static void pp_print_assign_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_check_comments(pp, stmt->line);
  pp_expr(pp, stmt->as.assign_stmt.target);
  if (stmt->as.assign_stmt.is_move) {
    pp_write(pp, " => ");
  } else {
    pp_write(pp, " = ");
  }
  pp_expr(pp, stmt->as.assign_stmt.value);
  pp_newline(pp);
}

static void pp_print_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  switch (stmt->kind) {
    case AST_STMT_DEF: pp_print_def_stmt(pp, stmt); break;
    case AST_STMT_DESTRUCT: pp_print_destruct_stmt(pp, stmt); break;
    case AST_STMT_EXPR:
      pp_check_comments(pp, stmt->line);
      pp_expr(pp, stmt->as.expr_stmt);
      pp_newline(pp);
      break;
    case AST_STMT_RET: pp_print_return_stmt(pp, stmt); break;
    case AST_STMT_IF: pp_print_if_stmt(pp, stmt); break;
    case AST_STMT_LOOP: pp_print_loop_stmt(pp, stmt); break;
    case AST_STMT_MATCH: pp_print_match_stmt(pp, stmt); break;
    case AST_STMT_ASSIGN: pp_print_assign_stmt(pp, stmt); break;
  }
}

static void pp_print_type_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_check_comments(pp, decl->line);
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
  if (!param) return;
  const AstParam* current = param;
  
  bool wrap = false;
  int count = 0;
  int estimated_len = pp->current_col;
  while (current) {
    count++;
    estimated_len += (int)current->name.len + 2 + 15;
    current = current->next;
  }
  if (count > 4 || estimated_len > 100) wrap = true;

  current = param;
  if (wrap) {
    pp_newline(pp);
    pp->indent++;
    while (current) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
      pp_write_type(pp, current->type);
      pp_write(pp, ",");
      pp_newline(pp);
      current = current->next;
    }
    pp->indent--;
    pp_write_indent(pp);
    pp->start_of_line = 0;
  } else {
    int first = 1;
    while (current) {
      if (!first) { pp_write(pp, ", "); } 
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
      pp_write_type(pp, current->type);
      first = 0;
      current = current->next;
    }
  }
}

static void pp_print_return_items(PrettyPrinter* pp, const AstReturnItem* item) {
  if (!item) return;
  const AstReturnItem* current = item;
  
  bool wrap = false;
  int count = 0;
  int estimated_len = pp->current_col;
  while (current) {
    count++;
    estimated_len += (current->has_name ? (int)current->name.len + 2 : 0) + 15;
    current = current->next;
  }
  if (count > 3 || estimated_len > 100) wrap = true;

  current = item;
  if (wrap) {
    pp_newline(pp);
    pp->indent++;
    while (current) {
      if (current->has_name) {
        pp_write_str(pp, current->name);
        pp_write(pp, ": ");
      }
      pp_write_type(pp, current->type);
      pp_write(pp, ",");
      pp_newline(pp);
      current = current->next;
    }
    pp->indent--;
    pp_write_indent(pp);
    pp->start_of_line = 0;
  } else {
    int first = 1;
    while (current) {
      if (!first) { pp_write(pp, ", "); } 
      if (current->has_name) {
        pp_write_str(pp, current->name);
        pp_write(pp, ": ");
      }
      pp_write_type(pp, current->type);
      first = 0;
      current = current->next;
    }
  }
}

static void pp_print_func_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_check_comments(pp, decl->line);
  if (decl->as.func_decl.is_extern) {
    pp_write(pp, "extern ");
  }
  pp_write(pp, "func ");
  pp_write_str(pp, decl->as.func_decl.name);
  pp_write(pp, "(");
  pp_print_params(pp, decl->as.func_decl.params);
  pp_write(pp, ")");
  int has_props = decl->as.func_decl.properties != NULL;
  int has_returns = decl->as.func_decl.returns != NULL;
  if (has_props || has_returns) {
    pp_write(pp, ": ");
    if (has_props) pp_write_properties(pp, decl->as.func_decl.properties);
    if (has_returns) {
      if (has_props) pp_space(pp);
      pp_write(pp, "ret ");
      pp_print_return_items(pp, decl->as.func_decl.returns);
    }
  }
  if (decl->as.func_decl.body) {
      pp_space(pp);
      pp_begin_block(pp);
      pp_print_block_body(pp, decl->as.func_decl.body);
      pp_end_block(pp);
  }
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
      .current_col = 0,
      .comments = module->comments,
      .comment_count = module->comment_count,
      .next_comment_idx = 0,
  };

  if (!module) return;

  const AstImport* imp = module->imports;
  while (imp) {
      pp_check_comments(&pp, imp->line);
      pp_write(&pp, imp->is_export ? "export " : "import ");
      pp_write_str(&pp, imp->path);
      pp_newline(&pp);
      imp = imp->next;
  }

  const AstDecl* decl = module->decls;
  int first = 1;
  while (decl) {
    if (!first) pp_newline(&pp);
    pp_print_decl(&pp, decl);
    first = 0;
    decl = decl->next;
  }
  
  // Print remaining comments
  pp_check_comments(&pp, (size_t)-1);
}
