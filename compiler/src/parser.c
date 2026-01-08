/* parser.c - Rae parser implementation */

#include "parser.h"

#include "diag.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  Arena* arena;
  const char* file_path;
  const Token* tokens;
  size_t count;
  size_t index;
} Parser;

typedef struct {
  int precedence;
  AstBinaryOp op;
} BinaryInfo;

static void parser_error(Parser* parser, const Token* token, const char* fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (!token) {
    diag_error(parser->file_path, 0, 0, buffer);
  } else {
    diag_error(parser->file_path, (int)token->line, (int)token->column, buffer);
  }
}

static const Token* parser_peek(Parser* parser) {
  if (parser->index >= parser->count) {
    return &parser->tokens[parser->count - 1];
  }
  return &parser->tokens[parser->index];
}

static const Token* parser_previous(Parser* parser) {
  if (parser->index == 0) {
    return parser_peek(parser);
  }
  return &parser->tokens[parser->index - 1];
}

static const Token* parser_peek_at(Parser* parser, size_t offset) {
  size_t idx = parser->index + offset;
  if (idx >= parser->count) {
    return &parser->tokens[parser->count - 1];
  }
  return &parser->tokens[idx];
}

static bool parser_check(Parser* parser, TokenKind kind) {
  return parser_peek(parser)->kind == kind;
}

static const Token* parser_advance(Parser* parser) {
  if (parser->index < parser->count) {
    parser->index += 1;
  }
  return parser_previous(parser);
}

static bool parser_match(Parser* parser, TokenKind kind) {
  if (!parser_check(parser, kind)) {
    return false;
  }
  parser_advance(parser);
  return true;
}

static const Token* parser_consume(Parser* parser, TokenKind kind, const char* message) {
  if (parser_check(parser, kind)) {
    return parser_advance(parser);
  }
  parser_error(parser, parser_peek(parser), message);
  return NULL;
}

static void* parser_alloc(Parser* parser, size_t size) {
  void* result = arena_alloc(parser->arena, size);
  if (!result) {
    diag_fatal("parser out of memory");
  }
  memset(result, 0, size);
  return result;
}

static Str parser_copy_str(Parser* parser, Str value) {
  if (value.len == 0) {
    return value;
  }
  char* buffer = parser_alloc(parser, value.len + 1);
  memcpy(buffer, value.data, value.len);
  buffer[value.len] = '\0';
  return (Str){.data = buffer, .len = value.len};
}

static AstExpr* new_expr(Parser* parser, AstExprKind kind, const Token* token) {
  AstExpr* expr = parser_alloc(parser, sizeof(AstExpr));
  expr->kind = kind;
  expr->is_raw = false;
  if (token) {
    expr->line = token->line;
    expr->column = token->column;
  }
  return expr;
}

static AstStmt* new_stmt(Parser* parser, AstStmtKind kind, const Token* token) {
  AstStmt* stmt = parser_alloc(parser, sizeof(AstStmt));
  stmt->kind = kind;
  if (token) {
    stmt->line = token->line;
    stmt->column = token->column;
  }
  return stmt;
}

static AstIdentifierPart* make_identifier_part(Parser* parser, Str text) {
  AstIdentifierPart* part = parser_alloc(parser, sizeof(AstIdentifierPart));
  part->text = parser_copy_str(parser, text);
  return part;
}

static bool is_type_modifier(TokenKind kind) {
  return kind == TOK_KW_OWN || kind == TOK_KW_VIEW || kind == TOK_KW_MOD || kind == TOK_KW_OPT;
}

static AstTypeRef* parse_type_ref(Parser* parser) {
  AstTypeRef* type = parser_alloc(parser, sizeof(AstTypeRef));
  AstIdentifierPart* head = NULL;
  AstIdentifierPart* tail = NULL;
  bool consumed_base = false;
  while (true) {
    TokenKind kind = parser_peek(parser)->kind;
    if (is_type_modifier(kind)) {
      const Token* tok = parser_advance(parser);
      AstIdentifierPart* part = make_identifier_part(parser, tok->lexeme);
      if (!head) {
        head = tail = part;
      } else {
        tail->next = part;
        tail = part;
      }
      continue;
    }
    if (kind == TOK_IDENT) {
      const Token* tok = parser_advance(parser);
      AstIdentifierPart* part = make_identifier_part(parser, tok->lexeme);
      if (!head) {
        head = tail = part;
      } else {
        tail->next = part;
        tail = part;
      }
      consumed_base = true;
      break;
    }
    break;
  }
  if (!consumed_base) {
    parser_error(parser, parser_peek(parser), "expected type");
  }
  type->parts = head;
  return type;
}

