/* lexer.c - Rae language lexer implementation */

#include "lexer.h"

#include "diag.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char* file_path;
  const char* input;
  size_t length;
  size_t index;
  size_t line;
  size_t column;
  int interpolation_depth;
} Lexer;

typedef struct {
  Token* data;
  size_t count;
  size_t capacity;
} TokenBuffer;

typedef struct {
  const char* text;
  TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"and", TOK_KW_AND},     {"case", TOK_KW_CASE},    {"def", TOK_KW_DEF},
    {"default", TOK_KW_DEFAULT},{"else", TOK_KW_ELSE},   {"export", TOK_KW_EXPORT},
    {"extern", TOK_KW_EXTERN},{"false", TOK_KW_FALSE}, {"func", TOK_KW_FUNC},
    {"id", TOK_KW_ID},       {"if", TOK_KW_IF},       {"import", TOK_KW_IMPORT},
    {"in", TOK_KW_IN},       {"is", TOK_KW_IS},       {"key", TOK_KW_KEY},
    {"loop", TOK_KW_LOOP},    {"match", TOK_KW_MATCH},
    {"mod", TOK_KW_MOD},     {"none", TOK_KW_NONE},    {"not", TOK_KW_NOT},
    {"opt", TOK_KW_OPT},     {"or", TOK_KW_OR},
    {"pack", TOK_KW_PACK},   {"priv", TOK_KW_PRIV},    {"pub", TOK_KW_PUB},
    {"ret", TOK_KW_RET},     {"spawn", TOK_KW_SPAWN},  {"true", TOK_KW_TRUE},
    {"type", TOK_KW_TYPE},   {"view", TOK_KW_VIEW}};

static const char* const TOKEN_KIND_NAMES[] = {
    [TOK_EOF] = "TOK_EOF",
    [TOK_ERROR] = "TOK_ERROR",
    [TOK_IDENT] = "TOK_IDENT",
    [TOK_INTEGER] = "TOK_INTEGER",
    [TOK_FLOAT] = "TOK_FLOAT",
    [TOK_STRING] = "TOK_STRING",
    [TOK_STRING_START] = "TOK_STRING_START",
    [TOK_STRING_MID] = "TOK_STRING_MID",
    [TOK_STRING_END] = "TOK_STRING_END",
    [TOK_RAW_STRING] = "TOK_RAW_STRING",
    [TOK_CHAR] = "TOK_CHAR",
    [TOK_COMMENT] = "TOK_COMMENT",
    [TOK_BLOCK_COMMENT] = "TOK_BLOCK_COMMENT",
    [TOK_KW_TYPE] = "TOK_TYPE",
    [TOK_KW_FUNC] = "TOK_FUNC",
    [TOK_KW_DEF] = "TOK_DEF",
    [TOK_KW_RET] = "TOK_RET",
    [TOK_KW_SPAWN] = "TOK_SPAWN",
    [TOK_KW_VIEW] = "TOK_VIEW",
    [TOK_KW_MOD] = "TOK_MOD",
    [TOK_KW_OPT] = "TOK_OPT",
    [TOK_KW_ID] = "TOK_ID",
    [TOK_KW_KEY] = "TOK_KEY",
    [TOK_KW_IF] = "TOK_IF",
    [TOK_KW_ELSE] = "TOK_ELSE",
    [TOK_KW_LOOP] = "TOK_LOOP",
    [TOK_KW_IN] = "TOK_IN",
    [TOK_KW_MATCH] = "TOK_MATCH",
    [TOK_KW_CASE] = "TOK_CASE",
    [TOK_KW_DEFAULT] = "TOK_DEFAULT",
    [TOK_KW_IMPORT] = "TOK_IMPORT",
    [TOK_KW_EXPORT] = "TOK_EXPORT",
    [TOK_KW_EXTERN] = "TOK_EXTERN",
    [TOK_KW_TRUE] = "TOK_TRUE",
    [TOK_KW_FALSE] = "TOK_FALSE",
    [TOK_KW_NONE] = "TOK_NONE",
    [TOK_KW_AND] = "TOK_AND",
    [TOK_KW_OR] = "TOK_OR",
    [TOK_KW_NOT] = "TOK_NOT",
    [TOK_KW_IS] = "TOK_IS",
    [TOK_KW_PUB] = "TOK_PUB",
    [TOK_KW_PACK] = "TOK_KW_PACK",
    [TOK_KW_PRIV] = "TOK_PRIV",
    [TOK_ASSIGN] = "TOK_ASSIGN",
    [TOK_ARROW] = "TOK_ARROW",
    [TOK_PLUS] = "TOK_PLUS",
    [TOK_MINUS] = "TOK_MINUS",
    [TOK_STAR] = "TOK_STAR",
    [TOK_SLASH] = "TOK_SLASH",
    [TOK_PERCENT] = "TOK_PERCENT",
    [TOK_INC] = "TOK_INC",
    [TOK_DEC] = "TOK_DEC",
    [TOK_LESS] = "TOK_LESS",
    [TOK_GREATER] = "TOK_GREATER",
    [TOK_LESS_EQUAL] = "TOK_LESS_EQUAL",
    [TOK_GREATER_EQUAL] = "TOK_GREATER_EQUAL",
    [TOK_LPAREN] = "TOK_LPAREN",
    [TOK_RPAREN] = "TOK_RPAREN",
    [TOK_LBRACE] = "TOK_LBRACE",
    [TOK_RBRACE] = "TOK_RBRACE",
    [TOK_LBRACKET] = "TOK_LBRACKET",
    [TOK_RBRACKET] = "TOK_RBRACKET",
    [TOK_COMMA] = "TOK_COMMA",
    [TOK_COLON] = "TOK_COLON",
    [TOK_DOT] = "TOK_DOT"};

