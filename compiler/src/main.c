/* main.c - Rae compiler entry point */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include "arena.h"
#include "str.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "pretty.h"
#include "vm.h"
#include "vm_compiler.h"

typedef struct {
  const char* input_path;
  const char* output_path;
  bool write_in_place;
} FormatOptions;

static void format_options_init(FormatOptions* opts) {
  opts->input_path = NULL;
  opts->output_path = NULL;
  opts->write_in_place = false;
}

static bool parse_format_args(int argc, char** argv, FormatOptions* opts) {
  format_options_init(opts);
  int i = 0;
  while (i < argc) {
    const char* arg = argv[i];
    if (strcmp(arg, "--write") == 0 || strcmp(arg, "-w") == 0) {
      opts->write_in_place = true;
      i += 1;
    } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s expects a file path\n", arg);
        return false;
      }
      opts->output_path = argv[i + 1];
      i += 2;
    } else if (arg[0] == '-') {
      fprintf(stderr, "error: unknown format option '%s'\n", arg);
      return false;
    } else {
      if (opts->input_path) {
        fprintf(stderr, "error: multiple input files provided ('%s' and '%s')\n", opts->input_path, arg);
        return false;
      }
      opts->input_path = arg;
      i += 1;
    }
  }
  if (!opts->input_path) {
    fprintf(stderr, "error: format command requires a file argument\n");
    return false;
  }
  if (opts->write_in_place && opts->output_path) {
    fprintf(stderr, "error: --write and --output cannot be used together\n");
    return false;
  }
  return true;
}

static void print_usage(const char* prog) {
  fprintf(stderr, "Usage: %s <command> <file>\n", prog);
  fprintf(stderr, "\nCommands:\n");
  fprintf(stderr, "  lex <file>      Tokenize Rae source file\n");
  fprintf(stderr, "  parse <file>    Parse Rae source file and dump AST\n");
  fprintf(stderr, "  format <file>   Parse Rae source file and pretty-print it\n");
  fprintf(stderr, "  run <file>      Execute Rae source via the bytecode VM\n");
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

static int run_vm_file(const char* file_path) {
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
  AstModule* module = parse_module(arena, file_path, tokens);
  if (!module) {
    arena_destroy(arena);
    free(source);
    return 1;
  }

  Chunk chunk;
  if (!vm_compile_module(module, &chunk, file_path)) {
    arena_destroy(arena);
    free(source);
    return 1;
  }

  VM vm;
  vm_init(&vm);
  VMResult result = vm_run(&vm, &chunk);
  chunk_free(&chunk);
  arena_destroy(arena);
  free(source);
  return result == VM_RUNTIME_OK ? 0 : 1;
}

static int run_command(const char* cmd, int argc, char** argv) {
  size_t file_size = 0;
  char* source = NULL;
  Arena* arena = NULL;
  const char* file_path = NULL;
  FormatOptions format_opts;
  bool is_format = (strcmp(cmd, "format") == 0);
  bool is_run = (strcmp(cmd, "run") == 0);

  if (is_format) {
    if (!parse_format_args(argc, argv, &format_opts)) {
      return 1;
    }
    file_path = format_opts.input_path;
  } else if (strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0) {
    if (argc < 1) {
      fprintf(stderr, "error: %s command requires a file argument\n", cmd);
      print_usage(argv[-1]);
      return 1;
    }
    file_path = argv[0];
  } else if (is_run) {
    if (argc < 1) {
      fprintf(stderr, "error: run command requires a file argument\n");
      print_usage(argv[-1]);
      return 1;
    }
    return run_vm_file(argv[0]);
  } else {
    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    print_usage(argv[-1]);
    return 1;
  }

  source = read_file(file_path, &file_size);
  if (!source) {
    fprintf(stderr, "error: could not read file '%s'\n", file_path);
    return 1;
  }
  arena = arena_create(1024 * 1024);
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
  } else if (is_format) {
    AstModule* module = parse_module(arena, format_opts.input_path, tokens);
    if (format_opts.write_in_place || format_opts.output_path) {
      const char* output_path = format_opts.write_in_place ? format_opts.input_path : format_opts.output_path;
      FILE* out = fopen(output_path, "w");
      if (!out) {
        fprintf(stderr, "error: could not open '%s' for writing: %s\n", output_path, strerror(errno));
        arena_destroy(arena);
        free(source);
        return 1;
      }
      pretty_print_module(module, out);
      fclose(out);
    } else {
      pretty_print_module(module, stdout);
    }
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
  if ((strcmp(cmd, "lex") == 0 || strcmp(cmd, "parse") == 0 || strcmp(cmd, "format") == 0 ||
       strcmp(cmd, "run") == 0)) {
    return run_command(cmd, argc - 2, argv + 2);
  }

  fprintf(stderr, "error: unknown command '%s'\n", cmd);
  print_usage(argv[0]);
  return 1;
}