static AstProperty* append_property(AstProperty* head, AstProperty* node) {
  if (!head) {
    return node;
  }
  AstProperty* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstProperty* make_property(Parser* parser, Str name) {
  AstProperty* prop = parser_alloc(parser, sizeof(AstProperty));
  prop->name = parser_copy_str(parser, name);
  return prop;
}

static AstProperty* parse_type_properties(Parser* parser) {
  AstProperty* head = NULL;
  while (parser_check(parser, TOK_KW_PUB) || parser_check(parser, TOK_KW_PRIV)) {
    const Token* token = parser_advance(parser);
    head = append_property(head, make_property(parser, token->lexeme));
  }
  return head;
}

static AstProperty* parse_func_properties(Parser* parser) {
  AstProperty* head = NULL;
  while (parser_check(parser, TOK_KW_PUB) || parser_check(parser, TOK_KW_PRIV) || parser_check(parser, TOK_KW_SPAWN)) {
    const Token* token = parser_advance(parser);
    head = append_property(head, make_property(parser, token->lexeme));
  }
  return head;
}

static AstImport* append_import(AstImport* head, AstImport* node) {
  if (!node) {
    return head;
  }
  if (!head) {
    return node;
  }
  AstImport* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static Str parse_import_path_spec(Parser* parser) {
  size_t len = 0;
  size_t tokens_to_consume = 0;
  
  while (true) {
    const Token* t = parser_peek_at(parser, tokens_to_consume);
    if (!t || t->kind == TOK_EOF) break;
    
    bool is_path_char = false;
    if (t->kind == TOK_IDENT || t->kind == TOK_INTEGER) is_path_char = true;
    else if (t->kind == TOK_DOT || t->kind == TOK_SLASH) is_path_char = true;
    else if (t->kind == TOK_MINUS || t->kind == TOK_PLUS) is_path_char = true;
    
    // Stop at keywords that start a new declaration, unless part of a path (preceded by separator)
    if (t->kind == TOK_KW_FUNC || t->kind == TOK_KW_TYPE || 
        t->kind == TOK_KW_IMPORT || t->kind == TOK_KW_EXPORT ||
        t->kind == TOK_KW_PUB || t->kind == TOK_KW_PRIV || 
        t->kind == TOK_KW_EXTERN) {
        if (tokens_to_consume > 0) {
            const Token* prev = parser_peek_at(parser, tokens_to_consume - 1);
            bool prev_sep = (prev->kind == TOK_SLASH || prev->kind == TOK_DOT);
            if (!prev_sep) break;
        } else {
            break;
        }
        is_path_char = true; // Keyword accepted as path component
    }
    
    if (!is_path_char) break;
    
    if (tokens_to_consume > 0) {
        const Token* prev = parser_peek_at(parser, tokens_to_consume - 1);
        bool prev_ident = (prev->kind == TOK_IDENT || prev->kind == TOK_INTEGER);
        bool curr_ident = (t->kind == TOK_IDENT || t->kind == TOK_INTEGER);
        if (prev_ident && curr_ident) break;
    }
    
    len += t->lexeme.len;
    tokens_to_consume++;
  }
  
  if (tokens_to_consume == 0) {
    parser_error(parser, parser_peek(parser), "expected module path");
    return (Str){.data="", .len=0};
  }
  
  char* buffer = parser_alloc(parser, len + 1);
  char* cursor = buffer;
  for (size_t i = 0; i < tokens_to_consume; ++i) {
    const Token* t = parser_advance(parser);
    memcpy(cursor, t->lexeme.data, t->lexeme.len);
    cursor += t->lexeme.len;
  }
  *cursor = '\0';
  return (Str){.data = buffer, .len = len};
}

static AstImport* parse_import_clause(Parser* parser, bool is_export) {
  const Token* start = parser_previous(parser);
  Str path = parse_import_path_spec(parser);
  if (path.len == 0) {
    return NULL;
  }
  AstImport* clause = parser_alloc(parser, sizeof(AstImport));
  clause->is_export = is_export;
  clause->path = path;
  clause->line = start->line;
  clause->column = start->column;
  return clause;
}

static AstParam* append_param(AstParam* head, AstParam* node) {
  if (!head) {
    return node;
  }
  AstParam* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstParam* parse_param(Parser* parser) {
  const Token* name = parser_consume(parser, TOK_IDENT, "expected parameter name");
  parser_consume(parser, TOK_COLON, "expected ':' after parameter name");
  AstParam* param = parser_alloc(parser, sizeof(AstParam));
  param->name = parser_copy_str(parser, name->lexeme);
  param->type = parse_type_ref(parser);
  return param;
}

static AstParam* parse_param_list(Parser* parser) {
  parser_consume(parser, TOK_LPAREN, "expected '(' after function name");
  if (parser_match(parser, TOK_RPAREN)) {
    return NULL;
  }
  AstParam* head = NULL;
  for (;;) {
    head = append_param(head, parse_param(parser));
    if (parser_match(parser, TOK_RPAREN)) {
      break;
    }
    parser_consume(parser, TOK_COMMA, "expected ',' between parameters");
    if (parser_match(parser, TOK_RPAREN)) {
      break;
    }
  }
  return head;
}

static AstReturnItem* append_return_item(AstReturnItem* head, AstReturnItem* node) {
  if (!head) {
    return node;
  }
  AstReturnItem* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstReturnItem* parse_return_clause(Parser* parser) {
  AstReturnItem* head = NULL;
  do {
    AstReturnItem* item = parser_alloc(parser, sizeof(AstReturnItem));
    if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
      const Token* label = parser_advance(parser);
      parser_consume(parser, TOK_COLON, "expected ':' after return label");
      item->has_name = true;
      item->name = parser_copy_str(parser, label->lexeme);
    }
    item->type = parse_type_ref(parser);
    head = append_return_item(head, item);
    if (parser_check(parser, TOK_LBRACE) || parser_check(parser, TOK_EOF)) break;
    // Also check for next top level decl or end of return clause markers if any
    // Actually, return clause is usually followed by a { or newline.
  } while (parser_match(parser, TOK_COMMA));
  return head;
}

static AstObjectField* append_object_field(AstObjectField* head, AstObjectField* node) {
  if (!head) {
    return node;
  }
  AstObjectField* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstCallArg* append_call_arg(AstCallArg* head, AstCallArg* node) {
  if (!head) {
    return node;
  }
  AstCallArg* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstStmt* append_stmt(AstStmt* head, AstStmt* node) {
  if (!head) {
    return node;
  }
  AstStmt* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstDestructureBinding* append_destructure_binding(AstDestructureBinding* head,
                                                         AstDestructureBinding* node) {
  if (!head) {
    return node;
  }
  AstDestructureBinding* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstExpr* parse_expression(Parser* parser);
static AstBlock* parse_block(Parser* parser);
static AstStmt* parse_statement(Parser* parser);
static AstExpr* parse_match_expression(Parser* parser, const Token* match_token);

static bool token_is_ident(const Token* token, const char* text) {
  if (!token || token->kind != TOK_IDENT) return false;
  size_t len = strlen(text);
  if (token->lexeme.len != len) return false;
  return strncmp(token->lexeme.data, text, len) == 0;
}

static bool is_unary_operator(TokenKind kind) {
  return kind == TOK_MINUS || kind == TOK_KW_NOT || kind == TOK_KW_SPAWN || kind == TOK_INC || kind == TOK_DEC;
}

static BinaryInfo get_binary_info(TokenKind kind) {
  switch (kind) {
    case TOK_PLUS:
      return (BinaryInfo){.precedence = 4, .op = AST_BIN_ADD};
    case TOK_MINUS:
      return (BinaryInfo){.precedence = 4, .op = AST_BIN_SUB};
    case TOK_STAR:
      return (BinaryInfo){.precedence = 5, .op = AST_BIN_MUL};
    case TOK_SLASH:
      return (BinaryInfo){.precedence = 5, .op = AST_BIN_DIV};
    case TOK_PERCENT:
      return (BinaryInfo){.precedence = 5, .op = AST_BIN_MOD};
    case TOK_LESS:
      return (BinaryInfo){.precedence = 3, .op = AST_BIN_LT};
    case TOK_GREATER:
      return (BinaryInfo){.precedence = 3, .op = AST_BIN_GT};
    case TOK_LESS_EQUAL:
      return (BinaryInfo){.precedence = 3, .op = AST_BIN_LE};
    case TOK_GREATER_EQUAL:
      return (BinaryInfo){.precedence = 3, .op = AST_BIN_GE};
    case TOK_KW_IS:
      return (BinaryInfo){.precedence = 2, .op = AST_BIN_IS};
    case TOK_KW_AND:
      return (BinaryInfo){.precedence = 1, .op = AST_BIN_AND};
    case TOK_KW_OR:
      return (BinaryInfo){.precedence = 0, .op = AST_BIN_OR};
    default:
      return (BinaryInfo){.precedence = -1};
  }
}

static AstExpr* parse_primary(Parser* parser);

static bool callee_allows_value_shorthand(const AstExpr* callee) {
  if (!callee || callee->kind != AST_EXPR_IDENT) return false;
  return str_eq_cstr(callee->as.ident, "log") || str_eq_cstr(callee->as.ident, "logS");
}

static AstExpr* finish_call(Parser* parser, AstExpr* callee, const Token* start_token) {
  AstExpr* expr = new_expr(parser, AST_EXPR_CALL, start_token);
  expr->as.call.callee = callee;
  if (parser_match(parser, TOK_RPAREN)) {
    return expr;
  }
  bool allow_shorthand = callee_allows_value_shorthand(callee);
  bool used_shorthand = false;
  AstCallArg* args = NULL;
  do {
    AstCallArg* arg = parser_alloc(parser, sizeof(AstCallArg));
    if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
      const Token* name = parser_advance(parser);
      parser_consume(parser, TOK_COLON, "expected ':' after argument name");
      arg->name = parser_copy_str(parser, name->lexeme);
      arg->value = parse_expression(parser);
    } else if (allow_shorthand && !used_shorthand) {
      arg->name = str_from_cstr("value");
      arg->value = parse_expression(parser);
      used_shorthand = true;
    } else {
      parser_error(parser, parser_peek(parser), "expected argument name");
      arg->name = str_from_cstr("<error>");
      arg->value = parse_expression(parser);
    }
    args = append_call_arg(args, arg);
    if (parser_check(parser, TOK_RPAREN)) break;
    parser_consume(parser, TOK_COMMA, "expected ',' between arguments");
    if (parser_check(parser, TOK_RPAREN)) break;
  } while (true);
  parser_consume(parser, TOK_RPAREN, "expected ')' after arguments");
  expr->as.call.args = args;
  return expr;
}

static AstExpr* parse_object_literal(Parser* parser, const Token* start_token) {
  AstExpr* expr = new_expr(parser, AST_EXPR_OBJECT, start_token);
  AstObjectField* fields = NULL;
  if (parser_match(parser, TOK_RPAREN)) {
    expr->as.object = NULL;
    return expr;
  }
  do {
    const Token* name = parser_consume(parser, TOK_IDENT, "expected field name in object literal");
    parser_consume(parser, TOK_COLON, "expected ':' after field name");
    AstObjectField* field = parser_alloc(parser, sizeof(AstObjectField));
    field->name = parser_copy_str(parser, name->lexeme);
    field->value = parse_expression(parser);
    fields = append_object_field(fields, field);
    if (parser_check(parser, TOK_RPAREN)) break;
    parser_consume(parser, TOK_COMMA, "expected ',' between fields");
    if (parser_check(parser, TOK_RPAREN)) break;
  } while (true);
  parser_consume(parser, TOK_RPAREN, "expected ')' after object literal");
  expr->as.object = fields;
  return expr;
}

static AstExpr* parse_group_or_object(Parser* parser) {
  const Token* start = parser_previous(parser);
  if (parser_check(parser, TOK_RPAREN)) {
    parser_error(parser, parser_peek(parser), "unexpected ')'");
  }
  bool is_object = false;
  if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
    is_object = true;
  }
  if (is_object) {
    return parse_object_literal(parser, start);
  }
  AstExpr* inner = parse_expression(parser);
  parser_consume(parser, TOK_RPAREN, "expected ')' after expression");
  return inner;
}

static AstExpr* parse_postfix(Parser* parser) {
  AstExpr* expr = parse_primary(parser);
  for (;;) {
    if (parser_match(parser, TOK_LPAREN)) {
      expr = finish_call(parser, expr, parser_previous(parser));
      continue;
    }
    if (parser_match(parser, TOK_DOT)) {
      const Token* name = parser_consume(parser, TOK_IDENT, "expected member name after '.'");
      AstExpr* member = new_expr(parser, AST_EXPR_MEMBER, parser_previous(parser));
      member->as.member.object = expr;
      member->as.member.member = parser_copy_str(parser, name->lexeme);
      expr = member;
      continue;
    }
    break;
  }
  return expr;
}

static AstExpr* parse_unary(Parser* parser) {
  if (is_unary_operator(parser_peek(parser)->kind)) {
    const Token* op_token = parser_advance(parser);
    AstExpr* operand = parse_unary(parser);
    AstExpr* expr = new_expr(parser, AST_EXPR_UNARY, op_token);
    expr->as.unary.operand = operand;
    switch (op_token->kind) {
      case TOK_MINUS:
        expr->as.unary.op = AST_UNARY_NEG;
        break;
      case TOK_KW_NOT:
        expr->as.unary.op = AST_UNARY_NOT;
        break;
      case TOK_KW_SPAWN:
        expr->as.unary.op = AST_UNARY_SPAWN;
        break;
      case TOK_INC:
        expr->as.unary.op = AST_UNARY_PRE_INC;
        break;
      case TOK_DEC:
        expr->as.unary.op = AST_UNARY_PRE_DEC;
        break;
      default:
        parser_error(parser, op_token, "unsupported unary operator");
        break;
    }
    return expr;
  }
  return parse_postfix(parser);
}

static AstExpr* parse_binary(Parser* parser, int min_prec) {
  AstExpr* left = parse_unary(parser);
  for (;;) {
    BinaryInfo info = get_binary_info(parser_peek(parser)->kind);
    if (info.precedence < min_prec) {
      break;
    }
    const Token* op_token = parser_advance(parser);
    AstExpr* right = parse_binary(parser, info.precedence + 1);
    AstExpr* binary = new_expr(parser, AST_EXPR_BINARY, op_token);
    binary->as.binary.op = info.op;
    binary->as.binary.lhs = left;
    binary->as.binary.rhs = right;
    left = binary;
  }
  return left;
}

static AstExpr* parse_expression(Parser* parser) {
  return parse_binary(parser, 0);
}

static AstExpr* parse_primary(Parser* parser) {
  const Token* token = parser_peek(parser);
  switch (token->kind) {
    case TOK_IDENT: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_IDENT, token);
      expr->as.ident = parser_copy_str(parser, token->lexeme);
      return expr;
    }
    case TOK_INTEGER: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_INTEGER, token);
      expr->as.integer = parser_copy_str(parser, token->lexeme);
      return expr;
    }
    case TOK_FLOAT: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_FLOAT, token);
      expr->as.floating = parser_copy_str(parser, token->lexeme);
      return expr;
    }
    case TOK_STRING: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_STRING, token);
      expr->as.string_lit = parser_copy_str(parser, token->lexeme);
      return expr;
    }
    case TOK_RAW_STRING: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_STRING, token);
      expr->is_raw = true;
      
      const char* data = token->lexeme.data;
      size_t len = token->lexeme.len;
      size_t start = 1; // skip 'r'
      size_t hashes = 0;
      while (start + hashes < len && data[start + hashes] == '#') hashes++;
      start += hashes + 1; // skip hashes and '"'
      
      size_t content_len = len - start - hashes - 1; // -1 for closing '"'
      Str content = {.data = data + start, .len = content_len};
      expr->as.string_lit = parser_copy_str(parser, content);
      return expr;
    }
    case TOK_KW_TRUE:
    case TOK_KW_FALSE: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_BOOL, token);
      expr->as.boolean = (token->kind == TOK_KW_TRUE);
      return expr;
    }
    case TOK_KW_NONE: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_NONE, token);
      return expr;
    }
    case TOK_LPAREN:
      parser_advance(parser);
      return parse_group_or_object(parser);
    case TOK_KW_MATCH: {
      const Token* match_token = parser_advance(parser);
      return parse_match_expression(parser, match_token);
    }
    default:
      parser_error(parser, token, "unexpected token in expression");
      return NULL;
  }
}

