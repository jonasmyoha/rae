// c_instance_check.c — RAE_CHECK_INSTANCES=1 (docs/generic-instances-design.md,
// step 4): the instances of generic functions sema made while it analyzed the
// program, against those the backend found (c_discovery.c) and emitted. Each
// difference is printed on stderr, one line each, with the instance's C name:
//   [instances] backend only: <name>   (sema did not decide a call the C makes)
// Instances only sema has are counted, not listed: sema decides some the C never
// needs (the drop of a type only declared), and they are no fault of step 4.
// An instance only the backend has that no line of the emitted C calls is not a
// difference: discovery registers some it never uses (step 7 removes it).
// RAE_CHECK_INSTANCES=error makes a difference fail the build (the suite stage).

#include "c_backend.h"
#include "c_backend_internal.h"
#include "mangler.h"
#include "type.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_names(const void* a, const void* b) {
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static bool has_body(const AstFuncDecl* f) { return f && !f->is_extern && f->body; }

/* The names, sorted, without repeats */
static size_t sorted_unique(const char** names, size_t count) {
  if (count == 0) return 0;
  qsort(names, count, sizeof *names, compare_names);
  size_t kept = 1;
  for (size_t i = 1; i < count; i++)
    if (strcmp(names[i], names[kept - 1]) != 0) names[kept++] = names[i];
  return kept;
}

/* Every instance sema decided is emitted (docs/generic-instances-design.md,
 * step 4): sema's set is the one step 5 emits from, and one no code reaches is an
 * unused static function (the drop of a type only declared). Called after
 * discovery, for those it did not find: where both have one, the backend's
 * spelling of its types stays (the C of a `view` of an enum differs). The number
 * it added. RAE_SEMA_INSTANCES=0 leaves them out. */
size_t c_register_sema_instances(CompilerContext* ctx) {
  const char* value = getenv("RAE_SEMA_INSTANCES");
  if (value && strcmp(value, "0") == 0) return 0;
  size_t known_count = ctx->specialized_func_count;
  const char** known = malloc(sizeof(char*) * (known_count + 1));
  for (size_t i = 0; i < known_count; i++)
    known[i] = rae_mangle_specialized_function(ctx, ctx->specialized_funcs[i].decl,
                                               ctx->specialized_funcs[i].concrete_args);
  known_count = sorted_unique(known, known_count);
  size_t added = 0;
  for (SpecializationEntry* e = ctx->type_registry ? ctx->type_registry->specializations : NULL; e; e = e->next) {
    if (e->made_for_backend || !e->generic_decl || e->generic_decl->kind != AST_DECL_FUNC) continue;
    const AstFuncDecl* f = &e->generic_decl->as.func_decl;
    const AstDecl* spec = e->specialized_decl;
    if (!has_body(f) || !spec || spec->kind != AST_DECL_FUNC || !spec->as.func_decl.specialization_args) continue;
    const char* name = rae_mangle_specialized_function(ctx, f, spec->as.func_decl.specialization_args);
    if (bsearch(&name, known, known_count, sizeof *known, compare_names)) continue;
    size_t before = ctx->specialized_func_count;
    register_function_specialization(ctx, f, spec->as.func_decl.specialization_args);
    added += ctx->specialized_func_count - before;
  }
  free(known);
  return added;
}

bool c_instances_wanted(void) {
  const char* value = getenv("RAE_CHECK_INSTANCES");
  return value && (strcmp(value, "1") == 0 || strcmp(value, "error") == 0);
}

bool c_instances_strict(void) {
  const char* value = getenv("RAE_CHECK_INSTANCES");
  return value && strcmp(value, "error") == 0;
}

/* The emitted C, NULL when it cannot be read */
static char* read_text(const char* path) {
  FILE* file = path ? fopen(path, "rb") : NULL;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* text = malloc((size_t)size + 1);
  size_t got = fread(text, 1, (size_t)size, file);
  text[got] = '\0';
  fclose(file);
  return text;
}

/* The name of the function whose body holds `at`: the last line before it that
 * starts at column 0 and opens a body, NULL outside any */
static size_t enclosing_function(const char* text, const char* at, const char** name) {
  const char* line = at;
  while (line > text) {
    while (line > text && line[-1] != '\n') line--;
    const char* end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    if (line[0] != ' ' && line[0] != '}' && line[0] != '#' && end > line && end[-1] == '{') {
      const char* open = memchr(line, '(', (size_t)(end - line));
      if (!open) return 0;
      const char* start = open;
      while (start > line && start[-1] != ' ' && start[-1] != '*') start--;
      *name = start;
      return (size_t)(open - start);
    }
    if (line == text) break;
    line--;
  }
  return 0;
}

