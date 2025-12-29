/* parser.h - Rae parser interface */

#ifndef PARSER_H
#define PARSER_H

#include "arena.h"
#include "ast.h"
#include "lexer.h"

AstModule* parse_module(Arena* arena, const char* file_path, TokenList tokens);

#endif /* PARSER_H */