static AstReturnArg* append_return_arg(AstReturnArg* head, AstReturnArg* node) {
  if (!head) {
    return node;
  }
  AstReturnArg* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstReturnArg* parse_return_values(Parser* parser) {
  AstReturnArg* head = NULL;
  do {
    AstReturnArg* arg = parser_alloc(parser, sizeof(AstReturnArg));
    if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
      const Token* label = parser_advance(parser);
      parser_consume(parser, TOK_COLON, "expected ':' after return label");
      arg->has_label = true;
      arg->label = parser_copy_str(parser, label->lexeme);
    }
    arg->value = parse_expression(parser);
    head = append_return_arg(head, arg);
  } while (parser_match(parser, TOK_COMMA));
  return head;
}

static void check_camel_case(Parser* parser, const Token* token, const char* context) {
  if (token && token->lexeme.len > 0) {
    char first = token->lexeme.data[0];
    if (first >= 'A' && first <= 'Z') {
      parser_error(parser, token, "%s name '%.*s' should be camelCase (start with lowercase)",
                   context, (int)token->lexeme.len, token->lexeme.data);
    }
  }
}

static AstStmt* parse_def_statement(Parser* parser, const Token* def_token) {
  const Token* name = parser_consume(parser, TOK_IDENT, "expected identifier after 'def'");
  check_camel_case(parser, name, "variable");
  parser_consume(parser, TOK_COLON, "expected ':' after local name");
  AstStmt* stmt = new_stmt(parser, AST_STMT_DEF, def_token);
  stmt->as.def_stmt.name = parser_copy_str(parser, name->lexeme);
  stmt->as.def_stmt.type = parse_type_ref(parser);
  if (parser_match(parser, TOK_ASSIGN)) {
    stmt->as.def_stmt.is_move = false;
  } else if (parser_match(parser, TOK_ARROW)) {
    stmt->as.def_stmt.is_move = true;
  } else {
    parser_error(parser, parser_peek(parser), "expected '=' or '=>' in definition");
  }
  stmt->as.def_stmt.value = parse_expression(parser);
  return stmt;
}

