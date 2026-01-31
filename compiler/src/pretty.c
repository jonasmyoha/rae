/* pretty.c - Rae pretty printer implementation */

#include "pretty.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "lexer.h"

typedef struct {
  size_t start_line;
  size_t end_line;
} VerbatimRange;

typedef struct {
  FILE* out;
  int indent;
  int start_of_line;
  int current_col;
  const Token* comments;
  size_t comment_count;
  size_t next_comment_idx;
  const char* source;
  VerbatimRange verbatim_ranges[64];
  size_t verbatim_count;
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

static void pp_write_string_literal(PrettyPrinter* pp, Str s) {
  pp_write_char(pp, '"');
  for (size_t i = 0; i < s.len; i++) {
    char c = s.data[i];
    switch (c) {
      case '"': pp_write(pp, "\""); break;
      case '\\': pp_write(pp, "\\\\"); break;
      case '\n': pp_write(pp, "\\n"); break;
      case '\r': pp_write(pp, "\\r"); break;
      case '\t': pp_write(pp, "\\t"); break;
      default: pp_write_char(pp, c); break;
    }
  }
  pp_write_char(pp, '"');
}

static const char* get_line_ptr(const char* source, size_t line) {
  if (!source) return NULL;
  const char* p = source;
  size_t current = 1;
  while (current < line && *p) {
    if (*p == '\n') current++;
    p++;
  }
  return p;
}

static void pp_print_verbatim_range(PrettyPrinter* pp, size_t start_line, size_t end_line) {
  const char* start = get_line_ptr(pp->source, start_line);
  const char* end = get_line_ptr(pp->source, end_line + 1);
  if (start && end && end > start) {
    fwrite(start, 1, end - start, pp->out);
    pp->start_of_line = 1;
    pp->current_col = 0;
  }
}

static VerbatimRange* find_range_for_line(PrettyPrinter* pp, size_t line) {
  for (size_t i = 0; i < pp->verbatim_count; i++) {
    if (line >= pp->verbatim_ranges[i].start_line && line <= pp->verbatim_ranges[i].end_line) {
      return &pp->verbatim_ranges[i];
    }
  }
  return NULL;
}

static void pp_check_comments(PrettyPrinter* pp, size_t line) {
  while (pp->next_comment_idx < pp->comment_count &&
         pp->comments[pp->next_comment_idx].line <= line) {
    const Token* comment = &pp->comments[pp->next_comment_idx++];
    
    if (find_range_for_line(pp, comment->line)) {
        continue; 
    }

    if (!pp->start_of_line) {
        pp_newline(pp);
    }
    
    if (comment->kind == TOK_COMMENT) {
        const char* text = comment->lexeme.data;
        size_t len = comment->lexeme.len;
        if (len > 0 && text[0] == '#') { text++; len--; } 
        
        size_t pos = 0;
        while (pos < len) {
            pp_write(pp, "#");
            size_t remaining = len - pos;
            size_t to_write = remaining;
            if (pp->indent * 2 + 1 + remaining > 120) {
                to_write = 120 - (pp->indent * 2 + 1);
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
        pp_write_str(pp, comment->lexeme);
        pp_newline(pp);
    }
  }
}

static void pp_write_type(PrettyPrinter* pp, const AstTypeRef* type) {
  if (!type) {
    pp_write(pp, "<type>");
    return;
  }
  if (type->is_opt) pp_write(pp, "opt ");
  if (type->is_view) pp_write(pp, "view ");
  if (type->is_mod) pp_write(pp, "mod ");
  if (type->is_id) pp_write(pp, "id ");
  if (type->is_key) pp_write(pp, "key ");
  
  if (!type->parts) {
    pp_write(pp, "<base>");
  } else {
    const AstIdentifierPart* part = type->parts;
    int is_first_part = 1;
    while (part) {
      if (!is_first_part) {
        pp_space(pp);
      }
      pp_write_str(pp, part->text);
      is_first_part = 0;
      part = part->next;
    }
  }
  if (type->generic_args) {
    pp_write_char(pp, '(');
    AstTypeRef* generic_param = type->generic_args;
    int is_first_generic_param = 1;
    while (generic_param) {
      if (!is_first_generic_param) {
        pp_write(pp, ", ");
      }
      pp_write_type(pp, generic_param);
      is_first_generic_param = 0;
      generic_param = generic_param->next;
    }
    pp_write_char(pp, ')');
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
  PREC_ATOMIC = 9
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
    case AST_UNARY_POST_INC: return "++";
    case AST_UNARY_POST_DEC: return "--";
    case AST_UNARY_VIEW: return "view ";
    case AST_UNARY_MOD: return "mod ";
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
      if (current->name.len > 0) {
        pp_write_str(pp, current->name);
        pp_write(pp, ": ");
      }
      pp_expr_prec(pp, current->value, PREC_LOWEST);
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
      if (current->name.len > 0) {
        pp_write_str(pp, current->name);
        pp_write(pp, ": ");
      }
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
    case AST_EXPR_INTERP: {
      pp_write_char(pp, '"');
      AstInterpPart* part = expr->as.interp.parts;
      while (part) {
          if (part->value->kind == AST_EXPR_STRING) {
              pp_write_str(pp, part->value->as.string_lit);
          } else {
              pp_write_char(pp, '{');
              pp_expr_prec(pp, part->value, PREC_LOWEST);
              pp_write_char(pp, '}');
          }
          part = part->next;
      }
      pp_write_char(pp, '"');
      break;
    }
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
      if (expr->as.object_literal.type) {
        pp_write_type(pp, expr->as.object_literal.type);
        pp_space(pp);
      }
      pp_write_char(pp, '{');
      if (expr->as.object_literal.fields) {
        bool wrap = false;
        int count = 0;
        AstObjectField* scan = expr->as.object_literal.fields;
        while (scan) { count++; scan = scan->next; }
        if (count > 3) wrap = true;

        if (wrap) {
          pp_newline(pp);
          pp->indent++;
          AstObjectField* field = expr->as.object_literal.fields;
          while (field) {
            pp_write_str(pp, field->name);
            pp_write(pp, ": ");
            pp_expr_prec(pp, field->value, PREC_LOWEST);
            pp_newline(pp);
            field = field->next;
          }
          pp->indent--;
          pp_write_indent(pp);
        } else {
          pp_space(pp);
          AstObjectField* field = expr->as.object_literal.fields;
          int first = 1;
          while (field) {
            if (!first) pp_write(pp, ", ");
            pp_write_str(pp, field->name);
            pp_write(pp, ": ");
            pp_expr_prec(pp, field->value, PREC_LOWEST);
            first = 0;
            field = field->next;
          }
          pp_space(pp);
        }
      }
      pp_write_char(pp, '}');
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
      
      bool is_post = (expr->as.unary.op == AST_UNARY_POST_INC || expr->as.unary.op == AST_UNARY_POST_DEC);
      const char* op_text = unary_op_text(expr->as.unary.op);
      
      if (!is_post) {
          pp_write(pp, op_text);
          if (expr->as.unary.op == AST_UNARY_NOT || expr->as.unary.op == AST_UNARY_SPAWN ||
              expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
            pp_space(pp);
          }
      }
      
      pp_expr_prec(pp, expr->as.unary.operand, PREC_UNARY);
      
      if (is_post) {
          pp_write(pp, op_text);
      }
      
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
    case AST_EXPR_METHOD_CALL: {
      int prec = PREC_CALL;
      int need_paren = prec < parent_prec;
      if (need_paren) pp_write_char(pp, '(');
      pp_expr_prec(pp, expr->as.method_call.object, PREC_CALL);
      pp_write_char(pp, '.');
      pp_write_str(pp, expr->as.method_call.method_name);
      pp_write_char(pp, '(');
      pp_call_args(pp, expr->as.method_call.args);
      pp_write_char(pp, ')');
      if (need_paren) pp_write_char(pp, ')');
      break;
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      if (expr->as.collection.type) {
        pp_write_type(pp, expr->as.collection.type);
        pp_space(pp);
      }
      pp_write_char(pp, '{');
      AstCollectionElement* current = expr->as.collection.elements;
      if (current) {
        bool wrap = false;
        int count = 0;
        AstCollectionElement* scan = current;
        while (scan) { count++; scan = scan->next; }
        if (count > 3) wrap = true;

        if (wrap) {
          pp_newline(pp);
          pp->indent++;
          while (current) {
            if (current->key) {
              pp_write_str(pp, *current->key);
              pp_write(pp, ": ");
            }
            pp_expr_prec(pp, current->value, PREC_LOWEST);
            pp_newline(pp);
            current = current->next;
          }
          pp->indent--;
          pp_write_indent(pp);
        } else {
          pp_space(pp);
          int first = 1;
          while (current) {
            if (!first) { pp_write(pp, ", "); }
            if (current->key) {
              pp_write_str(pp, *current->key);
              pp_write(pp, ": ");
            }
            pp_expr_prec(pp, current->value, PREC_LOWEST);
            first = 0;
            current = current->next;
          }
          pp_space(pp);
        }
      }
      pp_write_char(pp, '}');
      break;
    }
    case AST_EXPR_LIST: {
      pp_write_char(pp, '[');
      AstExprList* current = expr->as.list;
      if (current) {
        bool wrap = false;
        int count = 0;
        AstExprList* scan = current;
        while (scan) { count++; scan = scan->next; }
        if (count > 5) wrap = true;

        if (wrap) {
          pp_newline(pp);
          pp->indent++;
          while (current) {
            pp_expr_prec(pp, current->value, PREC_LOWEST);
            pp_newline(pp);
            current = current->next;
          }
          pp->indent--;
          pp_write_indent(pp);
        } else {
          int first = 1;
          while (current) {
            if (!first) pp_write(pp, ", ");
            pp_expr_prec(pp, current->value, PREC_LOWEST);
            first = 0;
            current = current->next;
          }
        }
      }
      pp_write_char(pp, ']');
      break;
    }
    case AST_EXPR_INDEX: {
      pp_expr_prec(pp, expr->as.index.target, PREC_ATOMIC);
      pp_write(pp, "[");
      pp_expr_prec(pp, expr->as.index.index, PREC_LOWEST);
      pp_write(pp, "]");
      break;
    }
    default:
      pp_write(pp, "<expr>"); // Placeholder for unhandled expression types
      break;
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

static void pp_print_let_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_check_comments(pp, stmt->line);
  pp_write(pp, "let ");
  pp_write_str(pp, stmt->as.let_stmt.name);
  pp_write(pp, ": ");
  pp_write_type(pp, stmt->as.let_stmt.type);
  if (stmt->as.let_stmt.value) {
    if (stmt->as.let_stmt.is_bind) {
      pp_write(pp, " => ");
    } else {
      pp_write(pp, " = ");
    }
    pp_expr(pp, stmt->as.let_stmt.value);
  }
  pp_newline(pp);
}

static void pp_print_destruct_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_check_comments(pp, stmt->line);
  const AstDestructureBinding* binding = stmt->as.destruct_stmt.bindings;
  int first = 1;
  while (binding) {
    if (!first) { pp_write(pp, ", "); } 
    pp_write(pp, "let ");
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
    if (stmt->as.loop_stmt.init->kind == AST_STMT_LET) {
      pp_write(pp, "let ");
      pp_write_str(pp, stmt->as.loop_stmt.init->as.let_stmt.name);
      pp_write(pp, ": ");
      pp_write_type(pp, stmt->as.loop_stmt.init->as.let_stmt.type);
      if (stmt->as.loop_stmt.init->as.let_stmt.value) {
        pp_write(pp, " = ");
        pp_expr(pp, stmt->as.loop_stmt.init->as.let_stmt.value);
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
  if (stmt->as.assign_stmt.is_bind) {
    pp_write(pp, " => ");
  } else {
    pp_write(pp, " = ");
  }
  pp_expr(pp, stmt->as.assign_stmt.value);
  pp_newline(pp);
}

static void pp_print_defer_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_check_comments(pp, stmt->line);
  pp_write(pp, "defer ");
  pp_begin_block(pp);
  pp_print_block_body(pp, stmt->as.defer_stmt.block);
  pp_end_block(pp);
  pp_newline(pp);
}

static void pp_print_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  switch (stmt->kind) {
    case AST_STMT_LET: pp_print_let_stmt(pp, stmt); break;
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
    case AST_STMT_DEFER: pp_print_defer_stmt(pp, stmt); break;
  }
}

static void pp_print_type_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_check_comments(pp, decl->line);
  pp_write(pp, "type ");
  pp_write_str(pp, decl->as.type_decl.name);
  if (decl->as.type_decl.generic_params) {
    pp_write_char(pp, '(');
    AstIdentifierPart* current = decl->as.type_decl.generic_params;
    int first = 1;
    while (current) {
      if (!first) {
        pp_write(pp, ", ");
      }
      pp_write_str(pp, current->text);
      first = 0;
      current = current->next;
    }
    pp_write_char(pp, ')');
  }
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
  pp_write(pp, "func ");
  pp_write_str(pp, decl->as.func_decl.name);
  if (decl->as.func_decl.generic_params) {
    pp_write_char(pp, '(');
    AstIdentifierPart* current = decl->as.func_decl.generic_params;
    int first = 1;
    while (current) {
      if (!first) {
        pp_write(pp, ", ");
      }
      pp_write_str(pp, current->text);
      first = 0;
      current = current->next;
    }
    pp_write_char(pp, ')');
  }
  pp_write(pp, "(");
  pp_print_params(pp, decl->as.func_decl.params);
  pp_write(pp, ")");
  
  if (decl->as.func_decl.properties) {
    pp_space(pp);
    pp_write_properties(pp, decl->as.func_decl.properties);
  }

  if (decl->as.func_decl.returns) {
    pp_space(pp);
    pp_write(pp, "ret ");
    pp_print_return_items(pp, decl->as.func_decl.returns);
  }

  if (decl->as.func_decl.body) {
      pp_space(pp);
      pp_begin_block(pp);
      pp_print_block_body(pp, decl->as.func_decl.body);
      pp_end_block(pp);
  }
  pp_newline(pp);
}

static void pp_print_enum_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "enum ");
  pp_write_str(pp, decl->as.enum_decl.name);
  pp_space(pp);
  
  bool wrap = false;
  int count = 0;
  AstEnumMember* m = decl->as.enum_decl.members;
  while (m) {
    count++;
    m = m->next;
  }
  if (count > 5) wrap = true;

  if (wrap) {
    pp_begin_block(pp);
    m = decl->as.enum_decl.members;
    while (m) {
      pp_write_str(pp, m->name);
      pp_newline(pp);
      m = m->next;
    }
    pp_end_block(pp);
  } else {
    pp_write(pp, "{ ");
    m = decl->as.enum_decl.members;
    int first = 1;
    while (m) {
      if (!first) pp_write(pp, ", ");
      pp_write_str(pp, m->name);
      first = 0;
      m = m->next;
    }
    pp_write(pp, " }");
  }
  pp_newline(pp);
}

static void pp_print_decl(PrettyPrinter* pp, const AstDecl* decl) {
  if (decl->kind == AST_DECL_TYPE) {
    pp_print_type_decl(pp, decl);
  } else if (decl->kind == AST_DECL_ENUM) {
    pp_print_enum_decl(pp, decl);
  } else {
    pp_print_func_decl(pp, decl);
  }
}

void pretty_print_module(const AstModule* module, const char* source, FILE* out) {
  PrettyPrinter pp = {
      .out = out,
      .indent = 0,
      .start_of_line = 1,
      .current_col = 0,
      .comments = module->comments,
      .comment_count = module->comment_count,
      .next_comment_idx = 0,
      .source = source,
      .verbatim_count = 0,
  };

  if (!module) return;

  size_t off_line = 0;
  for (size_t i = 0; i < module->comment_count; i++) {
    const Token* c = &module->comments[i];
    if (c->kind == TOK_COMMENT) {
      if (strstr(c->lexeme.data, "raefmt: off")) {
        if (off_line == 0) off_line = c->line;
      } else if (strstr(c->lexeme.data, "raefmt: on")) {
        if (off_line != 0) {
          if (pp.verbatim_count < 64) {
            pp.verbatim_ranges[pp.verbatim_count++] = (VerbatimRange){off_line, c->line};
          }
          off_line = 0;
        }
      }
    }
  }
  if (off_line != 0) {
    if (pp.verbatim_count < 64) {
      pp.verbatim_ranges[pp.verbatim_count++] = (VerbatimRange){off_line, (size_t)-1};
    }
  }

  const AstImport* imp = module->imports;
  size_t last_verbatim_end = 0;

  while (imp) {
    VerbatimRange* r = find_range_for_line(&pp, imp->line);
    if (r) {
      if (r->end_line > last_verbatim_end) {
        pp_print_verbatim_range(&pp, r->start_line, r->end_line);
        last_verbatim_end = r->end_line;
      }
      while (imp && imp->line <= r->end_line) imp = imp->next;
      continue;
    }

    pp_check_comments(&pp, imp->line);
    pp_write(&pp, imp->is_export ? "export " : "import ");
    pp_write_str(&pp, imp->path);
    pp_newline(&pp);
    imp = imp->next;
  }

  const AstDecl* decl = module->decls;
  int first = 1;
  while (decl) {
    VerbatimRange* r = find_range_for_line(&pp, decl->line);
    if (r) {
      if (r->end_line > last_verbatim_end) {
        if (!first) pp_newline(&pp);
        pp_print_verbatim_range(&pp, r->start_line, r->end_line);
        last_verbatim_end = r->end_line;
        first = 0;
      }
      while (decl && decl->line <= r->end_line) decl = decl->next;
      continue;
    }

    if (!first) pp_newline(&pp);
    pp_print_decl(&pp, decl);
    first = 0;
    decl = decl->next;
  }
  
  pp_check_comments(&pp, (size_t)-1);

  if (!pp.start_of_line) {
    pp_newline(&pp);
  }
}
