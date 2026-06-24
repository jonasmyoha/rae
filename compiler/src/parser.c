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
  bool had_error;
} Parser;

typedef struct {
  int precedence;
  AstBinaryOp op;
} BinaryInfo;

static void parser_error(Parser* parser, const Token* token, const char* fmt, ...) {
  parser->had_error = true;
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

static bool is_ident_like(TokenKind kind) {
  return kind == TOK_IDENT || kind == TOK_KW_VAL;
}

static bool looks_like_ident(Parser* parser) {
  return is_ident_like(parser_peek(parser)->kind);
}

static const Token* parser_consume_ident(Parser* parser, const char* message) {
  const Token* token = parser_peek(parser);
  if (is_ident_like(token->kind)) {
    return parser_advance(parser);
  }
  parser_error(parser, token, message);
  return NULL;
}

// Like parser_consume_ident, but also accepts a keyword token as a name. Used
// for places where a name is a VALUE label and keyword-ness is irrelevant:
// enum case names and member-access names (so e.g. an enum case may be `none`
// or `type`, and `Enum.none` resolves). Any token whose lexeme is a valid
// identifier shape (a keyword's lexeme always is) is accepted.
static const Token* parser_consume_name(Parser* parser, const char* message) {
  const Token* t = parser_peek(parser);
  if (is_ident_like(t->kind)) return parser_advance(parser);
  if (t->lexeme.len > 0) {
    char c0 = t->lexeme.data[0];
    bool ok = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_';
    for (size_t i = 1; ok && i < t->lexeme.len; i++) {
      char d = t->lexeme.data[i];
      ok = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || (d >= '0' && d <= '9') || d == '_';
    }
    if (ok) return parser_advance(parser);
  }
  parser_error(parser, t, message);
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
  type->line = ident_token->line;
  type->column = ident_token->column;
  type->is_opt = false;
  type->is_view = false;
  type->is_mod = false;
  type->is_val = false;
  type->is_own = false;
  type->is_copy = false;
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
  type->is_opt = false;
  type->is_view = false;
  type->is_mod = false;
  type->is_val = false;
  type->is_own = false;
  type->is_copy = false;
  type->generic_args = NULL;
  type->next = NULL;

  if (parser_match(parser, TOK_KW_OPT)) {
    type->is_opt = true;
  }

  // Parameter-mode prefix. Stage A: parse `copy T` as an explicit
  // synonym for bare `T` — same downstream semantics, but the
  // is_copy bit lets future stages distinguish "author asked for a
  // fresh owned copy" from "author left it unspecified". Mutually
  // exclusive with view/mod/val/own — caught later in sema.
  if (parser_match(parser, TOK_KW_VIEW)) {
    type->is_view = true;
  } else if (parser_match(parser, TOK_KW_MOD)) {
    type->is_mod = true;
  } else if (parser_match(parser, TOK_KW_VAL)) {
    type->is_val = true;
  } else if (parser_match(parser, TOK_KW_OWN)) {
    type->is_own = true;
  } else if (parser_match(parser, TOK_KW_COPY)) {
    type->is_copy = true;
  }

  AstIdentifierPart* parts_head = NULL;
  AstIdentifierPart* parts_tail = NULL;
  bool consumed_base = false;
  
  while (true) {
    TokenKind kind = parser_peek(parser)->kind;
    if (is_ident_like(kind)) {
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
    parser_advance(parser); // Consume the token to prevent infinite loops
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
  while (parser_check(parser, TOK_KW_PUB) || parser_check(parser, TOK_KW_PRIV) || looks_like_ident(parser)) {
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
        if (is_ident_like(t->kind) || t->kind == TOK_INTEGER) is_path_char = true;    else if (t->kind == TOK_DOT || t->kind == TOK_SLASH) is_path_char = true;
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
    bool prev_ident = (is_ident_like(prev->kind) || prev->kind == TOK_INTEGER);
    bool curr_ident = (is_ident_like(t->kind) || t->kind == TOK_INTEGER);

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
  (void)multiline;
  (void)context;
  // In Rae v0.3, commas are optional everywhere as long as there is 
  // either a newline OR a space between tokens.
  // The lexer/parser handles the separation of tokens naturally.
  if (parser_match(parser, TOK_COMMA)) {
    return;
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

// Parse one entry of the unified parameter list. Both runtime parameters
// (`name: view T`, etc.) and generic type parameters (`T: type`) share the
// same surface syntax; the discriminator is whether the token after `:` is
// the `type` keyword.
//
// For a type parameter, `*out_is_type_param` is set to true and the returned
// AstParam carries just the name (type = NULL). The caller routes it into
// the enclosing decl's `generic_params` sidecar instead of the regular
// `params` list, which preserves the existing AST shape every sema /
// mangler / codegen reader already understands.
static AstParam* parse_param(Parser* parser, bool* out_is_type_param) {
  *out_is_type_param = false;
  const Token* name = parser_consume_ident(parser, "expected parameter name");
  // Friendly diagnostic for the stale `(T)(args)` syntax: a bare identifier
  // followed by `,` or `)` is the old generic-param spelling. Catch it
  // here instead of letting `parse_type_ref` produce a cryptic
  // "expected type" further along.
  TokenKind nk = parser_peek(parser)->kind;
  if (nk == TOK_COMMA || nk == TOK_RPAREN) {
    parser_error(parser, name,
      "stale generic syntax: a parameter must have a type — write "
      "`T: type` for a generic type parameter, or `name: T` for a "
      "value parameter (the old `func name(T)(...)` form is gone)");
    AstParam* param = parser_alloc(parser, sizeof(AstParam));
    param->name = parser_copy_str(parser, name->lexeme);
    param->type = NULL;
    param->next = NULL;
    *out_is_type_param = true; // treat as type-param to keep parsing in sync
    return param;
  }
  parser_consume(parser, TOK_COLON, "expected ':' after parameter name");
  if (parser_peek(parser)->kind == TOK_KW_TYPE) {
    parser_advance(parser); // consume `type`
    *out_is_type_param = true;
    AstParam* param = parser_alloc(parser, sizeof(AstParam));
    param->name = parser_copy_str(parser, name->lexeme);
    param->type = NULL;
    param->next = NULL;
    return param;
  }
  AstParam* param = parser_alloc(parser, sizeof(AstParam));
  param->name = parser_copy_str(parser, name->lexeme);
  param->type = parse_type_ref(parser);
  param->next = NULL;
  return param;
}

// Parse the unified `(...)` parameter list. `*out_generic_params` collects
// the names of `T: type` entries (in source order); the returned list is
// only value params. `out_generic_params` may be NULL when the caller is
// known not to allow type params (currently no such caller; both func and
// type decls accept type params).
//
// Constraint: type params must appear before any value param. Mixing — or
// putting a type param after a value param — is a parser error so the
// reader can rely on "type params come first" without an extra pass.
static AstParam* parse_param_list(Parser* parser, AstIdentifierPart** out_generic_params) {
  if (out_generic_params) *out_generic_params = NULL;
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
  AstIdentifierPart* gp_head = NULL;
  AstIdentifierPart* gp_tail = NULL;
  bool seen_value_param = false;
  for (;;) {
    const Token* slot_start = parser_peek(parser);
    bool is_type_param = false;
    AstParam* p = parse_param(parser, &is_type_param);
    if (is_type_param) {
      if (seen_value_param) {
        parser_error(parser, slot_start,
          "type parameters (`T: type`) must come before value parameters");
      }
      AstIdentifierPart* node = make_identifier_part(parser, p->name);
      if (!gp_head) {
        gp_head = gp_tail = node;
      } else {
        gp_tail->next = node;
        gp_tail = node;
      }
    } else {
      seen_value_param = true;
      head = append_param(head, p);
    }
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
  if (out_generic_params) *out_generic_params = gp_head;
  // Reject the legacy `(T)(args)` double-paren form. Type parameters now
  // live in the unified param list; a second `(` right after the closer is
  // a stale spelling — surface it with a friendly message rather than the
  // cryptic "expected ret" that would otherwise follow.
  if (parser_check(parser, TOK_LPAREN)) {
    parser_error(parser, parser_peek(parser),
      "stale generic syntax: type parameters now live in the regular "
      "parameter list — write `func name(T: type, ...)` instead of "
      "`func name(T)(...)`");
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
        parser_check(parser, TOK_KW_FUNC) || parser_check(parser, TOK_KW_TYPE) ||
        parser_check(parser, TOK_KW_ENUM) || parser_check(parser, TOK_KW_LET) ||
        parser_check(parser, TOK_KW_VAR) || parser_check(parser, TOK_KW_CONST) ||
        parser_check(parser, TOK_KW_EXTERN)) break;

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
  return kind == TOK_MINUS || kind == TOK_KW_NOT || kind == TOK_KW_SPAWN || kind == TOK_INC || kind == TOK_DEC || kind == TOK_KW_VIEW || kind == TOK_KW_MOD || kind == TOK_KW_OWN;
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
static uint32_t parse_char_value(Str text) {
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
        uint32_t val = 0;
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
  if (c < 0x80) return (uint32_t)c;
  if ((c & 0xE0) == 0xC0) return (uint32_t)(((c & 0x1F) << 6) | (text.data[1] & 0x3F));
  if ((c & 0xF0) == 0xE0) return (uint32_t)(((c & 0x0F) << 12) | ((text.data[1] & 0x3F) << 6) | (text.data[2] & 0x3F));
  if ((c & 0xF8) == 0xF0) return (uint32_t)(((c & 0x07) << 18) | ((text.data[1] & 0x3F) << 12) | ((text.data[2] & 0x3F) << 6) | (text.data[3] & 0x3F));
  return (uint32_t)c;
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
  expr->as.call.generic_args = NULL;

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
  bool multiline = end ? is_multiline(parser_peek(parser), end) : false;

  AstCallArg* args = NULL;
  size_t arg_idx = 0;
  do {
    size_t prev_index = parser->index;
    AstCallArg* arg = parser_alloc(parser, sizeof(AstCallArg));
    
    // Named argument: identifier followed by ':'. Generic type arguments
    // are spelled with the param's actual name (`T:`) or positionally,
    // not via a magic `type:` keyword.
    TokenKind k = parser_peek(parser)->kind;
    bool is_named_arg = (k == TOK_IDENT) && parser_check_at(parser, 1, TOK_COLON);

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
    if (parser->index == prev_index) {
        parser_advance(parser);
    }
  } while (true);
  parser_consume(parser, TOK_RPAREN, "expected ')' after arguments");
  expr->as.call.args = args;

  // Legacy `name(T)(args)` double-paren generic-call syntax is no
  // longer accepted. Type arguments now share the regular argument
  // list — three accepted spellings:
  //
  //   createList(type: String, initialCap: 4)
  //   createList(String, initialCap: 4)
  //   String.createList(initialCap: 4)
  //
  // See c_backend.c::hoist_type_arg_if_present.
  if (parser_check(parser, TOK_LPAREN)) {
    const Token* lp = parser_peek(parser);
    parser_error(parser, lp,
        "double-paren generic call `foo(T)(args)` is no longer supported — pass the type as a regular argument: `foo(T, args)`, `foo(type: T, args)`, or `T.foo(args)`");
    // Best-effort recovery: skip the trailing `(...)` so we keep
    // parsing the rest of the file.
    parser_advance(parser);
    int depth = 1;
    while (depth > 0 && !parser_check(parser, TOK_EOF)) {
        if (parser_match(parser, TOK_LPAREN)) { depth++; continue; }
        if (parser_match(parser, TOK_RPAREN)) { depth--; continue; }
        parser_advance(parser);
    }
  }

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
    size_t prev_index = parser->index;
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
    if (parser->index == prev_index) {
        parser_advance(parser);
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
    size_t prev_index = parser->index;
    AstCollectionElement* element = parser_alloc(parser, sizeof(AstCollectionElement));
    element->key = NULL;
    
    // Check for key: value pair (implies map/object literal)
    if ((looks_like_ident(parser) || parser_check(parser, TOK_STRING)) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
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
    if (parser->index == prev_index) {
        parser_advance(parser);
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
        size_t prev_index = parser->index;
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
        if (parser->index == prev_index) {
            parser_advance(parser);
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
        size_t prev_index = parser->index;
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
        if (parser->index == prev_index) {
            parser_advance(parser);
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

        TokenKind k0 = token->kind;

        if (is_ident_like(k0) && is_type_name(token->lexeme)) {

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
    case TOK_KW_VAL:
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
      parser_advance(parser); // Advance to avoid infinite loops
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
  
      const Token* name = parser_consume_name(parser, "expected member name after '.'");
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
            
            // Named argument: identifier followed by ':'. Generic type
            // arguments are spelled with the param's actual name (`T:`)
            // or positionally, not via a magic `type:` keyword.
            TokenKind k = parser_peek(parser)->kind;
            bool is_named_arg = (k == TOK_IDENT) && parser_check_at(parser, 1, TOK_COLON);

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
            parser_consume_comma(parser, false, "argument list");
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
    // `own EXPR` is a separate AST node (AST_EXPR_OWN) rather than a
    // unary op — see docs/ownership-model.md. The wrapper signals
    // "consume / move this value" to the C codegen's move-detection
    // pass; the inner expression evaluates normally.
    if (op_token->kind == TOK_KW_OWN) {
      AstExpr* operand = parse_unary(parser);
      AstExpr* expr = new_expr(parser, AST_EXPR_OWN, op_token);
      expr->as.unary.operand = operand;
      return expr;
    }
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
    // `is not` is a single binary inequality operator. Without this,
    // `x is not y` would parse as `x is (not y)`, comparing x against
    // the boolean negation of y — almost never what the user wants.
    if (info.op == AST_BIN_IS && parser_peek(parser)->kind == TOK_KW_NOT) {
      parser_advance(parser);
      info.op = AST_BIN_NEQ;
    }
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
    size_t prev_index = parser->index;
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
    if (parser->index == prev_index) {
        parser_advance(parser);
    }
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
    size_t prev_index = parser->index;
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
    if (parser->index == prev_index) {
        parser_advance(parser);
    }
  }
  parser_consume(parser, TOK_RBRACE, "expected '}' after match expression");
  expr->as.match_expr.arms = arms;
  return expr;
}

// Naming conventions (Types/enums/modules PascalCase; functions, variables,
// constants and enum cases camelCase) are NO LONGER enforced at parse time:
// strict parser errors break generated code, foreign/C bindings, and
// migrations. The convention is documented in docs/naming-conventions.md and
// is intended to be a linter WARNING (see QUEUE.md), not a hard error. These
// helpers are kept as no-ops so call sites can stay until the lint lands.
static void check_camel_case(Parser* parser, const Token* token, const char* context) {
  (void)parser; (void)token; (void)context;
}

static void check_pascal_case(Parser* parser, const Token* token, const char* context) {
  (void)parser; (void)token; (void)context;
}

static bool looks_like_destructure(Parser* parser) {
  size_t i = parser->index;
  if (i >= parser->count || !is_ident_like(parser->tokens[i].kind)) {
    return false;
  }
  i++;
  if (i >= parser->count || parser->tokens[i].kind != TOK_COLON) {
    return false;
  }
  i++;
  if (i >= parser->count || !is_ident_like(parser->tokens[i].kind)) {
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

// Parses a local binding: `let`/`var`/`const`. The type annotation is
// optional (`let x = expr` infers the type from the initializer); when
// present it is written `name: Type`. `var` is mutable; `let`/`const` are
// immutable (enforced in sema); `const` additionally requires a compile-time
// initializer.
static AstStmt* parse_binding_statement(Parser* parser, const Token* kw_token, bool is_var, bool is_const) {
  const char* kw = is_const ? "const" : (is_var ? "var" : "let");
  const Token* name = parser_consume_ident(parser, "expected identifier after binding keyword");
  check_camel_case(parser, name, "variable");

  AstTypeRef* type = NULL;
  if (parser_match(parser, TOK_COLON)) {
    type = parse_type_ref(parser);
  }

  AstStmt* stmt = new_stmt(parser, AST_STMT_LET, kw_token);
  stmt->as.let_stmt.name = parser_copy_str(parser, name->lexeme);
  stmt->as.let_stmt.type = type;
  stmt->as.let_stmt.is_var = is_var;
  stmt->as.let_stmt.is_const = is_const;

  if (parser_match(parser, TOK_ASSIGN)) {
    stmt->as.let_stmt.is_bind = false;
    if (type && (type->is_view || type->is_mod)) {
        parser_error(parser, parser_previous(parser), "use '=>' for alias bindings (view/mod)");
    }
    stmt->as.let_stmt.value = parse_expression(parser);

    // Reject a typed constructor on the RHS only when the type is ALSO written
    // on the LHS (writing it twice). With no LHS type, `let p = Point { ... }`
    // is the idiomatic typed construction and is allowed.
    if (type != NULL
        && stmt->as.let_stmt.value->kind == AST_EXPR_OBJECT
        && stmt->as.let_stmt.value->as.object_literal.type != NULL
        && stmt->as.let_stmt.value->as.object_literal.fields != NULL) {
        parser_error(parser, kw_token, "with '%s', the binding's type must be written on the left-hand side only. Remove the type name from the RHS.", kw);
    }
  } else if (parser_match(parser, TOK_ARROW)) {
    stmt->as.let_stmt.is_bind = true;
    if (is_const) {
        parser_error(parser, parser_previous(parser), "'const' is a value, not an alias binding; use '='");
    }
    if (!type || (!type->is_view && !type->is_mod)) {
        parser_error(parser, parser_previous(parser), "'=>' is only legal when the target type is mod T or view T");
    }
    stmt->as.let_stmt.value = parse_expression(parser);
  } else {
    stmt->as.let_stmt.is_bind = false;
    stmt->as.let_stmt.value = NULL;
    if (is_const) {
        parser_error(parser, kw_token, "'const' requires an initializer");
    } else if (type && (type->is_view || type->is_mod)) {
        parser_error(parser, kw_token, "alias bindings (view/mod) must be explicitly initialized");
    } else if (!type) {
        parser_error(parser, kw_token, "'%s' without a type annotation needs an initializer to infer the type", kw);
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

  parser_match(parser, TOK_KW_LET); // Optional 'let'

  if (looks_like_ident(parser) && parser_peek_at(parser, 1)->kind == TOK_COLON) {
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
    return parse_binding_statement(parser, let_token, false, false);
  }
  if (parser_match(parser, TOK_KW_VAR)) {
    return parse_binding_statement(parser, parser_previous(parser), true, false);
  }
  if (parser_match(parser, TOK_KW_CONST)) {
    return parse_binding_statement(parser, parser_previous(parser), false, true);
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
  if (parser_match(parser, TOK_KW_PARALLELLOOP)) {
    // Same grammar as `loop`; flagged parallel. Compiled as a sequential
    // loop for now (real parallel execution lands with the C thread
    // runtime). Reusing parse_loop_statement keeps one loop grammar.
    AstStmt* s = parse_loop_statement(parser, parser_previous(parser));
    if (s && s->kind == AST_STMT_LOOP) s->as.loop_stmt.is_parallel = true;
    return s;
  }
  if (parser_match(parser, TOK_KW_MATCH)) {
    return parse_match_statement(parser, parser_previous(parser));
  }
  if (parser_match(parser, TOK_KW_DEFER)) {
    return parse_defer_statement(parser, parser_previous(parser));
  }
  if (parser_match(parser, TOK_KW_TASKSCOPE)) {
    // Structured concurrency block. Desugars to a run-once scope
    // (`if true { ... }`): tasks bound inside join-on-drop at the
    // scope's end. (Non-escape enforcement / cancel-on-error are future
    // refinements; the scope + join-on-drop is the core guarantee.)
    const Token* ts_token = parser_previous(parser);
    AstStmt* stmt = new_stmt(parser, AST_STMT_IF, ts_token);
    AstExpr* cond = new_expr(parser, AST_EXPR_BOOL, ts_token);
    cond->as.boolean = true;
    stmt->as.if_stmt.condition = cond;
    stmt->as.if_stmt.then_block = parse_block(parser);
    stmt->as.if_stmt.else_block = NULL;
    return stmt;
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
  block->first = NULL;
  AstStmt* tail = NULL;
  while (!parser_check(parser, TOK_RBRACE) && !parser_check(parser, TOK_EOF)) {
    size_t prev_index = parser->index;
    AstStmt* stmt = parse_statement(parser);
    if (stmt) {
      if (!block->first) block->first = stmt;
      else tail->next = stmt;
      tail = stmt;
    }
    if (parser->index == prev_index) {
      parser_advance(parser); // Force progress
    }
  }
  parser_consume(parser, TOK_RBRACE, "expected '}' to close block");
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
    if (!field_name) {
      /* Recovery: skip the bogus token so we don't loop forever or
       * deref NULL on `field_name->lexeme`. The error has already been
       * reported by parser_consume_ident. */
      if (!parser_check(parser, TOK_EOF) && !parser_check(parser, TOK_RBRACE)) {
        parser_advance(parser);
      }
      continue;
    }
    parser_consume(parser, TOK_COLON, "expected ':' after field name");
    AstTypeField* field = parser_alloc(parser, sizeof(AstTypeField));
    field->name = parser_copy_str(parser, field_name->lexeme);
    field->type = parse_type_ref(parser);
    field->default_value = NULL;
    if (parser_match(parser, TOK_KW_IS) || parser_match(parser, TOK_ASSIGN)) {
      field->default_value = parse_expression(parser);
    }
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
  // Unified generic / parameter list. Type declarations only accept type
  // parameters (`T: type`); a value parameter here is a parse error so the
  // diagnostic surfaces at the right location instead of failing later.
  AstIdentifierPart* td_generic_params = NULL;
  if (parser_check(parser, TOK_LPAREN)) {
    AstParam* value_params = parse_param_list(parser, &td_generic_params);
    if (value_params) {
      parser_error(parser, parser_peek(parser),
        "type declarations only accept type parameters (`T: type`); "
        "value parameters belong on functions");
    }
  }
  decl->as.type_decl.generic_params = td_generic_params;
  if (parser_match(parser, TOK_COLON)) {
    decl->as.type_decl.properties = parse_type_properties(parser);
    if (!decl->as.type_decl.properties) {
      parser_error(parser, parser_peek(parser), "expected property after ':' in type declaration");
    }
  }
  /* Accept and skip a `pub` visibility marker before the body. Types
   * are always cross-file visible in Rae right now, so this is a no-op
   * — but users naturally write `type Foo pub { ... }` by analogy with
   * `func bar() pub`, and the old parser segfaulted on it instead of
   * just accepting or rejecting cleanly. */
  if (parser_check(parser, TOK_KW_PUB)) {
    parser_advance(parser);
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
  // Unified parameter list — type parameters (`T: type`) come first, then
  // value parameters. The parser routes type params into `generic_params`
  // for compatibility with the existing sema / mangler / codegen readers.
  AstIdentifierPart* fn_generic_params = NULL;
  decl->as.func_decl.params = parse_param_list(parser, &fn_generic_params);
  decl->as.func_decl.generic_params = fn_generic_params;
  
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
      // Stop at the body brace or at the start of the NEXT top-level
      // declaration, so a bodyless (extern) function's return clause doesn't
      // overshoot into a following enum / let / var / const / func / type.
      if (t->kind == TOK_LBRACE || t->kind == TOK_EOF || t->kind == TOK_KW_FUNC || t->kind == TOK_KW_TYPE
          || t->kind == TOK_KW_ENUM || t->kind == TOK_KW_LET || t->kind == TOK_KW_VAR
          || t->kind == TOK_KW_CONST || t->kind == TOK_KW_EXTERN) {
        end_brace = t;
        break;
      }
    }
    // An extern function has no body, so its return clause ends with the
    // signature line — never treat it as a multiline (tuple) return, otherwise
    // the lookahead's terminator (the next decl, on a later line) makes it look
    // multiline and it swallows the following declaration.
    bool multiline_ret = (!is_extern && end_brace) ? is_multiline(ret_token, end_brace) : false;
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
      const Token* member_name = parser_consume_name(parser, "expected enum member name");
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

static Str str_clone(Parser* parser, Str s) {
    if (s.len == 0) return s;
    char* copy = parser_alloc(parser, s.len);
    memcpy(copy, s.data, s.len);
    return (Str){.data = copy, .len = s.len};
}

static AstDecl* parse_global_let_declaration(Parser* parser, bool is_var, bool is_const) {
  const Token* token = parser_peek_at(parser, -1);
  Str name = str_clone(parser, parser_consume(parser, TOK_IDENT, "expected identifier after binding keyword")->lexeme);

  AstTypeRef* type = NULL;
  if (parser_match(parser, TOK_COLON)) {
    type = parse_type_ref(parser);
  }

  bool is_bind = false;
  AstExpr* value = NULL;
  if (parser_match(parser, TOK_ASSIGN)) {
    value = parse_expression(parser);
  } else if (parser_match(parser, TOK_ARROW)) {
    is_bind = true;
    value = parse_expression(parser);
  }
  if (is_const && value == NULL) {
    parser_error(parser, token, "'const' requires an initializer");
  }

  AstDecl* decl = parser_alloc(parser, sizeof(AstDecl));
  decl->kind = AST_DECL_GLOBAL_LET;
  decl->line = token->line;
  decl->column = token->column;
  decl->as.let_decl.name = name;
  decl->as.let_decl.type = type;
  decl->as.let_decl.is_bind = is_bind;
  decl->as.let_decl.is_var = is_var;
  decl->as.let_decl.is_const = is_const;
  decl->as.let_decl.value = value;
  decl->next = NULL;
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
  if (parser_match(parser, TOK_KW_LET)) {
    return parse_global_let_declaration(parser, false, false);
  }
  if (parser_match(parser, TOK_KW_VAR)) {
    return parse_global_let_declaration(parser, true, false);
  }
  if (parser_match(parser, TOK_KW_CONST)) {
    return parse_global_let_declaration(parser, false, true);
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
    size_t prev_index = parser.index;
    if (parser_match(&parser, TOK_KW_IMPORT)) {
        imports = append_import(imports, parse_import_clause(&parser, false));
    } else {
        parser_advance(&parser); // consume export
        imports = append_import(imports, parse_import_clause(&parser, true));
    }
    if (parser.index == prev_index) {
        parser_advance(&parser);
    }
  }
  AstDecl* head = NULL;
  while (!parser_check(&parser, TOK_EOF)) {
    size_t prev_index = parser.index;
    AstDecl* decl = parse_declaration(&parser);
    head = append_decl(head, decl);
    if (parser.index == prev_index) {
        parser_advance(&parser);
    }
  }
  module->imports = imports;
  module->decls = head;
  module->had_error = parser.had_error;
  return module;
}

