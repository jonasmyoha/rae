/* lexer.h - Rae language tokenizer */

#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>
#include "arena.h"
#include "str.h"

typedef enum {
  TOK_EOF = 0,
  TOK_ERROR,

  TOK_IDENT,
  TOK_INTEGER,
  TOK_STRING,

  /* Keywords */
  TOK_KW_TYPE,
  TOK_KW_FUNC,
  TOK_KW_DEF,
  TOK_KW_RET,
  TOK_KW_SPAWN,
  TOK_KW_OWN,
  TOK_KW_VIEW,
  TOK_KW_MOD,
  TOK_KW_OPT,
  TOK_KW_IF,
  TOK_KW_ELSE,
  TOK_KW_WHILE,
  TOK_KW_MATCH,
  TOK_KW_CASE,
  TOK_KW_DEFAULT,
  TOK_KW_IMPORT,
  TOK_KW_EXPORT,
  TOK_KW_TRUE,
  TOK_KW_FALSE,
  TOK_KW_NONE,
  TOK_KW_AND,
  TOK_KW_OR,
  TOK_KW_NOT,
  TOK_KW_IS,
  TOK_KW_PUB,
  TOK_KW_PRIV,

  /* Operators */
  TOK_ASSIGN,
  TOK_ARROW,
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  TOK_PERCENT,
  TOK_LESS,
  TOK_GREATER,
  TOK_LESS_EQUAL,
  TOK_GREATER_EQUAL,

  /* Punctuation */
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_LBRACKET,
  TOK_RBRACKET,
  TOK_COMMA,
  TOK_COLON,
  TOK_DOT
} TokenKind;

typedef struct {
  TokenKind kind;
  Str lexeme;
  size_t line;
  size_t column;
} Token;

typedef struct {
  Token* data;
  size_t count;
} TokenList;

TokenList lexer_tokenize(Arena* arena,
                         const char* file_path,
                         const char* source,
                         size_t length);

const char* token_kind_name(TokenKind kind);

#endif /* LEXER_H */
