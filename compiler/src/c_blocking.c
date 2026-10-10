/* Calls to `blocking` externs from a lightweight task
 * (docs/lightweight-spawn-design.md §4.4, §17). A `blocking` extern is C that
 * may wait for a long time (file I/O, a name lookup, a child process). In a
 * resumable twin (c_twin.c) such a call does not hold the worker: its
 * arguments are packed, the call runs on a blocking-call thread while the
 * task suspends, and the task reads the result when it is woken.
 *
 * Per blocking extern this emits, ahead of the twins:
 *
 *   typedef struct { <param C types> f0..; RetT result; } __raeblock_args_F;
 *   __raeblock_args_F* __raeblock_pack_F(<the extern's parameters>);
 *   void __raeblock_run_F(void* args);   the call itself, result stored
 *
 * The pack takes exactly the extern's C parameters, so a call site packs by
 * the normal call's own argument text with the function swapped (every
 * argument lowered as for the call). */
#include "c_backend_internal.h"
#include "mangler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void c_blocking_emit_helpers(CompilerContext* ctx, const AstModule* module, FILE* out) {
  const char* seen[256];
  int seen_count = 0;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* decl = ctx->all_decls[i];
    if (decl->kind != AST_DECL_FUNC) continue;
    const AstFuncDecl* func = &decl->as.func_decl;
    if (!func->is_extern || !func->is_blocking || func->generic_params) continue;
    const char* symbol = rae_mangle_function(ctx, func);
    bool duplicate = false;
    for (int k = 0; k < seen_count; k++) {
      if (strcmp(seen[k], symbol) == 0) duplicate = true;
    }
    if (duplicate || seen_count >= 256) continue;
    seen[seen_count++] = symbol;
    CFuncContext tctx = { .compiler_ctx = ctx, .module = module, .func_decl = func };
    const char* returns = c_return_type(&tctx, func);
    bool is_void = strcmp(returns, "void") == 0;
    int count = 0;
    fprintf(out, "typedef struct { ");
    for (const AstParam* param = func->params; param; param = param->next, count++) {
      emit_param_c_type(&tctx, param, out, false);
      fprintf(out, " f%d; ", count);
    }
    /* The result has the C function's own type (a Rae declaration may wrap
     * it, as `opt String` does) */
    if (!is_void) {
      fprintf(out, "__typeof__(%s(", symbol);
      int k = 0;
      for (const AstParam* param = func->params; param; param = param->next, k++) {
        fprintf(out, "%s(*(", k ? ", " : "");
        emit_param_c_type(&tctx, param, out, false);
        fprintf(out, "*)0)");
      }
      fprintf(out, ")) result; ");
    }
    fprintf(out, "char unused; } __raeblock_args_%s;\n", symbol);
    fprintf(out, "RAE_UNUSED static void* __raeblock_pack_%s(", symbol);
    emit_param_list(&tctx, func->params, out, false);
    fprintf(out, ") {\n  __raeblock_args_%s* __a = (__raeblock_args_%s*)calloc(1, sizeof(__raeblock_args_%s));\n",
            symbol, symbol, symbol);
    count = 0;
    for (const AstParam* param = func->params; param; param = param->next, count++) {
      fprintf(out, "  memcpy((void*)&__a->f%d, (const void*)&%.*s, sizeof(__a->f%d));\n", count,
              (int)param->name.len, param->name.data, count);
    }
    fprintf(out, "  return __a;\n}\n");
    fprintf(out, "RAE_UNUSED static void __raeblock_run_%s(void* __vp) {\n", symbol);
    fprintf(out, "  __raeblock_args_%s* __a = (__raeblock_args_%s*)__vp;\n  ", symbol, symbol);
    if (!is_void) fprintf(out, "__a->result = ");
    fprintf(out, "%s(", symbol);
    for (int k = 0; k < count; k++) fprintf(out, "%s__a->f%d", k ? ", " : "", k);
    fprintf(out, ");\n}\n");
  }
}

/* The span of the call `symbol(...)` in `text`: its start and the index
 * just past its closing parenthesis (string and character literals
 * skipped), or false */
bool c_blocking_find_call(const char* text, const char* symbol, size_t* start, size_t* end) {
  size_t length = strlen(symbol);
  for (const char* at = strstr(text, symbol); at; at = strstr(at + 1, symbol)) {
    if (at > text && (at[-1] == '_' || (at[-1] >= 'a' && at[-1] <= 'z') || (at[-1] >= 'A' && at[-1] <= 'Z') ||
                      (at[-1] >= '0' && at[-1] <= '9'))) {
      continue;
    }
    if (at[length] != '(') continue;
    int depth = 0;
    for (const char* p = at + length; *p; p++) {
      if (*p == '"' || *p == '\'') {
        char quote = *p;
        for (p++; *p && *p != quote; p++) {
          if (*p == '\\' && p[1]) p++;
        }
        if (!*p) return false;
        continue;
      }
      if (*p == '(') depth++;
      if (*p == ')' && --depth == 0) {
        *start = (size_t)(at - text);
        *end = (size_t)(p - text) + 1;
        return true;
      }
    }
    return false;
  }
  return false;
}
