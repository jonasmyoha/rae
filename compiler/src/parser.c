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

static bool parser_check_at(Parser* parser, size_t offset, TokenKind kind) {
  return parser_peek_at(parser, offset)->kind == kind;
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

static const Token* parser_consume_ident(Parser* parser, const char* message) {
  const Token* token = parser_peek(parser);
  if (token->kind == TOK_IDENT || token->kind == TOK_KW_ID || token->kind == TOK_KW_KEY) {
    return parser_advance(parser);
  }
  parser_error(parser, token, message);
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

static AstTypeRef* parse_type_ref_from_ident(Parser* parser, const Token* ident_token) {
  AstTypeRef* type = parser_alloc(parser, sizeof(AstTypeRef));
  type->parts = make_identifier_part(parser, ident_token->lexeme);
  type->generic_args = NULL; // For now, no generic args from simple ident
  type->next = NULL;
  return type;
}

static AstTypeRef* append_type_ref_list(AstTypeRef* head, AstTypeRef* node) {
  if (!head) return node;
  AstTypeRef* tail = head;
  while (tail->next) { // Used for chaining generic parameters at the same level
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstTypeRef* parse_type_ref(Parser* parser) {
  const Token* start_token = parser_peek(parser);
  AstTypeRef* type = parser_alloc(parser, sizeof(AstTypeRef));
  type->line = start_token->line;
  type->column = start_token->column;
  
  if (parser_match(parser, TOK_KW_OPT)) {
    type->is_opt = true;
  }
  
  if (parser_match(parser, TOK_KW_VIEW)) {
    type->is_view = true;
  } else if (parser_match(parser, TOK_KW_MOD)) {
    type->is_mod = true;
  }
  
  if (parser_match(parser, TOK_KW_ID)) {
    type->is_id = true;
  } else if (parser_match(parser, TOK_KW_KEY)) {
    type->is_key = true;
  }
  
  AstIdentifierPart* parts_head = NULL;
  AstIdentifierPart* parts_tail = NULL;
  bool consumed_base = false;
  
  while (true) {
    TokenKind kind = parser_peek(parser)->kind;
    if (kind == TOK_IDENT) {
      const Token* tok = parser_advance(parser);
      AstIdentifierPart* part = make_identifier_part(parser, tok->lexeme);
      if (!parts_head) {
        parts_head = parts_tail = part;
      } else {
        parts_tail->next = part;
        parts_tail = part;
      }
      consumed_base = true;
      
      const Token* next = parser_peek(parser);
      if (next->kind == TOK_IDENT && next->line == tok->line) {
          continue;
      }
      break;
    }
    break;
  }
  
  if (!consumed_base) {
    parser_error(parser, parser_peek(parser), "expected type");
  }
  type->parts = parts_head;

  if (parser_match(parser, TOK_LPAREN)) {
    AstTypeRef* generic_params_head = NULL;
    do {
      AstTypeRef* generic_param = parse_type_ref(parser); // Recursive call for nested generics
      
      // Reject references in generic arguments
      if (generic_param->is_view || generic_param->is_mod) {
          parser_error(parser, start_token, "references (view/mod) cannot be used as generic type arguments");
      }

      generic_params_head = append_type_ref_list(generic_params_head, generic_param);
      if (parser_check(parser, TOK_RPAREN)) break;
      parser_consume(parser, TOK_COMMA, "expected ',' or ')' in generic type list");
      if (parser_check(parser, TOK_RPAREN)) break;
    } while (true);
    parser_consume(parser, TOK_RPAREN, "expected ')' after generic type list");
    type->generic_args = generic_params_head;
  }

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
  while (parser_check(parser, TOK_KW_PUB) || parser_check(parser, TOK_KW_PRIV) || parser_check(parser, TOK_IDENT)) {
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

static Str unescape_string(Parser* parser, Str lit, bool strip_start, bool strip_end);

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
  Str path;
  if (parser_check(parser, TOK_STRING)) {
      const Token* t = parser_advance(parser);
      path = unescape_string(parser, t->lexeme, true, true);
  } else if (parser_check(parser, TOK_IDENT) && str_eq_cstr(parser_peek(parser)->lexeme, "nostdlib")) {
    const Token* t = parser_advance(parser);
    AstImport* clause = parser_alloc(parser, sizeof(AstImport));
    clause->is_export = is_export;
    clause->path = parser_copy_str(parser, t->lexeme);
    clause->line = start->line;
    clause->column = start->column;
    return clause;
  } else {
    path = parse_import_path_spec(parser);
  }
  
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

static bool is_multiline(const Token* start, const Token* end) {
  return start->line != end->line;
}

static void parser_consume_comma(Parser* parser, bool multiline, const char* context) {
  if (parser_match(parser, TOK_COMMA)) {
    return;
  }
  if (!multiline) {
      TokenKind next = parser_peek(parser)->kind;
      if (next != TOK_RPAREN && next != TOK_RBRACE && next != TOK_RBRACKET) {
          parser_error(parser, parser_peek(parser), "expected ',' or newline between elements in %s", context);
      }
  }
}

static void check_no_trailing_comma(Parser* parser, const char* context) {
  if (parser_previous(parser)->kind == TOK_COMMA) {
    parser_error(parser, parser_previous(parser), "trailing comma not allowed in %s", context);
  }
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
  const Token* name = parser_consume_ident(parser, "expected parameter name");
  parser_consume(parser, TOK_COLON, "expected ':' after parameter name");
  AstParam* param = parser_alloc(parser, sizeof(AstParam));
  param->name = parser_copy_str(parser, name->lexeme);
  param->type = parse_type_ref(parser);
  return param;
}

static AstParam* parse_param_list(Parser* parser) {
  const Token* start = parser_consume(parser, TOK_LPAREN, "expected '(' after function name");
  if (parser_match(parser, TOK_RPAREN)) {
    return NULL;
  }
  
  // Peek ahead to find the closing RPAREN to determine if it's multi-line
  const Token* end = NULL;
  int depth = 1;
  for (size_t i = 0; i < parser->count - parser->index; i++) {
    const Token* t = parser_peek_at(parser, i);
    if (t->kind == TOK_LPAREN) depth++;
    else if (t->kind == TOK_RPAREN) {
      depth--;
      if (depth == 0) {
        end = t;
        break;
      }
    }
  }
  bool multiline = end ? is_multiline(start, end) : false;

  AstParam* head = NULL;
  for (;;) {
    head = append_param(head, parse_param(parser));
    if (parser_check(parser, TOK_RPAREN)) {
      parser_advance(parser);
      check_no_trailing_comma(parser, "parameter list");
      break;
    }
    parser_consume_comma(parser, multiline, "parameter list");
    if (parser_match(parser, TOK_RPAREN)) {
      check_no_trailing_comma(parser, "parameter list");
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

static AstReturnItem* parse_return_clause(Parser* parser, bool multiline) {
  AstReturnItem* head = NULL;
  for (;;) {
    AstReturnItem* item = parser_alloc(parser, sizeof(AstReturnItem));
    if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
      const Token* label = parser_advance(parser);
      parser_consume(parser, TOK_COLON, "expected ':' after return label");
      item->has_name = true;
      item->name = parser_copy_str(parser, label->lexeme);
    }
    item->type = parse_type_ref(parser);
    head = append_return_item(head, item);
    
    if (parser_check(parser, TOK_LBRACE) || parser_check(parser, TOK_EOF) || 
        parser_check(parser, TOK_KW_FUNC) || parser_check(parser, TOK_KW_TYPE)) break;
    
    parser_consume_comma(parser, multiline, "return type list");
    if (parser_check(parser, TOK_LBRACE)) {
      check_no_trailing_comma(parser, "return type list");
      break;
    }
  }
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

static AstDestructureBinding* append_destructure_binding(AstDestructureBinding* head, AstDestructureBinding* node) {
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

// Forward declarations for parsing expressions and statements
static AstExpr* parse_expression(Parser* parser);
static AstBlock* parse_block(Parser* parser);
static AstStmt* parse_statement(Parser* parser);
static AstExpr* parse_collection_literal(Parser* parser, const Token* start_token);
static AstExpr* parse_list_literal(Parser* parser, const Token* start_token);
static AstExpr* parse_match_expression(Parser* parser, const Token* start_token);
static AstExpr* parse_typed_literal(Parser* parser, const Token* start_token, AstTypeRef* type_hint);
static AstTypeRef* parse_type_ref_from_ident(Parser* parser, const Token* ident_token);
static AstExpr* finish_call(Parser* parser, AstExpr* callee, const Token* start_token);

static bool token_is_ident(const Token* token, const char* text) {
  if (!token || token->kind != TOK_IDENT) return false;
  size_t len = strlen(text);
  if (token->lexeme.len != len) return false;
  return strncmp(token->lexeme.data, text, len) == 0;
}

static bool is_unary_operator(TokenKind kind) {
  return kind == TOK_MINUS || kind == TOK_KW_NOT || kind == TOK_KW_SPAWN || kind == TOK_INC || kind == TOK_DEC || kind == TOK_KW_VIEW || kind == TOK_KW_MOD;
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

// Helper functions for parsing literal values (MOVED UP)
static int64_t parse_char_value(Str text) {
  if (text.len == 0) return 0;
  if (text.data[0] == '\\') {
    if (text.len < 2) return 0;
    char esc = text.data[1];
    if (esc == 'n') return '\n';
    if (esc == 'r') return '\r';
    if (esc == 't') return '\t';
    if (esc == '0') return '\0';
    if (esc == '\\') return '\\';
    if (esc == '\'') return '\'';
    if (esc == '"') return '"';
    if (esc == 'u') {
        if (text.len < 4) return 0;
        int64_t val = 0;
        for (size_t i = 3; i < text.len; i++) {
            char h = text.data[i];
            if (h == '}') break;
            val <<= 4;
            if (h >= '0' && h <= '9') val |= (h - '0');
            else if (h >= 'a' && h <= 'f') val |= (h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') val |= (h - 'A' + 10);
        }
        return val;
    }
    return esc;
  }
  unsigned char c = (unsigned char)text.data[0];
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0) return ((c & 0x1F) << 6) | (text.data[1] & 0x3F);
  if ((c & 0xF0) == 0xE0) return ((c & 0x0F) << 12) | ((text.data[1] & 0x3F) << 6) | (text.data[2] & 0x3F);
  if ((c & 0xF8) == 0xF0) return ((c & 0x07) << 18) | ((text.data[1] & 0x3F) << 12) | ((text.data[2] & 0x3F) << 6) | (text.data[3] & 0x3F);
  return c;
}

static Str unescape_string(Parser* parser, Str lit, bool strip_start, bool strip_end) {
  if (lit.len == 0) return lit;
  size_t start = strip_start ? 1 : 0;
  size_t end = strip_end ? lit.len - 1 : lit.len;
  if (end < start) return (Str){0};
  
  size_t len = end - start;
  char* buffer = parser_alloc(parser, len + 1);
  size_t out_len = 0;
  
  for (size_t i = start; i < end; i++) {
    char c = lit.data[i];
    if (c == '\\' && i + 1 < end) {
      i++;
      char esc = lit.data[i];
      switch (esc) {
        case 'n': buffer[out_len++] = '\n'; break;
        case 'r': buffer[out_len++] = '\r'; break;
        case 't': buffer[out_len++] = '\t'; break;
        case '0': buffer[out_len++] = '\0'; break;
        case '\\': buffer[out_len++] = '\\'; break;
        case '"': buffer[out_len++] = '"'; break;
        case '\'': buffer[out_len++] = '\''; break;
        case '{': buffer[out_len++] = '{'; break;
        case '}': buffer[out_len++] = '}'; break;
        case 'u': {
          if (i + 1 < end && lit.data[i+1] == '{') {
            i += 2; // skip u{
            int64_t val = 0;
            while (i < end && lit.data[i] != '}') {
              char h = lit.data[i++];
              val <<= 4;
              if (h >= '0' && h <= '9') val |= (h - '0');
              else if (h >= 'a' && h <= 'f') val |= (h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') val |= (h - 'A' + 10);
            }
            // Encode val as UTF-8 into buffer
            if (val < 0x80) {
              buffer[out_len++] = (char)val;
            } else if (val < 0x800) {
              buffer[out_len++] = (char)(0xC0 | (val >> 6));
              buffer[out_len++] = (char)(0x80 | (val & 0x3F));
            } else if (val < 0x10000) {
              buffer[out_len++] = (char)(0xE0 | (val >> 12));
              buffer[out_len++] = (char)(0x80 | ((val >> 6) & 0x3F));
              buffer[out_len++] = (char)(0x80 | (val & 0x3F));
            } else {
              buffer[out_len++] = (char)(0xF0 | (val >> 18));
              buffer[out_len++] = (char)(0x80 | ((val >> 12) & 0x3F));
              buffer[out_len++] = (char)(0x80 | ((val >> 6) & 0x3F));
              buffer[out_len++] = (char)(0x80 | (val & 0x3F));
            }
          } else {
            buffer[out_len++] = 'u'; // fallback
          }
          break;
        }
        default: buffer[out_len++] = esc; break;
      }
    } else {
      buffer[out_len++] = c;
    }
  }
  buffer[out_len] = '\0';
  return (Str){.data = buffer, .len = out_len};
}

static AstExpr* finish_call(Parser* parser, AstExpr* callee, const Token* start_token) {
  AstExpr* expr = new_expr(parser, AST_EXPR_CALL, start_token);
  expr->as.call.callee = callee;
  if (parser_match(parser, TOK_RPAREN)) {
    return expr;
  }

  // Peek ahead to find the closing RPAREN to determine if it's multi-line
  const Token* end = NULL;
  int depth = 1;
  for (size_t i = 0; i < parser->count - parser->index; i++) {
    const Token* t = parser_peek_at(parser, i);
    if (t->kind == TOK_LPAREN) depth++;
    else if (t->kind == TOK_RPAREN) {
      depth--;
      if (depth == 0) {
        end = t;
        break;
      }
    }
  }
  bool multiline = end ? is_multiline(start_token, end) : false;

  AstCallArg* args = NULL;
  size_t arg_idx = 0;
  do {
    AstCallArg* arg = parser_alloc(parser, sizeof(AstCallArg));
    
    // Check if it looks like a named argument: Identifier (or keyword) followed by ':'
    TokenKind k = parser_peek(parser)->kind;
    bool is_ident_like = (k == TOK_IDENT || k == TOK_KW_ID || k == TOK_KW_KEY);
    bool is_named_arg = is_ident_like && parser_check_at(parser, 1, TOK_COLON);

    if (arg_idx == 0 && !is_named_arg) {
        // Positional first argument
        arg->name = (Str){0};
        arg->value = parse_expression(parser);
    } else {
        const Token* name = parser_consume_ident(parser, "expected argument name (subsequent arguments must be named)");
        parser_consume(parser, TOK_COLON, "expected ':' after argument name");
        arg->name = parser_copy_str(parser, name->lexeme);
        arg->value = parse_expression(parser);
    }
    args = append_call_arg(args, arg);
    arg_idx++;
    if (parser_check(parser, TOK_RPAREN)) {
      check_no_trailing_comma(parser, "argument list");
      break;
    }
    parser_consume_comma(parser, multiline, "argument list");
    if (parser_check(parser, TOK_RPAREN)) {
      check_no_trailing_comma(parser, "argument list");
      break;
    }
  } while (true);
  parser_consume(parser, TOK_RPAREN, "expected ')' after arguments");
  expr->as.call.args = args;
  return expr;
}

AstCollectionElement* append_collection_element(AstCollectionElement* head, AstCollectionElement* node) {
  if (!head) return node;
  AstCollectionElement* tail = head;
  while (tail->next) {
    tail = tail->next;
  }
  tail->next = node;
  return head;
}

static AstExpr* parse_list_literal(Parser* parser, const Token* start_token) {
  AstExpr* expr = new_expr(parser, AST_EXPR_COLLECTION_LITERAL, start_token);
  AstCollectionElement* head = NULL;
  AstCollectionElement* tail = NULL;

  if (parser_match(parser, TOK_RBRACKET)) {
    expr->as.collection.elements = NULL;
    return expr;
  }

  // Peek ahead to find the closing RBRACKET to determine if it's multi-line
  const Token* end = NULL;
  int depth = 1;
  for (size_t i = 0; i < parser->count - parser->index; i++) {
    const Token* t = parser_peek_at(parser, i);
    if (t->kind == TOK_LBRACKET) depth++;
    else if (t->kind == TOK_RBRACKET) {
      depth--;
      if (depth == 0) {
        end = t;
        break;
      }
    }
  }
  bool multiline = end ? is_multiline(start_token, end) : false;

  do {
    AstCollectionElement* element = parser_alloc(parser, sizeof(AstCollectionElement));
    element->key = NULL;
    element->value = parse_expression(parser);
    element->next = NULL;

    if (!head) {
      head = tail = element;
    } else {
      tail->next = element;
      tail = element;
    }

    if (parser_check(parser, TOK_RBRACKET)) {
      check_no_trailing_comma(parser, "list literal");
      break;
    }
    parser_consume_comma(parser, multiline, "list literal");
    if (parser_check(parser, TOK_RBRACKET)) {
      check_no_trailing_comma(parser, "list literal");
      break;
    }
  } while (true);

  parser_consume(parser, TOK_RBRACKET, "expected ']' at end of list literal");
  expr->as.collection.elements = head;
  return expr;
}

static AstExpr* parse_collection_literal(Parser* parser, const Token* start_token) {
  AstCollectionElement* head = NULL;
  AstCollectionElement* tail = NULL;
  bool is_keyed = false;

  if (parser_match(parser, TOK_RBRACE)) { // Empty collection literal {}
    AstExpr* expr = new_expr(parser, AST_EXPR_COLLECTION_LITERAL, start_token);
    expr->as.collection.elements = NULL;
    return expr;
  }

  // Peek ahead to find the closing RBRACE to determine if it's multi-line
  const Token* end = NULL;
  int depth = 1;
  for (size_t i = 0; i < parser->count - parser->index; i++) {
    const Token* t = parser_peek_at(parser, i);
    if (t->kind == TOK_LBRACE) depth++;
    else if (t->kind == TOK_RBRACE) {
      depth--;
      if (depth == 0) {
        end = t;
        break;
      }
    }
  }
  bool multiline = end ? is_multiline(start_token, end) : false;

  do {
    AstCollectionElement* element = parser_alloc(parser, sizeof(AstCollectionElement));
    element->key = NULL;
    
    // Check for key: value pair (implies map/object literal)
    if ((parser_check(parser, TOK_IDENT) || parser_check(parser, TOK_STRING)) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
      if (head && !is_keyed) { // First element defines type
        parser_error(parser, parser_peek(parser), "mixing keyed and unkeyed elements in collection literal is not allowed");
        return NULL;
      }
      is_keyed = true; // Mark as map/object

      const Token* key_token = parser_advance(parser);
      Str key_str;
      if (key_token->kind == TOK_STRING) {
        key_str = unescape_string(parser, key_token->lexeme, true, true);
      } else { // TOK_IDENT
        key_str = parser_copy_str(parser, key_token->lexeme);
      }

      element->key = parser_alloc(parser, sizeof(Str));
      *element->key = key_str;
      parser_consume(parser, TOK_COLON, "expected ':' after key in collection literal");

      element->value = parse_expression(parser);
    } else { // Unkeyed element (implies list literal)
      if (head && is_keyed) { // First element defines type
        parser_error(parser, parser_peek(parser), "mixing keyed and unkeyed elements in collection literal is not allowed");
        return NULL;
      }
      element->value = parse_expression(parser);
    }

    if (!head) {
      head = tail = element;
    } else {
      tail->next = element;
      tail = element;
    }
    
    // Check for closing brace or comma
    if (parser_check(parser, TOK_RBRACE)) {
      check_no_trailing_comma(parser, "collection literal");
      break;
    }
    parser_consume_comma(parser, multiline, "collection literal");
    if (parser_check(parser, TOK_RBRACE)) {
      check_no_trailing_comma(parser, "collection literal");
      break;
    }
  } while (true);
  
  parser_consume(parser, TOK_RBRACE, "expected '}' after collection literal");

  if (is_keyed) {
      // Return as Object literal if it has keys
      AstExpr* expr = new_expr(parser, AST_EXPR_OBJECT, start_token);
      expr->as.object_literal.type = NULL;
      AstObjectField* o_head = NULL;
      AstObjectField* o_tail = NULL;
      
      AstCollectionElement* curr = head;
      while (curr) {
          AstObjectField* field = parser_alloc(parser, sizeof(AstObjectField));
          field->name = *curr->key;
          field->value = curr->value;
          field->next = NULL;
          if (!o_head) { o_head = o_tail = field; } else { o_tail->next = field; o_tail = field; }
          curr = curr->next;
      }
      expr->as.object_literal.fields = o_head;
      return expr;
  } else {
      AstExpr* expr = new_expr(parser, AST_EXPR_COLLECTION_LITERAL, start_token);
      expr->as.collection.elements = head;
      return expr;
  }
}

static bool type_is_list_or_array(AstTypeRef* type) {
  if (!type || !type->parts) return false;
  // Just check the first part
  Str name = type->parts->text;
  return str_eq_cstr(name, "List") || str_eq_cstr(name, "Array");
}

static AstExpr* parse_typed_literal(Parser* parser, const Token* start_token, AstTypeRef* type_hint) {
  bool is_object = false;

  if (parser_check(parser, TOK_RBRACE)) {
      // Empty.
      if (!type_is_list_or_array(type_hint)) is_object = true;
  } else {
      // Look ahead to disambiguate
      if (parser_check(parser, TOK_IDENT) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
          is_object = true;
      }
  }

  if (is_object) {
      AstExpr* expr = new_expr(parser, AST_EXPR_OBJECT, start_token);
      expr->as.object_literal.type = type_hint;
      AstObjectField* head = NULL;
      AstObjectField* tail = NULL;

      if (parser_match(parser, TOK_RBRACE)) {
        expr->as.object_literal.fields = NULL;
        return expr;
      }

      // Peek ahead to find the closing RBRACE to determine if it's multi-line
      const Token* end = NULL;
      int depth = 1;
      for (size_t i = 0; i < parser->count - parser->index; i++) {
        const Token* t = parser_peek_at(parser, i);
        if (t->kind == TOK_LBRACE) depth++;
        else if (t->kind == TOK_RBRACE) {
          depth--;
          if (depth == 0) {
            end = t;
            break;
          }
        }
      }
      bool multiline = end ? is_multiline(start_token, end) : false;

      do {
        AstObjectField* field = parser_alloc(parser, sizeof(AstObjectField));
        const Token* key_token = parser_consume_ident(parser, "expected field name in object literal");
        field->name = parser_copy_str(parser, key_token->lexeme);
        parser_consume(parser, TOK_COLON, "expected ':' after field name");
        field->value = parse_expression(parser);

        if (!head) { head = tail = field; } else { tail->next = field; tail = field; }
        
        if (parser_check(parser, TOK_RBRACE)) {
          check_no_trailing_comma(parser, "object literal");
          break;
        }
        parser_consume_comma(parser, multiline, "object literal");
        if (parser_check(parser, TOK_RBRACE)) {
          check_no_trailing_comma(parser, "object literal");
          break;
        }
      } while (true);
      parser_consume(parser, TOK_RBRACE, "expected '}' after object literal");
      expr->as.object_literal.fields = head;
      return expr;
  } else {
      // Collection literal (List/Array)
      AstExpr* expr = new_expr(parser, AST_EXPR_COLLECTION_LITERAL, start_token);
      expr->as.collection.type = type_hint; // Added type field
      AstCollectionElement* head = NULL;
      AstCollectionElement* tail = NULL;

      if (parser_match(parser, TOK_RBRACE)) {
        expr->as.collection.elements = NULL;
        return expr;
      }

      // Peek ahead to find the closing RBRACE to determine if it's multi-line
      const Token* end = NULL;
      int depth = 1;
      for (size_t i = 0; i < parser->count - parser->index; i++) {
        const Token* t = parser_peek_at(parser, i);
        if (t->kind == TOK_LBRACE) depth++;
        else if (t->kind == TOK_RBRACE) {
          depth--;
          if (depth == 0) {
            end = t;
            break;
          }
        }
      }
      bool multiline = end ? is_multiline(start_token, end) : false;

      do {
        AstCollectionElement* element = parser_alloc(parser, sizeof(AstCollectionElement));
        element->key = NULL; // List/Array elements have no keys
        
        element->value = parse_expression(parser);

        if (!head) { head = tail = element; } else { tail->next = element; tail = element; }
        
        if (parser_check(parser, TOK_RBRACE)) {
          check_no_trailing_comma(parser, "list literal");
          break;
        }
        parser_consume_comma(parser, multiline, "list literal");
        if (parser_check(parser, TOK_RBRACE)) {
          check_no_trailing_comma(parser, "list literal");
          break;
        }
      } while (true);
      parser_consume(parser, TOK_RBRACE, "expected '}' after list literal");
      expr->as.collection.elements = head;
      return expr;
  }
}

static bool is_type_name(Str name) {
  if (name.len == 0) return false;
  char c = name.data[0];
  return c >= 'A' && c <= 'Z';
}

static AstExpr* parse_primary(Parser* parser) {
  const Token* token = parser_peek(parser);

  // Check for typed object literal first
  if (token->kind == TOK_IDENT && is_type_name(token->lexeme)) {
    if (parser_peek_at(parser, 1)->kind == TOK_LBRACE) {
      const Token* ident_token = parser_advance(parser); // Consume the identifier
      AstTypeRef* type_ref = parse_type_ref_from_ident(parser, ident_token); // Create AstTypeRef from this ident_token
      const Token* start_token = parser_advance(parser); // Consume LBRACE
      return parse_typed_literal(parser, start_token, type_ref);
    }
    if (parser_peek_at(parser, 1)->kind == TOK_LPAREN) {
       // Look ahead for matching ) followed by {
       size_t i = 2;
       int depth = 1;
       while (i < parser->count - parser->index && depth > 0) {
           TokenKind k = parser_peek_at(parser, i)->kind;
           if (k == TOK_LPAREN) depth++;
           else if (k == TOK_RPAREN) depth--;
           else if (k == TOK_EOF) break;
           i++;
       }
       if (depth == 0 && parser_peek_at(parser, i)->kind == TOK_LBRACE) {
           AstTypeRef* type_ref = parse_type_ref(parser);
           const Token* start_token = parser_advance(parser); // {
           return parse_typed_literal(parser, start_token, type_ref);
       }
    }
  }

  // If not a typed object literal, proceed with other primary expressions
  switch (token->kind) {
    case TOK_IDENT: {
      parser_advance(parser); // Already peeked and confirmed not a typed object literal
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
      expr->as.string_lit = unescape_string(parser, token->lexeme, true, true);
      return expr;
    }
    case TOK_STRING_START: {
      const Token* start_token = token;
      parser_advance(parser);
      
      AstExpr* interp = new_expr(parser, AST_EXPR_INTERP, start_token);
      AstInterpPart* head = NULL;
      AstInterpPart* tail = NULL;

      AstExpr* initial = new_expr(parser, AST_EXPR_STRING, start_token);
      initial->as.string_lit = unescape_string(parser, start_token->lexeme, true, false);
      
      {
          AstInterpPart* part = parser_alloc(parser, sizeof(AstInterpPart));
          part->value = initial;
          part->next = NULL;
          head = tail = part;
      }
      
      while (true) {
        parser_consume(parser, TOK_LBRACE, "expected '{' in interpolated string");
        AstExpr* val = parse_expression(parser);
        parser_consume(parser, TOK_RBRACE, "expected '}' in interpolated string");
        
        {
            AstInterpPart* part = parser_alloc(parser, sizeof(AstInterpPart));
            part->value = val;
            part->next = NULL;
            tail->next = part;
            tail = part;
        }
        
        const Token* next = parser_peek(parser);
        if (next->kind == TOK_STRING_MID) {
            parser_advance(parser);
            AstExpr* mid = new_expr(parser, AST_EXPR_STRING, next);
            mid->as.string_lit = unescape_string(parser, next->lexeme, false, false);
            
            AstInterpPart* part = parser_alloc(parser, sizeof(AstInterpPart));
            part->value = mid;
            part->next = NULL;
            tail->next = part;
            tail = part;
        } else if (next->kind == TOK_STRING_END) {
            parser_advance(parser);
            AstExpr* end = new_expr(parser, AST_EXPR_STRING, next);
            end->as.string_lit = unescape_string(parser, next->lexeme, false, true);
            
            AstInterpPart* part = parser_alloc(parser, sizeof(AstInterpPart));
            part->value = end;
            part->next = NULL;
            tail->next = part;
            tail = part;
            break;
        } else {
            parser_error(parser, next, "expected string continuation");
            break;
        }
      }
      interp->as.interp.parts = head;
      return interp;
    }
    case TOK_CHAR: {
      parser_advance(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_CHAR, token);
      Str content = token->lexeme;
      if (content.len >= 2) {
          content.data += 1;
          content.len -= 2;
      }
      expr->as.char_lit = parser_copy_str(parser, content);
      expr->as.char_value = parse_char_value(content);
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
      AstExpr* inner = parse_expression(parser);
      parser_consume(parser, TOK_RPAREN, "expected ')' after expression");
      return inner;
    case TOK_LBRACE: { // Handle untyped collection literal (map or set)
      const Token* start = parser_advance(parser);
      return parse_collection_literal(parser, start);
    }
    case TOK_LBRACKET: { // Handle list literal
      const Token* start = parser_advance(parser);
      return parse_list_literal(parser, start);
    }
    case TOK_KW_MATCH: {
      const Token* match_token = parser_advance(parser);
      return parse_match_expression(parser, match_token);
    }
    default:
      parser_error(parser, token, "unexpected token in expression");
      return NULL;
  }
}

static AstExpr* parse_postfix(Parser* parser) {

  AstExpr* expr = parse_primary(parser);

  for (;;) {
    if (parser_match(parser, TOK_LPAREN)) {

      expr = finish_call(parser, expr, parser_previous(parser));

      continue;
    }
    if (parser_match(parser, TOK_DOT)) {
  
      const Token* name = parser_consume_ident(parser, "expected member name after '.'");
      // Check if it's a method call
      if (parser_match(parser, TOK_LPAREN)) {

        AstExpr* method_call = new_expr(parser, AST_EXPR_METHOD_CALL, parser_previous(parser));
        method_call->as.method_call.object = expr;
        method_call->as.method_call.method_name = parser_copy_str(parser, name->lexeme);
        // Parse arguments, implicitly adding 'this' (receiver)
        AstCallArg* args = NULL;
        // NOTE: We don't add 'this' as a named arg here in the parser anymore, 
        // the VM compiler will handle the receiver.
        
        if (!parser_match(parser, TOK_RPAREN)) {
          size_t arg_idx = 0;
          do {
            AstCallArg* arg = parser_alloc(parser, sizeof(AstCallArg));
            
            // Check if it looks like a named argument: Identifier (or keyword) followed by ':'
            TokenKind k = parser_peek(parser)->kind;
            bool is_ident_like = (k == TOK_IDENT || k == TOK_KW_ID || k == TOK_KW_KEY);
            bool is_named_arg = is_ident_like && parser_check_at(parser, 1, TOK_COLON);

            if (arg_idx == 0 && !is_named_arg) {
                // Positional first argument
                arg->name = (Str){0};
                arg->value = parse_expression(parser);
            } else {
                const Token* arg_name = parser_consume_ident(parser, "expected argument name (subsequent arguments must be named)");
                parser_consume(parser, TOK_COLON, "expected ':' after argument name");
                arg->name = parser_copy_str(parser, arg_name->lexeme);
                arg->value = parse_expression(parser);
            }
            args = append_call_arg(args, arg);
            arg_idx++;
            if (parser_check(parser, TOK_RPAREN)) break;
            parser_consume(parser, TOK_COMMA, "expected ',' between arguments");
            if (parser_check(parser, TOK_RPAREN)) {
              break;
            }
          } while (true);
          parser_consume(parser, TOK_RPAREN, "expected ')' after arguments");
        }
        method_call->as.method_call.args = args;
        expr = method_call;

        continue;
      } else {

        AstExpr* member = new_expr(parser, AST_EXPR_MEMBER, parser_previous(parser));
        member->as.member.object = expr;
        member->as.member.member = parser_copy_str(parser, name->lexeme);
        expr = member;

        continue;
      }
    }
    if (parser_match(parser, TOK_LBRACKET)) {
  
      AstExpr* index_expr = parse_expression(parser);
      parser_consume(parser, TOK_RBRACKET, "expected ']' after index expression");
      AstExpr* indexed = new_expr(parser, AST_EXPR_INDEX, parser_previous(parser));
      indexed->as.index.target = expr;
      indexed->as.index.index = index_expr;
      expr = indexed;

      continue;
    }
    if (parser_match(parser, TOK_INC)) {
      AstExpr* post = new_expr(parser, AST_EXPR_UNARY, parser_previous(parser));
      post->as.unary.op = AST_UNARY_POST_INC;
      post->as.unary.operand = expr;
      expr = post;
      continue;
    }
    if (parser_match(parser, TOK_DEC)) {
      AstExpr* post = new_expr(parser, AST_EXPR_UNARY, parser_previous(parser));
      post->as.unary.op = AST_UNARY_POST_DEC;
      post->as.unary.operand = expr;
      expr = post;
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
      case TOK_KW_VIEW:
        expr->as.unary.op = AST_UNARY_VIEW;
        break;
      case TOK_KW_MOD:
        expr->as.unary.op = AST_UNARY_MOD;
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

static AstIdentifierPart* parse_generic_params(Parser* parser) {
  if (!parser_check(parser, TOK_LPAREN)) return NULL;
  
  if (parser_peek_at(parser, 1)->kind == TOK_RPAREN) return NULL; // () is params
  
  // Check if it looks like a parameter list: (name: type) or (name: type, ...)
  // Generic params are just (T) or (T, U)
  for (size_t i = 1; i < parser->count - parser->index; i++) {
    TokenKind k = parser_peek_at(parser, i)->kind;
    if (k == TOK_COLON) return NULL; // Found a colon, definitely regular params
    if (k == TOK_RPAREN) break; // End of list, no colon found yet
    if (k == TOK_EOF) break;
  }

  parser_advance(parser); // Consume (
  AstIdentifierPart* head = NULL;
  AstIdentifierPart* tail = NULL;
  do {
    const Token* name = parser_consume_ident(parser, "expected generic type parameter name");
    AstIdentifierPart* node = make_identifier_part(parser, name->lexeme);
    if (!head) {
      head = tail = node;
    } else {
      tail->next = node;
      tail = node;
    }
    if (parser_check(parser, TOK_RPAREN)) break;
    parser_consume(parser, TOK_COMMA, "expected ',' or ')' in generic parameter list");
    if (parser_check(parser, TOK_RPAREN)) break;
  } while (true);
  parser_consume(parser, TOK_RPAREN, "expected ')' after generic parameter list");
  return head;
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

static AstStmt* parse_defer_statement(Parser* parser, const Token* token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_DEFER, token);
  stmt->as.defer_stmt.block = parse_block(parser);
  return stmt;
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
      match_case->block = parse_block(parser);
    } else if (parser_match(parser, TOK_KW_DEFAULT)) {
      if (saw_default) {
        parser_error(parser, parser_peek(parser), "match already has a default arm");
      }
      saw_default = true;
      match_case->pattern = NULL;
      match_case->block = parse_block(parser);
    } else {
      parser_error(parser, parser_peek(parser), "expected 'case' or 'default' inside match");
      match_case->pattern = NULL;
      match_case->block = NULL;
    }
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

static void check_camel_case(Parser* parser, const Token* token, const char* context) {
  if (token && token->lexeme.len > 0) {
    char first = token->lexeme.data[0];
    if (first >= 'A' && first <= 'Z') {
      parser_error(parser, token, "%s name '%.*s' should be camelCase (start with lowercase)",
                   context, (int)token->lexeme.len, token->lexeme.data);
    }
    for (size_t i = 0; i < token->lexeme.len; ++i) {
      if (token->lexeme.data[i] == '_') {
        parser_error(parser, token, "%s name '%.*s' should be camelCase (no underscores allowed)",
                     context, (int)token->lexeme.len, token->lexeme.data);
        break;
      }
    }
  }
}

static void check_pascal_case(Parser* parser, const Token* token, const char* context) {
  if (token && token->lexeme.len > 0) {
    char first = token->lexeme.data[0];
    if (first >= 'a' && first <= 'z') {
      parser_error(parser, token, "%s name '%.*s' should be PascalCase (start with uppercase)",
                   context, (int)token->lexeme.len, token->lexeme.data);
    }
    for (size_t i = 0; i < token->lexeme.len; ++i) {
      if (token->lexeme.data[i] == '_') {
        parser_error(parser, token, "%s name '%.*s' should be PascalCase (no underscores allowed)",
                     context, (int)token->lexeme.len, token->lexeme.data);
        break;
      }
    }
  }
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
      if (i + 1 < parser->count && parser->tokens[i + 1].kind == TOK_KW_LET) {
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

static AstStmt* parse_destructure_statement(Parser* parser, const Token* let_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_DESTRUCT, let_token);
  AstDestructureBinding* bindings = NULL;
  size_t binding_count = 0;
  for (;;) {
    const Token* local = parser_consume_ident(parser, "expected local name in destructuring binding");
    parser_consume(parser, TOK_COLON, "expected ':' after local name in destructuring binding");
    const Token* label = parser_consume_ident(parser, "expected return label in destructuring binding");
    AstDestructureBinding* binding = parser_alloc(parser, sizeof(AstDestructureBinding));
    binding->local_name = parser_copy_str(parser, local->lexeme);
    binding->return_label = parser_copy_str(parser, label->lexeme);
    bindings = append_destructure_binding(bindings, binding);
    binding_count++;
    if (parser_match(parser, TOK_COMMA)) {
      parser_consume(parser, TOK_KW_LET, "expected 'let' before next destructuring binding");
      continue;
    }
    break;
  }
  if (binding_count < 2) {
    parser_error(parser, let_token, "destructuring assignments require at least two bindings");
  }
  parser_consume(parser, TOK_ASSIGN, "destructuring assignments require '=' ");
  AstExpr* rhs = parse_expression(parser);
  if (!expr_is_call_like(rhs)) {
    parser_error(parser, let_token, "destructuring assignments require a call expression on the right-hand side");
  }
  stmt->as.destruct_stmt.bindings = bindings;
  stmt->as.destruct_stmt.call = rhs;
  return stmt;
}

static AstStmt* parse_let_statement(Parser* parser, const Token* let_token) {
  const Token* name = parser_consume_ident(parser, "expected identifier after 'let'");
  check_camel_case(parser, name, "variable");
  parser_consume(parser, TOK_COLON, "expected ':' after local name");
  
  AstTypeRef* type = parse_type_ref(parser);
  AstStmt* stmt = new_stmt(parser, AST_STMT_LET, let_token);
  stmt->as.let_stmt.name = parser_copy_str(parser, name->lexeme);
  stmt->as.let_stmt.type = type;
  
  if (parser_match(parser, TOK_ASSIGN)) {
    stmt->as.let_stmt.is_bind = false;
    if (type->is_view || type->is_mod) {
        parser_error(parser, parser_previous(parser), "use '=>' for alias bindings (view/mod)");
    }
    stmt->as.let_stmt.value = parse_expression(parser);
    
    // Rule 3.1: Reject typed constructor expressions on the RHS of let
    if (stmt->as.let_stmt.value->kind == AST_EXPR_OBJECT && stmt->as.let_stmt.value->as.object_literal.type != NULL) {
        // Exempt the canonical default value expression: Type {}
        if (stmt->as.let_stmt.value->as.object_literal.fields != NULL) {
            parser_error(parser, let_token, "with 'let', the binding's type must be written on the left-hand side only. Remove the type name from the RHS.");
        }
    }
  } else if (parser_match(parser, TOK_ARROW)) {
    stmt->as.let_stmt.is_bind = true;
    if (!type->is_view && !type->is_mod) {
        parser_error(parser, parser_previous(parser), "'=>' is only legal when the target type is mod T or view T");
    }
    stmt->as.let_stmt.value = parse_expression(parser);
  } else {
    stmt->as.let_stmt.is_bind = false;
    stmt->as.let_stmt.value = NULL;
    if (type->is_view || type->is_mod) {
        parser_error(parser, let_token, "alias bindings (view/mod) must be explicitly initialized");
    }
  }
  return stmt;
}
static AstStmt* parse_return_statement(Parser* parser, const Token* ret_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_RET, ret_token);
  if (parser_check(parser, TOK_RBRACE) || parser_check(parser, TOK_KW_CASE) || parser_check(parser, TOK_EOF)) {
    stmt->as.ret_stmt.values = NULL;
    return stmt;
  }
  stmt->as.ret_stmt.values = parse_return_values(parser);
  
  // Rule 3.1 Extension: Enforce type visibility for top-level structural literals in return
  AstReturnArg* arg = stmt->as.ret_stmt.values;
  while (arg) {
      if (arg->value->kind == AST_EXPR_OBJECT && arg->value->as.object_literal.type == NULL) {
          parser_error(parser, ret_token, "structural literals in 'ret' must be explicitly typed (e.g. 'ret Color { ... }').");
      }
      arg = arg->next;
  }
  
  return stmt;
}

static AstStmt* parse_if_statement(Parser* parser, const Token* if_token) {
  AstStmt* stmt = new_stmt(parser, AST_STMT_IF, if_token);
  stmt->as.if_stmt.condition = parse_expression(parser);
  stmt->as.if_stmt.then_block = parse_block(parser);
  if (parser_match(parser, TOK_KW_ELSE)) {
    if (parser_match(parser, TOK_KW_IF)) {
      // Synthesize a block for the nested 'if' to keep AST consistent
      AstBlock* block = parser_alloc(parser, sizeof(AstBlock));
      block->first = parse_if_statement(parser, parser_previous(parser));
      stmt->as.if_stmt.else_block = block;
    } else {
      stmt->as.if_stmt.else_block = parse_block(parser);
    }
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
      AstStmt* init = new_stmt(parser, AST_STMT_LET, name);
      init->as.let_stmt.name = parser_copy_str(parser, name->lexeme);
      init->as.let_stmt.type = type;
      init->as.let_stmt.value = NULL;
      init->as.let_stmt.is_bind = false;

      stmt->as.loop_stmt.init = init;
      stmt->as.loop_stmt.condition = parse_expression(parser);
      stmt->as.loop_stmt.increment = NULL;
    } else {
      bool is_bind = false;
      if (parser_match(parser, TOK_ASSIGN)) {
        is_bind = false;
      } else if (parser_match(parser, TOK_ARROW)) {
        is_bind = true;
      } else {
        parser_error(parser, parser_peek(parser), "expected '=' or 'in' in loop declaration");
      }

      AstStmt* init = new_stmt(parser, AST_STMT_LET, name);
      init->as.let_stmt.name = parser_copy_str(parser, name->lexeme);
      init->as.let_stmt.type = type;
      init->as.let_stmt.value = parse_expression(parser);
      init->as.let_stmt.is_bind = is_bind;

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
      AstStmt* init = new_stmt(parser, AST_STMT_LET, NULL);
      init->as.let_stmt.name = parser_copy_str(parser, expr->as.ident);
      init->as.let_stmt.type = NULL;
      init->as.let_stmt.value = NULL;
      init->as.let_stmt.is_bind = false;

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

static AstStmt* parse_statement(Parser* parser) {
  if (parser_match(parser, TOK_KW_LET)) {
    const Token* let_token = parser_previous(parser);
    if (looks_like_destructure(parser)) {
      return parse_destructure_statement(parser, let_token);
    }
    return parse_let_statement(parser, let_token);
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
  if (parser_match(parser, TOK_KW_DEFER)) {
    return parse_defer_statement(parser, parser_previous(parser));
  }
  
  AstExpr* expr = parse_expression(parser);
  if (parser_match(parser, TOK_ASSIGN)) {
    AstStmt* stmt = new_stmt(parser, AST_STMT_ASSIGN, parser_previous(parser));
    stmt->as.assign_stmt.target = expr;
    stmt->as.assign_stmt.value = parse_expression(parser);
    stmt->as.assign_stmt.is_bind = false;
    return stmt;
  }
  if (parser_match(parser, TOK_ARROW)) {
    parser_error(parser, parser_previous(parser), "rebinding an alias is illegal. '=>' is only for 'let' bindings.");
    return NULL;
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
  const Token* start = parser_previous(parser); // TOK_LBRACE
  
  // Peek ahead to find the closing RBRACE to determine if it's multi-line
  const Token* end = NULL;
  int depth = 1;
  for (size_t i = 0; i < parser->count - parser->index; i++) {
    const Token* t = parser_peek_at(parser, i);
    if (t->kind == TOK_LBRACE) depth++;
    else if (t->kind == TOK_RBRACE) {
      depth--;
      if (depth == 0) {
        end = t;
        break;
      }
    }
  }
  bool multiline = end ? is_multiline(start, end) : false;

  AstTypeField* head = NULL;
  while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
    const Token* field_name = parser_consume_ident(parser, "expected field name");
    parser_consume(parser, TOK_COLON, "expected ':' after field name");
    AstTypeField* field = parser_alloc(parser, sizeof(AstTypeField));
    field->name = parser_copy_str(parser, field_name->lexeme);
    field->type = parse_type_ref(parser);
    head = append_field(head, field);
    
    if (parser_check(parser, TOK_RBRACE)) {
      check_no_trailing_comma(parser, "type fields");
      break;
    }
    parser_consume_comma(parser, multiline, "type fields");
    if (parser_check(parser, TOK_RBRACE)) {
      check_no_trailing_comma(parser, "type fields");
      break;
    }
  }
  return head;
}

static AstDecl* parse_type_declaration(Parser* parser) {
  const Token* type_token = parser_previous(parser);
  const Token* name = parser_consume_ident(parser, "expected type name");
  check_pascal_case(parser, name, "type");
  AstDecl* decl = parser_alloc(parser, sizeof(AstDecl));
  decl->kind = AST_DECL_TYPE;
  decl->line = type_token->line;
  decl->column = type_token->column;
  decl->as.type_decl.name = parser_copy_str(parser, name->lexeme);
  decl->as.type_decl.generic_params = parse_generic_params(parser); // Parse generic params
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
  const Token* name_token = parser_consume_ident(parser, "expected function name");
  
  AstDecl* decl = parser_alloc(parser, sizeof(AstDecl));
  decl->kind = AST_DECL_FUNC;
  decl->line = func_token->line;
  decl->column = func_token->column;
  decl->as.func_decl.name = parser_copy_str(parser, name_token->lexeme);
  decl->as.func_decl.generic_params = parse_generic_params(parser);
  decl->as.func_decl.params = parse_param_list(parser);
  
  // Parse zero or more modifiers (extern, priv, spawn, etc.)
  AstProperty* props_head = NULL;
  
  // Also handle legacy colon before properties
  parser_match(parser, TOK_COLON);

  while (parser_check(parser, TOK_IDENT) || parser_check(parser, TOK_KW_EXTERN) || 
         parser_check(parser, TOK_KW_PRIV) || parser_check(parser, TOK_KW_PUB) || 
         parser_check(parser, TOK_KW_SPAWN)) {
    const Token* mod_token = parser_advance(parser);
    if (mod_token->kind == TOK_KW_EXTERN) is_extern = true;
    
    AstProperty* prop = make_property(parser, mod_token->lexeme);
    props_head = append_property(props_head, prop);
  }
  decl->as.func_decl.properties = props_head;
  decl->as.func_decl.is_extern = is_extern;

  // Handle optional colon before ret (legacy)
  parser_match(parser, TOK_COLON);

  if (parser_match(parser, TOK_KW_RET)) {
    const Token* ret_token = parser_previous(parser);
    const Token* end_brace = NULL;
    for (size_t i = 0; i < parser->count - parser->index; i++) {
      const Token* t = parser_peek_at(parser, i);
      if (t->kind == TOK_LBRACE || t->kind == TOK_EOF || t->kind == TOK_KW_FUNC || t->kind == TOK_KW_TYPE) {
        end_brace = t;
        break;
      }
    }
    bool multiline_ret = end_brace ? is_multiline(ret_token, end_brace) : false;
    decl->as.func_decl.returns = parse_return_clause(parser, multiline_ret);
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

static AstDecl* parse_enum_declaration(Parser* parser) {
  const Token* enum_token = parser_previous(parser);
  const Token* name = parser_consume_ident(parser, "expected enum name");
  check_pascal_case(parser, name, "enum");
  
  AstDecl* decl = parser_alloc(parser, sizeof(AstDecl));
  decl->kind = AST_DECL_ENUM;
  decl->line = enum_token->line;
  decl->column = enum_token->column;
  decl->as.enum_decl.name = parser_copy_str(parser, name->lexeme);
  decl->as.enum_decl.members = NULL;
  
  const Token* start = parser_consume(parser, TOK_LBRACE, "expected '{' after enum name");
  
  // Peek ahead to find the closing RBRACE to determine if it's multi-line
  const Token* end = NULL;
  int depth = 1;
  for (size_t i = 0; i < parser->count - parser->index; i++) {
    const Token* t = parser_peek_at(parser, i);
    if (t->kind == TOK_LBRACE) depth++;
    else if (t->kind == TOK_RBRACE) {
      depth--;
      if (depth == 0) {
        end = t;
        break;
      }
    }
  }
  bool multiline = end ? is_multiline(start, end) : false;

  AstEnumMember* head = NULL;
  AstEnumMember* tail = NULL;
  
  if (!parser_check(parser, TOK_RBRACE)) {
    do {
      const Token* member_name = parser_consume_ident(parser, "expected enum member name");
      check_pascal_case(parser, member_name, "enum member");
      
      AstEnumMember* member = parser_alloc(parser, sizeof(AstEnumMember));
      member->name = parser_copy_str(parser, member_name->lexeme);
      member->next = NULL;
      
      if (!head) {
        head = tail = member;
      } else {
        tail->next = member;
        tail = member;
      }
      
      if (parser_check(parser, TOK_RBRACE)) {
        check_no_trailing_comma(parser, "enum variants");
        break;
      }
      parser_consume_comma(parser, multiline, "enum variants");
      if (parser_check(parser, TOK_RBRACE)) {
        check_no_trailing_comma(parser, "enum variants");
        break;
      }
    } while (!parser_check(parser, TOK_RBRACE));
  }
  
  parser_consume(parser, TOK_RBRACE, "expected '}' at end of enum body");
  decl->as.enum_decl.members = head;
  return decl;
}

static AstDecl* parse_declaration(Parser* parser) {
  if (parser_check(parser, TOK_KW_PUB) || parser_check(parser, TOK_KW_PRIV)) {
    const Token* token = parser_advance(parser);
    Str text = token->lexeme;
    if (parser_check(parser, TOK_KW_FUNC)) {
      parser_error(parser, token, "Function property '%.*s' cannot be defined before function. It must be put into the properties section after the function parameters like this: 'func name() %.*s'",
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

  bool is_extern = parser_match(parser, TOK_KW_EXTERN);
  
  if (parser_match(parser, TOK_KW_TYPE)) {
    return parse_type_declaration(parser);
  }
  if (parser_match(parser, TOK_KW_ENUM)) {
    return parse_enum_declaration(parser);
  }
  if (parser_match(parser, TOK_KW_FUNC)) {
    return parse_func_declaration(parser, is_extern);
  }
  
  if (!is_extern && parser_check(parser, TOK_KW_FUNC)) {
      parser_advance(parser);
      return parse_func_declaration(parser, false);
  }
  
  if (is_extern) {
    parser_error(parser, parser_previous(parser), "extern must be followed by func");
    return NULL;
  }

  parser_error(parser, parser_peek(parser), "expected 'type', 'enum' or 'func'");
  parser_advance(parser); // Advance to avoid infinite loop
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
  while (parser_check(&parser, TOK_KW_IMPORT) || (parser_check(&parser, TOK_KW_EXPORT) && parser_peek_at(&parser, 1)->kind == TOK_STRING)) {
    if (parser_match(&parser, TOK_KW_IMPORT)) {
        imports = append_import(imports, parse_import_clause(&parser, false));
    } else {
        parser_advance(&parser); // consume export
        imports = append_import(imports, parse_import_clause(&parser, true));
    }
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

