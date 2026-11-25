/* The result of a task (docs/concurrency-model.md, `get`). A result that owns
 * memory belongs to the task until `get` or `tryGet` moves it out, once; one
 * that nobody took is freed with the task. The task is told how when it is
 * spawned: a drop function per result type, emitted here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_backend_internal.h"
#include "mangler.h"
#include "ownership.h"
#include "sema.h"

/* Does a value of this type own memory (or a thread)? */
bool c_task_result_owns(CFuncContext* ctx, const AstTypeRef* type) {
  return type && !type->is_view && !type->is_mod
      && type_needs_cascade_drop(ctx->compiler_ctx, ctx->module, type, 0);
}

/* The name of the drop function of a task whose result has this type, or NULL */
static const char* task_drop_name(CFuncContext* ctx, const AstTypeRef* type, char* buffer, size_t size) {
  if (!c_task_result_owns(ctx, type)) return NULL;
  AstTypeRef plain = *type;
  plain.is_own = false;
  plain.is_copy = false;
  plain.next = NULL;
  snprintf(buffer, size, "__raetask_drop_%s", rae_mangle_type_specialized(ctx->compiler_ctx, NULL, NULL, &plain));
  return buffer;
}

/* `rae_task_new(...)` for a spawn of `callee` */
void c_emit_task_new(CFuncContext* ctx, const TypeInfo* result, const AstFuncDecl* callee, FILE* out) {
  fprintf(out, "rae_task_new(");
  if (!result || result->kind == TYPE_VOID) {
    fprintf(out, "0, NULL)");
    return;
  }
  fprintf(out, "sizeof(");
  emit_type_info_as_c_type(ctx, (TypeInfo*)result, out);
  char buffer[1200];
  const char* drop = callee && !callee->generic_params && callee->returns && !callee->returns->next
      ? task_drop_name(ctx, callee->returns->type, buffer, sizeof buffer) : NULL;
  fprintf(out, "), %s)", drop ? drop : "NULL");
}

/* The drop functions, for the result of every function that could be spawned.
 * Into a buffer: a List's drop may be registered here, and is declared first. */
void c_emit_task_drops(CompilerContext* ctx, const AstModule* module, FILE* out) {
  char (*done)[1200] = NULL;
  size_t count = 0, cap = 0;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* d = ctx->all_decls[i];
    if (d->kind != AST_DECL_FUNC || d->as.func_decl.generic_params || d->as.func_decl.specialization_args) continue;
    const AstFuncDecl* f = &d->as.func_decl;
    if (!f->returns || f->returns->next || !f->returns->type) continue;
    CFuncContext fctx = {.compiler_ctx = ctx, .module = module, .func_decl = f};
    char name[1200];
    if (!task_drop_name(&fctx, f->returns->type, name, sizeof name)) continue;
    bool seen = false;
    for (size_t k = 0; k < count && !seen; k++) seen = strcmp(done[k], name) == 0;
    if (seen) continue;
    if (count == cap) {
      cap = cap ? cap * 2 : 32;
      done = realloc(done, cap * sizeof *done);
    }
    memcpy(done[count++], name, sizeof name);
    AstTypeRef type = *f->returns->type;
    type.is_own = false;
    type.is_copy = false;
    type.next = NULL;
    const char* c_type = c_return_type(&fctx, f);
    fprintf(out, "RAE_UNUSED static void %s(void* __vp) {\n", name);
    fprintf(out, "  %s* __result = (%s*)__vp; (void)__result;\n", c_type, c_type);
    emit_drop_for_value(&fctx, out, &type, "(*__result)", true);
    fprintf(out, "}\n");
  }
  free(done);
}
