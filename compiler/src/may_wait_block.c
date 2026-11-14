/* A sleep a mainLoop body can reach, in a browser build (docs/web-frame-loop.md
 * §7): there a frame has no stack Asyncify can return to the page through, so
 * the runtime holds such a sleep (it spins until it ends, with one console
 * warning). This tells the programmer at compile time, at the mainLoop
 * statement, naming the call path. The body's calls are walked into a node
 * of their own (wait_graph_walk_block); a spawn there is no edge, so a sleep
 * in a spawned task, which suspends between frames, does not count. */
#include "may_wait.h"

#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "wait_graph.h"

/* Whether lib/Target's `os` (written per build, main.c apply_build_target) is web */
static bool build_targets_web(const AstModule* module) {
  for (const AstDecl* d = module->decls; d; d = d->next) {
    if (d->kind != AST_DECL_GLOBAL_LET || !d->module_name || strcmp(d->module_name, "Target") != 0) continue;
    if (!str_eq_cstr(d->as.let_decl.name, "os")) continue;
    const AstExpr* value = d->as.let_decl.value;
    return value && value->kind == AST_EXPR_MEMBER && str_eq_cstr(value->as.member.member, "web");
  }
  return false;
}

/* The call path from `start` to a sleep extern, as "a -> b -> sleep" (the
 * Rae functions; the extern itself only when it is called directly) */
static bool sleep_path(WaitGraph* graph, int start, char* path, size_t cap) {
  int* parent = malloc(graph->count * sizeof(int));
  int* queue = malloc((graph->count + 1) * sizeof(int));
  if (!parent || !queue) {
    free(parent);
    free(queue);
    return false;
  }
  for (size_t i = 0; i < graph->count; i++) parent[i] = -2;
  size_t head = 0, tail = 0;
  queue[tail++] = start;
  parent[start] = -1;
  int found = -1;
  while (head < tail && found < 0) {
    int at = queue[head++];
    const WaitNode* node = &graph->nodes[at];
    for (size_t c = 0; c < node->callee_count; c++) {
      int callee = node->callees[c];
      if (parent[callee] != -2) continue;
      parent[callee] = at;
      if (graph->nodes[callee].direct == WAIT_SLEEP) {
        found = callee;
        break;
      }
      queue[tail++] = callee;
    }
  }
  bool ok = found >= 0;
  if (ok) {
    /* walk back to the body's node, then print forward */
    int chain[64];
    int length = 0;
    for (int at = found; at >= 0 && at != start && length < 64; at = parent[at]) chain[length++] = at;
    size_t used = 0;
    path[0] = '\0';
    for (int i = length - 1; i >= 0; i--) {
      const AstFuncDecl* func = &graph->nodes[chain[i]].decl->as.func_decl;
      if (func->is_extern && length > 1) continue;
      int wrote = snprintf(path + used, cap - used, "%s%.*s", used ? " -> " : "", (int)func->name.len,
                           func->name.data);
      if (wrote < 0 || (size_t)wrote >= cap - used) break;
      used += (size_t)wrote;
    }
  }
  free(parent);
  free(queue);
  return ok;
}

void may_wait_check_main_loop_sleeps(struct WaitGraph* graph, const AstModule* module) {
  if (!graph || !module || !build_targets_web(module)) return;
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind != AST_DECL_FUNC || !str_eq_cstr(decl->as.func_decl.name, "main")) continue;
    const AstBlock* body = decl->as.func_decl.body;
    if (!body) continue;
    for (const AstStmt* stmt = body->first; stmt; stmt = stmt->next) {
      if (stmt->kind != AST_STMT_LOOP || !stmt->as.loop_stmt.is_main_loop) continue;
      int node = wait_graph_walk_block(graph, decl, stmt->as.loop_stmt.body);
      char path[512];
      if (!sleep_path(graph, node, path, sizeof path)) continue;
      char message[900];
      snprintf(message, sizeof message,
               "in the browser this mainLoop body can reach a sleep (%s): a frame cannot let the page run, so "
               "the sleep holds the page until it ends; return from the frame and go on in a later one, or "
               "sleep in a spawned task (docs/web-frame-loop.md §7)",
               path);
      diag_warn(decl->origin_file ? decl->origin_file : module->file_path, (int)stmt->line, (int)stmt->column,
                message);
    }
  }
}