static AstStmt* parse_return_statement(Parser* parser, const Token* ret_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_RET, ret_token);
  if (parser_check(parser, TOK_RBRACE) || parser_check(parser, TOK_KW_CASE) || parser_check(parser, TOK_EOF)) {
    stmt->as.ret_stmt.values = NULL;
    return stmt;
  }
  stmt->as.ret_stmt.values = parse_return_values(parser);
  return stmt;
}

static bool looks_like_destructure(Parser* parser) {
  size_t i = parser->index;
  if (i >= parser->count || parser->tokens[i].kind != TOK_IDENT) {
    return false;
  }
  i++;
  if (i >= parser->count || parser->tokens[i].kind != TOK_COLON) {
    return false;
  }
  i++;
  if (i >= parser->count || parser->tokens[i].kind != TOK_IDENT) {
    return false;
  }
  i++;
  while (i < parser->count) {
    TokenKind kind = parser->tokens[i].kind;
    if (kind == TOK_COMMA) {
      if (i + 1 < parser->count && parser->tokens[i + 1].kind == TOK_KW_DEF) {
        return true;
      }
      return false;
    }
    if (kind == TOK_ASSIGN || kind == TOK_ARROW || kind == TOK_EOF || kind == TOK_RBRACE) {
      return false;
    }
    i++;
  }
  return false;
}