static bool in_list(const char** names, size_t count, const char* name, size_t length) {
  for (size_t i = 0; i < count; i++)
    if (strlen(names[i]) == length && strncmp(names[i], name, length) == 0) return true;
  return false;
}

/* Whether `name(` is called on a line of `text` that is not the function's own
 * prototype or definition (those start with RAE_UNUSED static), and not in the
 * body of a function of `unused` */
static bool is_called(const char* text, const char* name, const char** unused, size_t unused_count) {
  if (!text) return true;
  size_t length = strlen(name);
  for (const char* at = strstr(text, name); at; at = strstr(at + 1, name)) {
    if (at[length] != '(' || (at > text && at[-1] != ' ' && at[-1] != '(' && at[-1] != '*' && at[-1] != '!'
                               && at[-1] != ',' && at[-1] != '=' && at[-1] != '&' && at[-1] != '\t')) continue;
    const char* line = at;
    while (line > text && line[-1] != '\n') line--;
    if (strncmp(line, "RAE_UNUSED static ", 18) == 0) continue;
    const char* caller = NULL;
    size_t caller_length = enclosing_function(text, at, &caller);
    if (caller_length && in_list(unused, unused_count, caller, caller_length)) continue;
    return true;
  }
  return false;
}

size_t c_check_instances(CompilerContext* ctx, const char* c_path) {
  char* text = read_text(c_path);
  size_t unused = 0, sema_only = 0;
  size_t backend_count = 0, sema_count = 0;
  const char** backend = malloc(sizeof(char*) * (ctx->specialized_func_count + 1));
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
    const AstFuncDecl* f = ctx->specialized_funcs[i].decl;
    if (!has_body(f) || !f->generic_params) continue;
    backend[backend_count++] = rae_mangle_specialized_function(ctx, f, ctx->specialized_funcs[i].concrete_args);
  }
  size_t entries = 0;
  for (SpecializationEntry* e = ctx->type_registry ? ctx->type_registry->specializations : NULL; e; e = e->next)
    entries++;
  const char** sema = malloc(sizeof(char*) * (entries + 1));
  for (SpecializationEntry* e = ctx->type_registry ? ctx->type_registry->specializations : NULL; e; e = e->next) {
    if (e->made_for_backend || !e->generic_decl || e->generic_decl->kind != AST_DECL_FUNC) continue;
    const AstFuncDecl* f = &e->generic_decl->as.func_decl;
    const AstDecl* spec = e->specialized_decl;
    if (!has_body(f) || !spec || spec->kind != AST_DECL_FUNC) continue;
    sema[sema_count++] = rae_mangle_specialized_function(ctx, f, spec->as.func_decl.specialization_args);
  }
  backend_count = sorted_unique(backend, backend_count);
  sema_count = sorted_unique(sema, sema_count);
  /* the backend's own that nothing calls, also through each other (`grow` in an
   * unused `add`), until no more come */
  const char** never = malloc(sizeof(char*) * (backend_count + 1));
  size_t never_count = 0;
  for (bool grew = true; grew;) {
    grew = false;
    for (size_t a = 0, b = 0; a < backend_count; a++) {
      while (b < sema_count && strcmp(sema[b], backend[a]) < 0) b++;
      if (b < sema_count && strcmp(sema[b], backend[a]) == 0) continue;
      if (in_list(never, never_count, backend[a], strlen(backend[a]))) continue;
      if (!is_called(text, backend[a], never, never_count)) { never[never_count++] = backend[a]; grew = true; }
    }
  }
  size_t differences = 0, i = 0, j = 0;
  while (i < backend_count || j < sema_count) {
    int order = i >= backend_count ? 1 : j >= sema_count ? -1 : strcmp(backend[i], sema[j]);
    if (order == 0) { i++; j++; continue; }
    if (order < 0 && in_list(never, never_count, backend[i], strlen(backend[i]))) { unused++; i++; continue; }
    if (order < 0) fprintf(stderr, "[instances] backend only: %s\n", backend[i++]);
    else { sema_only++; j++; continue; }
    differences++;
  }
  if (differences > 0 || !c_instances_strict())
    fprintf(stderr, "[instances] %zu by the backend, %zu by sema, %zu differ (%zu the backend made and never calls, "
            "%zu only sema made)\n", backend_count, sema_count, differences, unused, sema_only);
  free(never);
  free(text);
  free(backend);
  free(sema);
  return differences;
}
