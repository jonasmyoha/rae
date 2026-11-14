/* The may-wait analysis behind `rae build --report-waits`
 * (docs/lightweight-spawn-design.md §4.1, §8 step 2), and the question the C
 * backend asks of it: which spawns run as tasks on the scheduler
 * (may_wait_spawn_on_pool, §8 step 3).
 *
 * 1. Wait primitives, a closed list the compiler knows:
 *    - `task.get()` on a Task;
 *    - the externs that block the calling thread: `sleep` and `Time.waitUntil`
 *      (rae_ext_rae_sleep, rae_ext_Time_sleepNs) and the socket / poller
 *      waits (rae_ext_NetSys_pollOne, rae_ext_NetSys_pollerWait);
 *    - a call to a `blocking` extern (C that may wait for long, §4.4);
 *    - the implicit joins: the end of a `taskScope { }`, and a Task local
 *      joined when it goes out of scope.
 * 2. May-wait is a fixed point over the call graph: a function may wait if
 *    it uses a primitive or calls a may-wait function. The call graph is
 *    complete (whole program, no function pointers).
 * 3. Generics are analyzed AFTER specialization: every specialization the
 *    backend instantiates (its discovery pass) is its own node, walked with
 *    its type parameters bound, so `process(Slow)` can wait while
 *    `process(Fast)` does not. A call names its target through sema's
 *    `decl_link`; inside a generic body a call sema could not resolve (it
 *    depends on T) is resolved by name and by the concrete type of its first
 *    argument. When neither decides, the edge goes to every candidate, which
 *    over-approximates; the report counts those calls.
 *    RAE_REPORT_WAITS_DEBUG=1 prints each of those calls on stderr.
 * 4. A function gets a RESUMABLE TWIN when it may wait and is reachable from
 *    a `spawn` (the spawned function included). `main` stays a thread (F5),
 *    so code only main reaches is compiled as today. */
#include "may_wait.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "type.h"
#include "wait_graph.h"
#include "ownership.h"
#include "sema.h"

static size_t hash_pointer(const void* pointer, size_t cap) {
  uintptr_t value = (uintptr_t)pointer;
  value ^= value >> 17;
  value *= 0x9E3779B97F4A7C15ull;
  return (size_t)(value >> 7) & (cap - 1);
}

DeclSlot* wait_graph_decl_slot(WaitGraph* graph, const AstDecl* decl, bool create) {
  if (create && (graph->table_cap == 0 || (graph->count + 1) * 2 > graph->table_cap)) {
    size_t old_cap = graph->table_cap;
    DeclSlot* old = graph->table;
    graph->table_cap = old_cap ? old_cap * 2 : 1024;
    graph->table = calloc(graph->table_cap, sizeof(DeclSlot));
    for (size_t i = 0; i < old_cap; i++) {
      if (!old[i].decl) continue;
      size_t slot = hash_pointer(old[i].decl, graph->table_cap);
      while (graph->table[slot].decl) slot = (slot + 1) & (graph->table_cap - 1);
      graph->table[slot] = old[i];
    }
    free(old);
  }
  if (!graph->table_cap) return NULL;
  size_t slot = hash_pointer(decl, graph->table_cap);
  while (graph->table[slot].decl) {
    if (graph->table[slot].decl == decl) return &graph->table[slot];
    slot = (slot + 1) & (graph->table_cap - 1);
  }
  if (!create) return NULL;
  graph->table[slot].decl = decl;
  graph->table[slot].first = -1;
  return &graph->table[slot];
}

/* The node for (decl, key); NULL key = not a specialization. Added on first
 * sight when `create`. */