static bool expr_is_call_like(const AstExpr* expr) {
  if (!expr) {
    return false;
  }
  if (expr->kind == AST_EXPR_CALL) {
    return true;
  }
  if (expr->kind == AST_EXPR_UNARY && expr->as.unary.op == AST_UNARY_SPAWN) {
    return expr_is_call_like(expr->as.unary.operand);
  }
  return false;
}

static AstStmt* parse_destructure_statement(Parser* parser, const Token* def_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_DESTRUCT, def_token);
  AstDestructureBinding* bindings = NULL;
  size_t binding_count = 0;
  for (;;) {
    const Token* local = parser_consume(parser, TOK_IDENT, "expected local name in destructuring binding");
    parser_consume(parser, TOK_COLON, "expected ':' after local name in destructuring binding");
    const Token* label = parser_consume(parser, TOK_IDENT, "expected return label in destructuring binding");
    AstDestructureBinding* binding = parser_alloc(parser, sizeof(AstDestructureBinding));
    binding->local_name = parser_copy_str(parser, local->lexeme);
    binding->return_label = parser_copy_str(parser, label->lexeme);
    bindings = append_destructure_binding(bindings, binding);
    binding_count++;
    if (parser_match(parser, TOK_COMMA)) {
      parser_consume(parser, TOK_KW_DEF, "expected 'def' before next destructuring binding");
      continue;
    }
    break;
  }
  if (binding_count < 2) {
    parser_error(parser, def_token, "destructuring assignments require at least two bindings");
  }
  parser_consume(parser, TOK_ASSIGN, "destructuring assignments require '='");
  AstExpr* rhs = parse_expression(parser);
  if (!expr_is_call_like(rhs)) {
    parser_error(parser, def_token, "destructuring assignments require a call expression on the right-hand side");
  }
  stmt->as.destruct_stmt.bindings = bindings;
  stmt->as.destruct_stmt.call = rhs;
  return stmt;
}