static void lexer_error(Lexer* lexer, size_t line, size_t column, const char* fmt, ...) {
  char buffer[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  diag_error(lexer->file_path, (int)line, (int)column, buffer);
}

static bool lexer_is_at_end(const Lexer* lexer) {
  return lexer->index >= lexer->length;
}

static char lexer_peek(const Lexer* lexer) {
  if (lexer_is_at_end(lexer)) {
    return '\0';
  }
  return lexer->input[lexer->index];
}

static char lexer_advance(Lexer* lexer) {
  if (lexer_is_at_end(lexer)) {
    return '\0';
  }

  char c = lexer->input[lexer->index++];
  if (c == '\n') {
    lexer->line += 1;
    lexer->column = 1;
  } else if (c == '\r') {
    if (!lexer_is_at_end(lexer) && lexer->input[lexer->index] == '\n') {
      lexer->index += 1;
    }
    lexer->line += 1;
    lexer->column = 1;
  } else {
    lexer->column += 1;
  }

  return c;
}

static bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_continue(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9');
}

static void token_buffer_push(TokenBuffer* buffer, Token token) {
  if (buffer->count == buffer->capacity) {
    size_t new_capacity = buffer->capacity ? buffer->capacity * 2 : 64;
    Token* new_data = realloc(buffer->data, new_capacity * sizeof(Token));
    if (!new_data) {
      diag_fatal("out of memory while lexing");
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
  }

  buffer->data[buffer->count++] = token;
}

static void emit_token(Lexer* lexer,
                       TokenBuffer* buffer,
                       TokenKind kind,
                       size_t start_index,
                       size_t line,
                       size_t column) {
  size_t length = lexer->index - start_index;
  Str lexeme = str_from_buf(lexer->input + start_index, length);
  Token token = {.kind = kind, .lexeme = lexeme, .line = line, .column = column};
  token_buffer_push(buffer, token);
}

TokenKind lookup_keyword(Str lexeme) {
  for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); ++i) {
    if (str_eq_cstr(lexeme, KEYWORDS[i].text)) {
      return KEYWORDS[i].kind;
    }
  }
  return TOK_IDENT;
}

static void lexer_skip_whitespace(Lexer* lexer) {
  for (;;) {
    char c = lexer_peek(lexer);
    if (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\n' || c == '\r') {
      lexer_advance(lexer);
      continue;
    }
    break;
  }
}

static void scan_block_comment(Lexer* lexer, TokenBuffer* buffer, size_t start_index, size_t line, size_t col) {
  int depth = 1;
  while (depth > 0 && !lexer_is_at_end(lexer)) {
    char c = lexer_advance(lexer);
    if (c == '#' && lexer_peek(lexer) == '[') {
      lexer_advance(lexer);
      depth++;
    } else if (c == ']' && lexer_peek(lexer) == '#') {
      lexer_advance(lexer);
      depth--;
    }
  }
  
  if (depth > 0) {
    lexer_error(lexer, line, col, "unterminated block comment");
  }
  
  emit_token(lexer, buffer, TOK_BLOCK_COMMENT, start_index, line, col);
}

