/* `rae build --report-waits` (docs/lightweight-spawn-design.md §11): the
 * may-wait graph of may_wait.c, printed. */
#include "may_wait.h"

#include <stdlib.h>
#include <string.h>

#include "wait_graph.h"

static void print_function_name(FILE* out, const WaitNode* node) {
  const AstFuncDecl* func = &node->decl->as.func_decl;
  fprintf(out, "%.*s", (int)func->name.len, func->name.data);
  if (node->key) fprintf(out, "(%s)", node->key);
}

/* f -> g -> h -> sleep */
static void print_chain(FILE* out, const WaitGraph* graph, int index) {
  for (int hops = 0; index >= 0 && hops < 10; hops++) {
    const WaitNode* node = &graph->nodes[index];
    if (hops > 0) fprintf(out, " -> ");
    print_function_name(out, node);
    if (node->via < 0) {
      fprintf(out, " [%s]", wait_kind_name(node->direct));
      return;
    }
    index = node->via;
  }
  fprintf(out, " -> ...");
}

bool may_wait_report(CompilerContext* ctx, const AstModule* merged, const char* entry, bool with_lib, FILE* out) {
  WaitGraph graph;
  wait_graph_build(&graph, ctx, merged);

  /* Every count is split: [0] the program, [1] lib/. The default report
   * prints the program's (lib/ grows; a report on a program should not move
   * with it) and names the lib/ twins; `with_lib` adds the lib/ side. */
  size_t functions[2] = { 0, 0 }, specializations[2] = { 0, 0 }, may_wait[2] = { 0, 0 };
  size_t spawned[2] = { 0, 0 }, reachable[2] = { 0, 0 }, twins[2] = { 0, 0 };
  for (size_t i = 0; i < graph.count; i++) {
    const WaitNode* node = &graph.nodes[i];
    int scope = wait_graph_is_stdlib(&graph, node->decl) ? 1 : 0;
    functions[scope]++;
    if (node->key) specializations[scope]++;
    if (node->may_wait) may_wait[scope]++;
    if (node->spawned) spawned[scope]++;
    if (node->reachable) reachable[scope]++;
    if (node->may_wait && node->reachable) twins[scope]++;
  }
  int scopes = with_lib ? 2 : 1;
  fprintf(out, "may-wait report: %s\n", wait_graph_file_name(entry));
  for (int scope = 0; scope < scopes; scope++) {
    fprintf(out, "  %s\n", scope == 0 ? "the program:" : "lib/:");
    fprintf(out, "    functions: %zu (%zu generic specializations)\n", functions[scope], specializations[scope]);
    fprintf(out, "    wait primitive sites: Task.get %zu, sleep extern %zu, socket/poller extern %zu, task join %zu, "
                 "blocking extern %zu\n",
            graph.primitive_sites[scope][WAIT_TASK_GET], graph.primitive_sites[scope][WAIT_SLEEP],
            graph.primitive_sites[scope][WAIT_SOCKET], graph.primitive_sites[scope][WAIT_JOIN],
            graph.primitive_sites[scope][WAIT_BLOCKING]);
    fprintf(out, "    calls resolved conservatively (to every candidate): %zu\n", graph.approximate_calls[scope]);
    fprintf(out, "    may wait: %zu functions\n", may_wait[scope]);
    fprintf(out, "    spawn sites: %zu, spawned functions: %zu, reachable from a spawn: %zu\n", graph.spawn_sites[scope],
            spawned[scope], reachable[scope]);
  }
  fprintf(out, "  resumable twins (may wait and reachable from a spawn): %zu in the program, %zu in lib/\n", twins[0],
          twins[1]);
  for (int scope = 0; scope < 2; scope++) {
    if (scope == 1 && !with_lib) break;
    for (size_t i = 0; i < graph.count; i++) {
      const WaitNode* node = &graph.nodes[i];
      if (!(node->may_wait && node->reachable) || (wait_graph_is_stdlib(&graph, node->decl) ? 1 : 0) != scope) continue;
      fprintf(out, "    ");
      print_function_name(out, node);
      fprintf(out, "  %s:%zu%s\n      ", scope ? wait_graph_short_lib_path(node->decl->origin_file) : wait_graph_file_name(node->decl->origin_file),
              node->decl->line, node->spawned ? "  spawned" : "");
      print_chain(out, &graph, (int)i);
      fprintf(out, "\n");
    }
  }
  if (twins[1] && !with_lib) {
    fprintf(out, "    lib/:");
    for (size_t i = 0; i < graph.count; i++) {
      const WaitNode* node = &graph.nodes[i];
      if (!(node->may_wait && node->reachable) || !wait_graph_is_stdlib(&graph, node->decl)) continue;
      fprintf(out, " ");
      print_function_name(out, node);
    }
    fprintf(out, "\n");
  }
  wait_graph_release(&graph);
  return true;
}