static AstStmt* parse_if_statement(Parser* parser, const Token* if_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_IF, if_token);
  stmt->as.if_stmt.condition = parse_expression(parser);
  stmt->as.if_stmt.then_block = parse_block(parser);
  if (parser_match(parser, TOK_KW_ELSE)) {
    stmt->as.if_stmt.else_block = parse_block(parser);
  }
  return stmt;
}

static AstStmt* parse_loop_statement(Parser* parser, const Token* loop_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_LOOP, loop_token);
  stmt->as.loop_stmt.is_range = false;

  if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
    const Token* name = parser_advance(parser);
    parser_consume(parser, TOK_COLON, "expected ':' after identifier");
    AstTypeRef* type = parse_type_ref(parser);

    if (parser_match(parser, TOK_KW_IN)) {
      stmt->as.loop_stmt.is_range = true;
      AstStmt* init = new_stmt(parser, AST_STMT_DEF, name);
      init->as.def_stmt.name = parser_copy_str(parser, name->lexeme);
      init->as.def_stmt.type = type;
      init->as.def_stmt.value = NULL;
      init->as.def_stmt.is_move = false;

      stmt->as.loop_stmt.init = init;
      stmt->as.loop_stmt.condition = parse_expression(parser);
      stmt->as.loop_stmt.increment = NULL;
    } else {
      bool is_move = false;
      if (parser_match(parser, TOK_ASSIGN)) {
        is_move = false;
      } else if (parser_match(parser, TOK_ARROW)) {
        is_move = true;
      } else {
        parser_error(parser, parser_peek(parser), "expected '=' or 'in' in loop declaration");
      }

      AstStmt* init = new_stmt(parser, AST_STMT_DEF, name);
      init->as.def_stmt.name = parser_copy_str(parser, name->lexeme);
      init->as.def_stmt.type = type;
      init->as.def_stmt.value = parse_expression(parser);
      init->as.def_stmt.is_move = is_move;

      stmt->as.loop_stmt.init = init;
      parser_consume(parser, TOK_COMMA, "expected ',' after loop init");
      stmt->as.loop_stmt.condition = parse_expression(parser);
      parser_consume(parser, TOK_COMMA, "expected ',' after loop condition");
      stmt->as.loop_stmt.increment = parse_expression(parser);
    }
  } else {
    AstExpr* expr = parse_expression(parser);

    if (parser_match(parser, TOK_KW_IN)) {
      if (expr->kind != AST_EXPR_IDENT) {
        parser_error(parser, NULL, "range loop variable must be an identifier");
      }
      stmt->as.loop_stmt.is_range = true;
      AstStmt* init = new_stmt(parser, AST_STMT_DEF, NULL);
      init->as.def_stmt.name = parser_copy_str(parser, expr->as.ident);
      init->as.def_stmt.type = NULL;
      init->as.def_stmt.value = NULL;
      init->as.def_stmt.is_move = false;

      stmt->as.loop_stmt.init = init;
      stmt->as.loop_stmt.condition = parse_expression(parser);
      stmt->as.loop_stmt.increment = NULL;
    } else if (parser_match(parser, TOK_COMMA)) {
      AstStmt* init = new_stmt(parser, AST_STMT_EXPR, NULL);
      init->as.expr_stmt = expr;

      stmt->as.loop_stmt.init = init;
      stmt->as.loop_stmt.condition = parse_expression(parser);
      parser_consume(parser, TOK_COMMA, "expected ',' after loop condition");
      stmt->as.loop_stmt.increment = parse_expression(parser);
    } else {
      stmt->as.loop_stmt.init = NULL;
      stmt->as.loop_stmt.condition = expr;
      stmt->as.loop_stmt.increment = NULL;
    }
  }

  stmt->as.loop_stmt.body = parse_block(parser);
  return stmt;
}

static AstMatchCase* append_match_case(AstMatchCase* head, AstMatchCase* node) {
  if (!head) {
    return node;
  }
  AstMatchCase* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstMatchArm* append_match_arm(AstMatchArm* head, AstMatchArm* node) {
  if (!head) {
    return node;
  }
  AstMatchArm* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstStmt* parse_match_statement(Parser* parser, const Token* match_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_MATCH, match_token);
  stmt->as.match_stmt.subject = parse_expression(parser);
  parser_consume(parser, TOK_LBRACE, "expected '{' after match subject");
  AstMatchCase* cases = NULL;
  bool saw_default = false;
  while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
    AstMatchCase* match_case = parser_alloc(parser, sizeof(AstMatchCase));
    if (parser_match(parser, TOK_KW_CASE)) {
      match_case->pattern = parse_expression(parser);
    } else if (parser_match(parser, TOK_KW_DEFAULT)) {
      if (saw_default) {
        parser_error(parser, parser_peek(parser), "match already has a default arm");
      }
      saw_default = true;
      match_case->pattern = NULL;
    } else {
      parser_error(parser, parser_peek(parser), "expected 'case' or 'default' inside match");
      match_case->pattern = NULL;
    }
    match_case->block = parse_block(parser);
    cases = append_match_case(cases, match_case);
  }
  if (!cases) {
    parser_error(parser, parser_peek(parser), "match must have at least one case");
  }
  parser_consume(parser, TOK_RBRACE, "expected '}' after match cases");
  stmt->as.match_stmt.cases = cases;
  return stmt;
}

