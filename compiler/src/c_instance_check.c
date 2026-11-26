// c_instance_check.c — RAE_CHECK_INSTANCES=1 (docs/generic-instances-design.md,
// step 4): the instances of generic functions sema made while it analyzed the
// program, against those the backend found (c_discovery.c) and emitted. Each
// difference is printed on stderr, one line each, with the instance's C name:
//   [instances] backend only: <name>   (sema did not decide that call)
//   [instances] sema only: <name>      (the backend did not emit it)

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

bool c_instances_wanted(void) {
  const char* value = getenv("RAE_CHECK_INSTANCES");
  return value && strcmp(value, "1") == 0;
}

/* Prints the differences; their number */
size_t c_check_instances(CompilerContext* ctx) {
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
  size_t differences = 0, i = 0, j = 0;
  while (i < backend_count || j < sema_count) {
    int order = i >= backend_count ? 1 : j >= sema_count ? -1 : strcmp(backend[i], sema[j]);
    if (order == 0) { i++; j++; continue; }
    if (order < 0) fprintf(stderr, "[instances] backend only: %s\n", backend[i++]);
    else fprintf(stderr, "[instances] sema only: %s\n", sema[j++]);
    differences++;
  }
  fprintf(stderr, "[instances] %zu by the backend, %zu by sema, %zu differ\n", backend_count, sema_count, differences);
  free(backend);
  free(sema);
  return differences;
}