static void scan_line_comment(Lexer* lexer, TokenBuffer* buffer, size_t start_index, size_t line, size_t col) {
  while (!lexer_is_at_end(lexer) && lexer_peek(lexer) != '\n' && lexer_peek(lexer) != '\r') {
    lexer_advance(lexer);
  }
  emit_token(lexer, buffer, TOK_COMMENT, start_index, line, col);
}

static void scan_number(Lexer* lexer,
                        TokenBuffer* buffer,
                        size_t start_index,
                        size_t line,
                        size_t column,
                        char first_digit) {
  if (first_digit == '0' && isdigit((unsigned char)lexer_peek(lexer))) {
    lexer_error(lexer, line, column, "integer literal cannot contain leading zeros");
  }
  while (isdigit((unsigned char)lexer_peek(lexer))) {
    lexer_advance(lexer);
  }

  bool is_float = false;
  if (lexer_peek(lexer) == '.') {
    // Look ahead to see if there is a digit after the dot
    if (lexer->index + 1 < lexer->length && isdigit((unsigned char)lexer->input[lexer->index + 1])) {
      is_float = true;
      lexer_advance(lexer); // .
      while (isdigit((unsigned char)lexer_peek(lexer))) {
        lexer_advance(lexer);
      }
    }
  }

  emit_token(lexer, buffer, is_float ? TOK_FLOAT : TOK_INTEGER, start_index, line, column);
}

static void scan_identifier(Lexer* lexer,
                            TokenBuffer* buffer,
                            size_t start_index,
                            size_t line,
                            size_t column) {
  while (is_ident_continue(lexer_peek(lexer))) {
    lexer_advance(lexer);
  }

  size_t length = lexer->index - start_index;
  Str lexeme = str_from_buf(lexer->input + start_index, length);
  TokenKind kind = lookup_keyword(lexeme);
  Token token = {.kind = kind, .lexeme = lexeme, .line = line, .column = column};
  token_buffer_push(buffer, token);
}

static void scan_string(Lexer* lexer,
                        TokenBuffer* buffer,
                        size_t start_index,
                        size_t line,
                        size_t column,
                        bool is_continuation) {
  while (!lexer_is_at_end(lexer)) {
    char c = lexer_peek(lexer);
    
    if (c == '"') {
      lexer_advance(lexer);
      TokenKind kind = is_continuation ? TOK_STRING_END : TOK_STRING;
      emit_token(lexer, buffer, kind, start_index, line, column);
      return;
    }
    
    if (c == '{') {
      // Interpolation start
      TokenKind kind = is_continuation ? TOK_STRING_MID : TOK_STRING_START;
      emit_token(lexer, buffer, kind, start_index, line, column);
      lexer->interpolation_depth++;
      return; // Return to main loop to handle '{' as LBRACE
    }

    if (c == '\\') {
      if (lexer->index + 1 >= lexer->length) {
        lexer_error(lexer, line, column, "unterminated string literal");
        return;
      }
      lexer_advance(lexer); // consume backslash
      char next = lexer_peek(lexer);
      if (next == '{' || next == '}' || next == '\\' || next == '"' || 
          next == 'n' || next == 'r' || next == 't' || next == '0') {
        lexer_advance(lexer);
        continue;
      } else if (next == 'u') {
        lexer_advance(lexer); // u
        if (lexer_peek(lexer) != '{') {
          lexer_error(lexer, line, column, "expected '{' after \\u");
        } else {
          lexer_advance(lexer); // {
          while (!lexer_is_at_end(lexer) && lexer_peek(lexer) != '}') {
            char h = lexer_advance(lexer);
            if (!isxdigit((unsigned char)h)) {
               // lexer_error(lexer, ...); 
            }
          }
          if (lexer_peek(lexer) == '}') {
            lexer_advance(lexer); // }
          } else {
            lexer_error(lexer, line, column, "unterminated unicode escape");
          }
        }
        continue;
      } else {
        lexer_error(lexer, lexer->line, lexer->column, "invalid escape sequence '\\%c'", next);
        lexer_advance(lexer);
        continue;
      }
    }
    
    if (c == '\n' || c == '\r') {
       lexer_error(lexer, line, column, "unterminated string literal");
       return;
    }
    
    lexer_advance(lexer);
  }

  lexer_error(lexer, line, column, "unterminated string literal");
}