static AstExpr* parse_match_expression(Parser* parser, const Token* match_token) {
  AstExpr* expr = new_expr(parser, AST_EXPR_MATCH, match_token);
  expr->as.match_expr.subject = parse_expression(parser);
  parser_consume(parser, TOK_LBRACE, "expected '{' after match subject");
  AstMatchArm* arms = NULL;
  bool saw_default = false;
  while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
    bool is_default = false;
    AstExpr* pattern = NULL;
    if (parser_match(parser, TOK_KW_DEFAULT)) {
      is_default = true;
    } else if (parser_match(parser, TOK_KW_CASE)) {
      pattern = parse_expression(parser);
    } else if (token_is_ident(parser_peek(parser), "_")) {
      parser_error(parser, parser_peek(parser), "use 'default' instead of '_' in match expressions");
    } else {
      parser_error(parser, parser_peek(parser), "expected 'case' or 'default' in match expression");
    }
    if (is_default) {
      if (saw_default) {
        parser_error(parser, parser_peek(parser), "match already has a default arm");
      }
      saw_default = true;
    }
    parser_consume(parser, TOK_ARROW, "expected '=>' after match pattern");
    AstExpr* value = parse_expression(parser);
    AstMatchArm* arm = parser_alloc(parser, sizeof(AstMatchArm));
    arm->pattern = pattern;
    arm->value = value;
    arm->next = NULL;
    arms = append_match_arm(arms, arm);
    if (parser_check(parser, TOK_RBRACE)) break;
    parser_match(parser, TOK_COMMA);
  }
  parser_consume(parser, TOK_RBRACE, "expected '}' after match expression");
  expr->as.match_expr.arms = arms;
  return expr;
}

static AstStmt* parse_statement(Parser* parser) {
  if (parser_match(parser, TOK_KW_DEF)) {
    const Token* def_token = parser_previous(parser);
    if (looks_like_destructure(parser)) {
      return parse_destructure_statement(parser, def_token);
    }
    return parse_def_statement(parser, def_token);
  }
  if (parser_match(parser, TOK_KW_RET)) {
    return parse_return_statement(parser, parser_previous(parser));
  }
  if (parser_match(parser, TOK_KW_IF)) {
    return parse_if_statement(parser, parser_previous(parser));
  }
  if (parser_match(parser, TOK_KW_LOOP)) {
    return parse_loop_statement(parser, parser_previous(parser));
  }
  if (parser_match(parser, TOK_KW_MATCH)) {
    return parse_match_statement(parser, parser_previous(parser));
  }
  
  AstExpr* expr = parse_expression(parser);
  if (parser_match(parser, TOK_ASSIGN)) {
    AstStmt* stmt = new_stmt(parser, AST_STMT_ASSIGN, parser_previous(parser));
    stmt->as.assign_stmt.target = expr;
    stmt->as.assign_stmt.value = parse_expression(parser);
    stmt->as.assign_stmt.is_move = false;
    return stmt;
  }
  if (parser_match(parser, TOK_ARROW)) {
    AstStmt* stmt = new_stmt(parser, AST_STMT_ASSIGN, parser_previous(parser));
    stmt->as.assign_stmt.target = expr;
    stmt->as.assign_stmt.value = parse_expression(parser);
    stmt->as.assign_stmt.is_move = true;
    return stmt;
  }

  AstStmt* stmt = new_stmt(parser, AST_STMT_EXPR, parser_peek(parser));
  stmt->as.expr_stmt = expr;
  return stmt;
}

static AstBlock* parse_block(Parser* parser) {
  parser_consume(parser, TOK_LBRACE, "expected '{' to start block");
  AstBlock* block = parser_alloc(parser, sizeof(AstBlock));
  AstStmt* head = NULL;
  while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
    AstStmt* stmt = parse_statement(parser);
    head = append_stmt(head, stmt);
  }
  parser_consume(parser, TOK_RBRACE, "expected '}' to close block");
  block->first = head;
  return block;
}

static AstTypeField* append_field(AstTypeField* head, AstTypeField* node) {
  if (!head) {
    return node;
  }
  AstTypeField* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstTypeField* parse_type_fields(Parser* parser) {
  AstTypeField* head = NULL;
  while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
    const Token* field_name = parser_consume(parser, TOK_IDENT, "expected field name");
    parser_consume(parser, TOK_COLON, "expected ':' after field name");
    AstTypeField* field = parser_alloc(parser, sizeof(AstTypeField));
    field->name = parser_copy_str(parser, field_name->lexeme);
    field->type = parse_type_ref(parser);
    head = append_field(head, field);
    
    if (parser_check(parser, TOK_RBRACE)) break;
    if (parser_match(parser, TOK_COMMA)) {
        if (parser_check(parser, TOK_RBRACE)) break;
    }
  }
  return head;
}