int wait_graph_node(WaitGraph* graph, const AstDecl* decl, const char* key, const AstTypeRef* concrete_args,
                      bool create) {
  DeclSlot* slot = wait_graph_decl_slot(graph, decl, create);
  if (!slot) return -1;
  for (int index = slot->first; index >= 0; index = graph->nodes[index].next_same_decl) {
    const char* existing = graph->nodes[index].key;
    if ((!existing && !key) || (existing && key && strcmp(existing, key) == 0)) return index;
  }
  if (!create) return -1;
  if (graph->count == graph->cap) {
    graph->cap = graph->cap ? graph->cap * 2 : 512;
    graph->nodes = realloc(graph->nodes, graph->cap * sizeof(*graph->nodes));
  }
  int index = (int)graph->count++;
  WaitNode* node = &graph->nodes[index];
  memset(node, 0, sizeof(*node));
  node->decl = decl;
  node->key = key ? strdup(key) : NULL;
  node->concrete_args = concrete_args;
  node->via = -1;
  node->pool_safe = -1;
  node->next_same_decl = slot->first;
  slot->first = index;
  return index;
}

void wait_graph_add_callee(WaitNode* node, int callee) {
  for (size_t i = 0; i < node->callee_count; i++) {
    if (node->callees[i] == callee) return;
  }
  if (node->callee_count == node->callee_cap) {
    node->callee_cap = node->callee_cap ? node->callee_cap * 2 : 8;
    node->callees = realloc(node->callees, node->callee_cap * sizeof(int));
  }
  node->callees[node->callee_count++] = callee;
}

/* ----- the fixed points -------------------------------------------------- */

static void graph_solve(WaitGraph* graph) {
  for (size_t i = 0; i < graph->count; i++) graph->nodes[i].may_wait = graph->nodes[i].direct != WAIT_NONE;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < graph->count; i++) {
      WaitNode* node = &graph->nodes[i];
      if (node->may_wait) continue;
      for (size_t c = 0; c < node->callee_count; c++) {
        if (graph->nodes[node->callees[c]].may_wait) {
          node->may_wait = true;
          node->via = node->callees[c];
          changed = true;
          break;
        }
      }
    }
  }
  int* stack = malloc((graph->count + 1) * sizeof(int));
  size_t depth = 0;
  for (size_t i = 0; i < graph->count; i++) {
    if (graph->nodes[i].spawned && !graph->nodes[i].reachable) {
      graph->nodes[i].reachable = true;
      stack[depth++] = (int)i;
    }
  }
  while (depth > 0) {
    WaitNode* node = &graph->nodes[stack[--depth]];
    for (size_t c = 0; c < node->callee_count; c++) {
      WaitNode* callee = &graph->nodes[node->callees[c]];
      if (!callee->reachable) {
        callee->reachable = true;
        stack[depth++] = node->callees[c];
      }
    }
  }
  free(stack);
}

/* ----- the report -------------------------------------------------------- */

/* lib/net/Poller.rae, from wherever the toolchain's lib/ is */
static const char* short_lib_path(const char* path) {
  if (!path) return "?";
  const char* lib = strstr(path, "/lib/");
  return lib ? lib + 1 : path;
}

static const char* file_name(const char* path) {
  if (!path) return "?";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static const AstDecl* decl_of_func(const AstFuncDecl* func) {
  return (const AstDecl*)((const char*)func - offsetof(AstDecl, as.func_decl));
}

static void graph_build(WaitGraph* graph, CompilerContext* ctx, const AstModule* merged) {
  memset(graph, 0, sizeof(*graph));
  graph->ctx = ctx;
  graph->merged = merged;
  /* Every specialization first, so a call can find its own */
  for (size_t i = 0; i < ctx->specialized_func_count; i++) {
    const FunctionSpecialization* spec = &ctx->specialized_funcs[i];
    if (!spec->decl || !spec->decl->generic_params) continue;
    char key[512];
    wait_graph_specialization_key(spec->concrete_args, key, sizeof key);
    const AstDecl* decl = decl_of_func(spec->decl);
    if (wait_graph_node(graph, decl, key, NULL, false) < 0) wait_graph_node(graph, decl, key, spec->concrete_args, true);
  }
  for (const AstDecl* decl = merged->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC && !decl->as.func_decl.generic_params) wait_graph_node(graph, decl, NULL, NULL, true);
  }
  if (ctx->type_registry) {
    for (SpecializationEntry* spec = ctx->type_registry->specializations; spec; spec = spec->next) {
      if (spec->specialized_decl && spec->specialized_decl->kind == AST_DECL_FUNC) {
        wait_graph_node(graph, spec->specialized_decl, NULL, NULL, true);
      }
    }
  }
  wait_graph_walk_all(graph);
  graph_solve(graph);
}

