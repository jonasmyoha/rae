/* main.c - Rae compiler entry point */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "arena.h"
#include "str.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"

static void print_usage(const char* prog) {
  fprintf(stderr, "Usage: %s <command> <file>\n", prog);
  fprintf(stderr, "\nCommands:\n");
  fprintf(stderr, "  lex <file>      Tokenize Rae source file\n");
  fprintf(stderr, "  parse <file>    Parse Rae source file and dump AST\n");
}

static void dump_tokens(const TokenList* tokens) {
  for (size_t i = 0; i < tokens->count; ++i) {
    const Token* tok = &tokens->data[i];
    const char* name = token_kind_name(tok->kind);
    printf("%s \"", name);
    fwrite(tok->lexeme.data, 1, tok->lexeme.len, stdout);
    printf("\" at %zu:%zu\n", tok->line, tok->column);
  }
}

static int run_command(const char* cmd, const char* file_path) {
  size_t file_size = 0;
  char* source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read file '%s'\n", file_path);
    return 1;
  }

  Arena* arena = arena_create(1024 * 1024);
  if (!arena) {
    free(source);
    diag_fatal("could not allocate arena");
  }

  TokenList tokens = lexer_tokenize(arena, file_path, source, file_size);

  if (strcmp(cmd, "lex") == 0) {
    dump_tokens(&tokens);
  } else if (strcmp(cmd, "parse") == 0) {
    AstModule* module = parse_module(arena, file_path, tokens);
    ast_dump_module(module, stdout);
  } else {
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    arena_destroy(arena);
    free(source);
    return 1;
  }

  arena_destroy(arena);
  free(source);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  const char* cmd = argv[1];
  if ((strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0)) {
    if (argc < 3) {
      fprintf(stderr, "error: %s command requires a file argument\n", cmd);
      print_usage(argv[0]);
      return 1;
    }
    return run_command(cmd, argv[2]);
  }

  fprintf(stderr, "error: unknown command '%s'\n", cmd);
  print_usage(argv[0]);
  return 1;
}