static AstDecl* parse_type_declaration(Parser* parser) {
  const Token* type_token = parser_previous(parser);
  const Token* name = parser_consume(parser, TOK_IDENT, "expected type name");
  AstDecl* decl = parser_alloc(parser, sizeof(AstDecl));
  decl->kind = AST_DECL_TYPE;
  decl->line = type_token->line;
  decl->column = type_token->column;
  decl->as.type_decl.name = parser_copy_str(parser, name->lexeme);
  if (parser_match(parser, TOK_COLON)) {
    decl->as.type_decl.properties = parse_type_properties(parser);
    if (!decl->as.type_decl.properties) {
      parser_error(parser, parser_peek(parser), "expected property after ':' in type declaration");
    }
  }
  parser_consume(parser, TOK_LBRACE, "expected '{' to start type body");
  decl->as.type_decl.fields = parse_type_fields(parser);
  parser_consume(parser, TOK_RBRACE, "expected '}' after type body");
  return decl;
}

static AstDecl* parse_func_declaration(Parser* parser, bool is_extern) {
  const Token* func_token = parser_previous(parser);
  const Token* name = parser_consume(parser, TOK_IDENT, "expected function name");
  if (!is_extern) { // Externs might need C names which can be PascalCase (e.g. Raylib, Windows API)
      check_camel_case(parser, name, "function");
  }
  AstDecl* decl = parser_alloc(parser, sizeof(AstDecl));
  decl->kind = AST_DECL_FUNC;
  decl->line = func_token->line;
  decl->column = func_token->column;
  decl->as.func_decl.name = parser_copy_str(parser, name->lexeme);
  decl->as.func_decl.is_extern = is_extern;
  decl->as.func_decl.params = parse_param_list(parser);
  if (parser_match(parser, TOK_COLON)) {
    decl->as.func_decl.properties = parse_func_properties(parser);
    if (parser_match(parser, TOK_KW_RET)) {
      decl->as.func_decl.returns = parse_return_clause(parser);
    }
  }
  if (is_extern) {
    if (parser_check(parser, TOK_LBRACE)) {
      parser_error(parser, parser_peek(parser), "extern functions cannot have a body");
    }
    decl->as.func_decl.body = NULL;
    return decl;
  }
  decl->as.func_decl.body = parse_block(parser);
  return decl;
}

static AstDecl* parse_declaration(Parser* parser) {
  if (parser_match(parser, TOK_KW_PUB) || parser_match(parser, TOK_KW_PRIV)) {
    const Token* token = parser_previous(parser);
    Str text = token->lexeme;
    if (parser_check(parser, TOK_KW_FUNC)) {
      parser_error(parser, token, "Function property '%.*s' cannot be defined before function. It must be put into the properties section after the function parameters like this: 'func name(): %.*s'",
                   (int)text.len, text.data, (int)text.len, text.data);
      return NULL;
    }
    if (parser_check(parser, TOK_KW_TYPE)) {
        parser_error(parser, token, "Type property '%.*s' cannot be defined before type. It must be put into the properties section like this: 'type Name: %.*s'",
                     (int)text.len, text.data, (int)text.len, text.data);
        return NULL;
    }
    parser_error(parser, token, "unexpected property '%.*s' at top level", (int)text.len, text.data);
    return NULL;
  }

  bool saw_extern = parser_match(parser, TOK_KW_EXTERN);
  if (parser_match(parser, TOK_KW_TYPE)) {
    if (saw_extern) {
      parser_error(parser, parser_previous(parser), "extern is only valid before func");
    }
    return parse_type_declaration(parser);
  }
  if (parser_match(parser, TOK_KW_FUNC)) {
    return parse_func_declaration(parser, saw_extern);
  }
  if (saw_extern) {
    parser_error(parser, parser_previous(parser), "extern must be followed by func");
  }
  parser_error(parser, parser_peek(parser), "expected 'type' or 'func'");
  return NULL;
}

static AstDecl* append_decl(AstDecl* head, AstDecl* node) {
  if (!node) {
    return head;
  }
  if (!head) {
    return node;
  }
  AstDecl* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

AstModule* parse_module(Arena* arena, const char* file_path, TokenList tokens) {
  if (!tokens.count) {
    return NULL;
  }

  // Filter out comments into their own list
  Token* filtered_tokens = arena_alloc(arena, tokens.count * sizeof(Token));
  Token* comments = arena_alloc(arena, tokens.count * sizeof(Token));
  size_t filtered_count = 0;
  size_t comment_count = 0;

  for (size_t i = 0; i < tokens.count; i++) {
    if (tokens.data[i].kind == TOK_COMMENT || tokens.data[i].kind == TOK_BLOCK_COMMENT) {
      comments[comment_count++] = tokens.data[i];
    } else {
      filtered_tokens[filtered_count++] = tokens.data[i];
    }
  }

  Parser parser = {
      .arena = arena,
      .file_path = file_path,
      .tokens = filtered_tokens,
      .count = filtered_count,
      .index = 0,
  };

  AstModule* module = parser_alloc(&parser, sizeof(AstModule));
  module->comments = comments;
  module->comment_count = comment_count;
  AstImport* imports = NULL;
  while (parser_match(&parser, TOK_KW_IMPORT) || parser_match(&parser, TOK_KW_EXPORT)) {
    const Token* prev = parser_previous(&parser);
    bool is_export = prev && prev->kind == TOK_KW_EXPORT;
    AstImport* clause = parse_import_clause(&parser, is_export);
    imports = append_import(imports, clause);
  }
  AstDecl* head = NULL;
  while (!parser_check(&parser, TOK_EOF)) {
    AstDecl* decl = parse_declaration(&parser);
    head = append_decl(head, decl);
  }
  module->imports = imports;
  module->decls = head;
  return module;
}