static void scan_raw_string(Lexer* lexer, TokenBuffer* buffer, size_t start_index, size_t line, size_t col) {
  int hash_count = 0;
  while (lexer_peek(lexer) == '#') {
    lexer_advance(lexer);
    hash_count++;
  }
  
  if (lexer_advance(lexer) != '"') {
    lexer_error(lexer, line, col, "expected '\"' after 'r' and optional '#' in raw string");
    return;
  }
  
  while (!lexer_is_at_end(lexer)) {
    if (lexer_advance(lexer) == '"') {
      int current_hashes = 0;
      while (lexer_peek(lexer) == '#' && current_hashes < hash_count) {
        lexer_advance(lexer);
        current_hashes++;
      }
      if (current_hashes == hash_count) {
        // Terminator found
        emit_token(lexer, buffer, TOK_RAW_STRING, start_index, line, col);
        return;
      }
    }
  }
  
  lexer_error(lexer, line, col, "unterminated raw string");
}

static void scan_char_literal(Lexer* lexer, TokenBuffer* buffer, size_t start_index, size_t line, size_t col) {
  if (lexer_peek(lexer) == '\'') {
    lexer_error(lexer, line, col, "empty char literal");
    lexer_advance(lexer);
    return;
  }

  char c = lexer_advance(lexer);
  if (c == '\\') {
    if (lexer_is_at_end(lexer)) {
      lexer_error(lexer, line, col, "unterminated char literal");
      return;
    }
    char esc = lexer_advance(lexer);
    if (esc == 'u') {
      if (lexer_peek(lexer) != '{') {
        lexer_error(lexer, line, col, "expected '{' after \\u");
      } else {
        lexer_advance(lexer); // {
        while (!lexer_is_at_end(lexer) && lexer_peek(lexer) != '}') {
          char h = lexer_advance(lexer);
          if (!isxdigit((unsigned char)h)) {
             // lexer_error(lexer, ...); // Optional: strict hex check
          }
        }
        if (lexer_peek(lexer) == '}') {
          lexer_advance(lexer); // }
        } else {
          lexer_error(lexer, line, col, "unterminated unicode escape");
        }
      }
    }
  }

  if (lexer_peek(lexer) != '\'') {
    lexer_error(lexer, line, col, "char literal must contain exactly one character");
    while (!lexer_is_at_end(lexer) && lexer_peek(lexer) != '\'') lexer_advance(lexer);
  }
  
  if (!lexer_is_at_end(lexer) && lexer_peek(lexer) == '\'') {
    lexer_advance(lexer); // '
    emit_token(lexer, buffer, TOK_CHAR, start_index, line, col);
  } else {
    lexer_error(lexer, line, col, "unterminated char literal");
  }
}

