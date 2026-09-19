#include "array_methods.h"
#include "lexer.h"
#include "parser.h"

#include <stdio.h>
#include <string.h>

/* The module, with `%lld` standing for the cap. Mirrors lib/core/List.rae:
 * indexed reads are optional, `copyAtFallback` is the total read, `set`
 * past the end is bounds-checked, ignored and reported on stderr
 * (docs/collections.md), `length` is the cap. `this[index]` inside `unsafe`
 * is the raw slot — the ONE place in the language that indexes an Array. */
static const char* const array_module_template =
  "func copyAt(T: type, this: view Array(T, cap: %lld), index: view Int) ret opt T {\n"
  "  if index < 0 or index >= %lld {\n"
  "    ret none\n"
  "  }\n"
  "  unsafe {\n"
  "    ret this[index]\n"
  "  }\n"
  "}\n"
  "\n"
  "func copyAtFallback(\n"
  "  T: type\n"
  "  this: view Array(T, cap: %lld)\n"
  "  index: view Int\n"
  "  fallback: copy T\n"
  ") ret T {\n"
  "  if let value: T = copyAt(this, index: index) {\n"
  "    ret value\n"
  "  }\n"
  "  ret fallback\n"
  "}\n"
  "\n"
  "func viewAt(T: type, this: view Array(T, cap: %lld), index: view Int) ret opt view T {\n"
  "  if index < 0 or index >= %lld {\n"
  "    ret none\n"
  "  }\n"
  "  unsafe {\n"
  "    ret view this[index]\n"
  "  }\n"
  "}\n"
  "\n"
  "func modAt(T: type, this: mod Array(T, cap: %lld), index: view Int) ret opt mod T {\n"
  "  if index < 0 or index >= %lld {\n"
  "    ret none\n"
  "  }\n"
  "  unsafe {\n"
  "    ret mod this[index]\n"
  "  }\n"
  "}\n"
  "\n"
  "func set(\n"
  "  T: type\n"
  "  this: mod Array(T, cap: %lld)\n"
  "  index: view Int\n"
  "  value: own T\n"
  ") {\n"
  "  if index < 0 or index >= %lld {\n"
  "    runtimeWarning(message: \"Array.set: index {index} is out of range for length %lld\")\n"
  "    ret\n"
  "  }\n"
  "  unsafe {\n"
  "    this[index] = value\n"
  "  }\n"
  "}\n"
  "\n"
  "func length(T: type, this: view Array(T, cap: %lld)) ret Int {\n"
  "  ret %lld\n"
  "}\n";

AstDecl* array_methods_synthesize(Arena* arena, int64_t cap) {
  long long n = (long long)cap;
  int len = snprintf(NULL, 0, array_module_template, n, n, n, n, n, n, n, n, n, n, n, n);
  if (len <= 0) return NULL;
  char* source = arena_alloc(arena, (size_t)len + 1);
  snprintf(source, (size_t)len + 1, array_module_template, n, n, n, n, n, n, n, n, n, n, n, n);
  char name[64];
  snprintf(name, sizeof name, "<core/Array cap %lld>", n);
  char* file_path = arena_alloc(arena, strlen(name) + 1);
  memcpy(file_path, name, strlen(name) + 1);
  TokenList tokens = lexer_tokenize(arena, file_path, source, (size_t)len, true);
  if (tokens.had_error) return NULL;
  AstModule* module = parse_module(arena, file_path, tokens);
  if (!module || module->had_error) return NULL;
  static const char* const module_name = "core/Array";
  for (AstDecl* d = module->decls; d; d = d->next) {
    d->module_name = module_name;
    if (d->kind == AST_DECL_FUNC) d->as.func_decl.module_name = module_name;
  }
  return module->decls;
}