static void graph_release(WaitGraph* graph) {
  for (size_t i = 0; i < graph->count; i++) {
    free(graph->nodes[i].effect_callees);
    free(graph->nodes[i].callees);
    free(graph->nodes[i].key);
  }
  free(graph->nodes);
  free(graph->table);
}

WaitGraph* may_wait_build(CompilerContext* ctx, const AstModule* merged) {
  WaitGraph* graph = malloc(sizeof(WaitGraph));
  graph_build(graph, ctx, merged);
  return graph;
}

void may_wait_free(WaitGraph* graph) {
  if (!graph) return;
  graph_release(graph);
  free(graph);
}

/* An extern the scheduler's workers may call: the runtime's own (lib/core,
 * Math, Parallel, Time, Channel, String; and the file modules Sys, Io, Files,
 * Filesystem, Image, whose calls that can wait are `blocking`), which never
 * blocks for long, and lib/net's
 * system calls, which are non-blocking (a socket answers `wouldBlock`) —
 * minus the waits: sleeps, socket waits and `blocking` externs (the name
 * lookup among them) suspend a task (S3b, S4, S5) */
static bool extern_is_pool_safe(const WaitGraph* graph, const AstDecl* decl) {
  const AstFuncDecl* func = &decl->as.func_decl;
  if (wait_extern_kind(func) != WAIT_NONE) return false;
  if (!wait_graph_is_stdlib(graph, decl)) return false;
  const char* path = short_lib_path(decl->origin_file);
  static const char* const safe[] = { "lib/core/", "lib/Math.rae", "lib/Parallel.rae", "lib/Time.rae",
                                      "lib/Channel.rae", "lib/String.rae", "lib/net/NetSystem.rae", "lib/net/ByteBuffer.rae",
                                      "lib/Sys.rae", "lib/Io.rae", "lib/Files.rae", "lib/Filesystem.rae",
                                      "lib/Image.rae" };
  for (size_t i = 0; i < sizeof safe / sizeof safe[0]; i++) {
    if (strncmp(path, safe[i], strlen(safe[i])) == 0) return true;
  }
  return false;
}

/* Everything `start` can reach, once: whether any node `bad` says yes to */
static bool reach_any(WaitGraph* graph, int start, bool (*bad)(WaitGraph*, const WaitNode*, bool), bool flag) {
  bool* seen = calloc(graph->count, sizeof(bool));
  int* stack = malloc((graph->count + 1) * sizeof(int));
  size_t depth = 0;
  bool found = false;
  stack[depth++] = start;
  seen[start] = true;
  while (depth > 0 && !found) {
    const WaitNode* node = &graph->nodes[stack[--depth]];
    if (bad(graph, node, flag)) found = true;
    for (size_t c = 0; c < node->callee_count && !found; c++) {
      int callee = node->callees[c];
      if (!seen[callee]) {
        seen[callee] = true;
        stack[depth++] = callee;
      }
    }
  }
  free(seen);
  free(stack);
  return found;
}

/* A wait that holds its thread unless the task suspends there: a sleep or a
 * socket / poller wait */
static bool node_sleeps(WaitGraph* graph, const WaitNode* node, bool flag) {
  (void)graph; (void)flag;
  return node->direct == WAIT_SLEEP || node->direct == WAIT_SOCKET || node->direct == WAIT_BLOCKING;
}