TokenList lexer_tokenize(Arena* arena,
                         const char* file_path,
                         const char* source,
                         size_t length) {
  Lexer lexer = {
      .file_path = file_path,
      .input = source,
      .length = length,
      .index = 0,
      .line = 1,
      .column = 1,
      .interpolation_depth = 0,
  };

  TokenBuffer buffer = {0};

  for (;;) {
    lexer_skip_whitespace(&lexer);
    if (lexer_is_at_end(&lexer)) {
      Str lexeme = str_from_buf(source + lexer.index, 0);
      Token eof_token = {.kind = TOK_EOF, .lexeme = lexeme, .line = lexer.line, .column = lexer.column};
      token_buffer_push(&buffer, eof_token);
      break;
    }

    size_t start_index = lexer.index;
    size_t token_line = lexer.line;
    size_t token_column = lexer.column;
    char c = lexer_advance(&lexer);

    switch (c) {
      case '#':
        if (lexer_peek(&lexer) == '[') {
          lexer_advance(&lexer);
          scan_block_comment(&lexer, &buffer, start_index, token_line, token_column);
        } else {
          scan_line_comment(&lexer, &buffer, start_index, token_line, token_column);
        }
        break;
      case '(':
        emit_token(&lexer, &buffer, TOK_LPAREN, start_index, token_line, token_column);
        break;
      case ')':
        emit_token(&lexer, &buffer, TOK_RPAREN, start_index, token_line, token_column);
        break;
      case '{':
        emit_token(&lexer, &buffer, TOK_LBRACE, start_index, token_line, token_column);
        break;
      case '}':
        emit_token(&lexer, &buffer, TOK_RBRACE, start_index, token_line, token_column);
        if (lexer.interpolation_depth > 0) {
            lexer.interpolation_depth--;
            scan_string(&lexer, &buffer, lexer.index, lexer.line, lexer.column, true);
        }
        break;
      case '[':
        emit_token(&lexer, &buffer, TOK_LBRACKET, start_index, token_line, token_column);
        break;
      case ']':
        emit_token(&lexer, &buffer, TOK_RBRACKET, start_index, token_line, token_column);
        break;
      case ',':
        emit_token(&lexer, &buffer, TOK_COMMA, start_index, token_line, token_column);
        break;
      case ':':
        emit_token(&lexer, &buffer, TOK_COLON, start_index, token_line, token_column);
        break;
      case '.':
        emit_token(&lexer, &buffer, TOK_DOT, start_index, token_line, token_column);
        break;
      case '+':
        if (lexer_peek(&lexer) == '+') {
          lexer_advance(&lexer);
          emit_token(&lexer, &buffer, TOK_INC, start_index, token_line, token_column);
        } else {
          emit_token(&lexer, &buffer, TOK_PLUS, start_index, token_line, token_column);
        }
        break;
      case '-':
        if (lexer_peek(&lexer) == '-') {
          lexer_advance(&lexer);
          emit_token(&lexer, &buffer, TOK_DEC, start_index, token_line, token_column);
        } else {
          emit_token(&lexer, &buffer, TOK_MINUS, start_index, token_line, token_column);
        }
        break;
      case '*':
        emit_token(&lexer, &buffer, TOK_STAR, start_index, token_line, token_column);
        break;
      case '/':
        emit_token(&lexer, &buffer, TOK_SLASH, start_index, token_line, token_column);
        break;
      case '%':
        emit_token(&lexer, &buffer, TOK_PERCENT, start_index, token_line, token_column);
        break;
      case '=':
        if (lexer_peek(&lexer) == '>') {
          lexer_advance(&lexer);
          emit_token(&lexer, &buffer, TOK_ARROW, start_index, token_line, token_column);
        } else {
          emit_token(&lexer, &buffer, TOK_ASSIGN, start_index, token_line, token_column);
        }
        break;
      case '<':
        if (lexer_peek(&lexer) == '=') {
          lexer_advance(&lexer);
          emit_token(&lexer, &buffer, TOK_LESS_EQUAL, start_index, token_line, token_column);
        } else {
          emit_token(&lexer, &buffer, TOK_LESS, start_index, token_line, token_column);
        }
        break;
      case '>':
        if (lexer_peek(&lexer) == '=') {
          lexer_advance(&lexer);
          emit_token(&lexer, &buffer, TOK_GREATER_EQUAL, start_index, token_line, token_column);
        } else {
          emit_token(&lexer, &buffer, TOK_GREATER, start_index, token_line, token_column);
        }
        break;
      case '"':
        scan_string(&lexer, &buffer, start_index, token_line, token_column, false);
        break;
      case '\'':
        scan_char_literal(&lexer, &buffer, start_index, token_line, token_column);
        break;
      case 'r':
        if (lexer_peek(&lexer) == '"' || lexer_peek(&lexer) == '#') {
          scan_raw_string(&lexer, &buffer, start_index, token_line, token_column);
        } else {
          scan_identifier(&lexer, &buffer, start_index, token_line, token_column);
        }
        break;
      default:
        if (isdigit((unsigned char)c)) {
          scan_number(&lexer, &buffer, start_index, token_line, token_column, c);
        } else if (is_ident_start(c)) {
          scan_identifier(&lexer, &buffer, start_index, token_line, token_column);
        } else {
          lexer_error(&lexer, token_line, token_column, "unexpected character '%c'", c);
        }
        break;
    }
  }

  Token* stored = NULL;
  if (buffer.count > 0) {
    stored = arena_alloc(arena, buffer.count * sizeof(Token));
    if (!stored) {
      free(buffer.data);
      diag_fatal("lexer arena allocation failed");
    }
    memcpy(stored, buffer.data, buffer.count * sizeof(Token));
  }

  free(buffer.data);
  return (TokenList){.data = stored, .count = buffer.count};
}

const char* token_kind_name(TokenKind kind) {
  size_t count = sizeof(TOKEN_KIND_NAMES) / sizeof(TOKEN_KIND_NAMES[0]);
  if (kind < 0 || (size_t)kind >= count || TOKEN_KIND_NAMES[kind] == NULL) {
    return "TOK_UNKNOWN";
  }
  return TOKEN_KIND_NAMES[kind];
}