/* A node a pool task must not reach: C that may block, and a sleep or socket
 * wait unless it can suspend (`allow_sleep`) */
static bool node_off_pool(WaitGraph* graph, const WaitNode* node, bool allow_sleep) {
  if ((node->direct == WAIT_SLEEP || node->direct == WAIT_SOCKET || node->direct == WAIT_BLOCKING) && !allow_sleep) {
    return true;
  }
  const AstFuncDecl* func = &node->decl->as.func_decl;
  if (func->is_extern) {
    if (wait_extern_kind(func) != WAIT_NONE) return !allow_sleep;
    return !extern_is_pool_safe(graph, node->decl);
  }
  return false;
}

static int decl_node(WaitGraph* graph, const AstDecl* decl) {
  if (!graph || !decl || decl->kind != AST_DECL_FUNC || decl->as.func_decl.generic_params) return -1;
  return wait_graph_node(graph, decl, NULL, NULL, false);
}

bool may_wait_reaches_sleep(WaitGraph* graph, const AstDecl* decl) {
  int start = decl_node(graph, decl);
  return start >= 0 && reach_any(graph, start, node_sleeps, false);
}

static bool node_waits_on_socket(WaitGraph* graph, const WaitNode* node, bool flag) {
  (void)graph; (void)flag;
  return node->direct == WAIT_SOCKET;
}

bool may_wait_reaches_socket(WaitGraph* graph, const AstDecl* decl) {
  int start = decl_node(graph, decl);
  return start >= 0 && reach_any(graph, start, node_waits_on_socket, false);
}

bool may_wait_decl_may_wait(WaitGraph* graph, const AstDecl* decl) {
  int start = decl_node(graph, decl);
  return start >= 0 && graph->nodes[start].may_wait;
}

bool may_wait_twin_candidate(WaitGraph* graph, const AstDecl* decl) {
  int start = decl_node(graph, decl);
  return start >= 0 && graph->nodes[start].may_wait && graph->nodes[start].reachable;
}

bool may_wait_spawn_on_pool(WaitGraph* graph, const AstDecl* decl, bool allow_sleep) {
  int start = decl_node(graph, decl);
  if (start < 0) return false;
  if (allow_sleep) return !reach_any(graph, start, node_off_pool, true);
  if (graph->nodes[start].pool_safe >= 0) return graph->nodes[start].pool_safe == 1;
  /* Everything the spawned function can reach, once */
  bool* seen = calloc(graph->count, sizeof(bool));
  int* stack = malloc((graph->count + 1) * sizeof(int));
  size_t depth = 0;
  bool safe = true;
  stack[depth++] = start;
  seen[start] = true;
  while (depth > 0 && safe) {
    const WaitNode* node = &graph->nodes[stack[--depth]];
    if (node->direct == WAIT_SLEEP || node->direct == WAIT_SOCKET || node->direct == WAIT_BLOCKING) safe = false;
    if (node->decl->as.func_decl.is_extern && !extern_is_pool_safe(graph, node->decl)) safe = false;
    for (size_t c = 0; c < node->callee_count && safe; c++) {
      int callee = node->callees[c];
      if (!seen[callee]) {
        seen[callee] = true;
        stack[depth++] = callee;
      }
    }
  }
  free(seen);
  free(stack);
  graph->nodes[start].pool_safe = safe ? 1 : 0;
  return safe;
}

/* ----- shared with may_wait_report.c and realtime_check.c ---------------- */


void wait_graph_build(WaitGraph* graph, CompilerContext* ctx, const AstModule* merged) {
  graph_build(graph, ctx, merged);
}

void wait_graph_release(WaitGraph* graph) {
  graph_release(graph);
}

const char* wait_graph_short_lib_path(const char* path) {
  return short_lib_path(path);
}

const char* wait_graph_file_name(const char* path) {
  return file_name(path);
}
