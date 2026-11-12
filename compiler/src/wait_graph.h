/* The may-wait graph's types, shared by may_wait.c (the walk and the waits),
 * may_wait_report.c (--report-waits) and realtime_check.c (the realtime and
 * noAlloc checks, docs/realtime-language-design.md §2-3). Internal to the
 * compiler: nothing outside these files includes it. */
#ifndef RAE_WAIT_GRAPH_H
#define RAE_WAIT_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "ast.h"

typedef enum {
  WAIT_NONE = 0,
  WAIT_TASK_GET,
  WAIT_SLEEP,
  WAIT_SOCKET,
  WAIT_JOIN,
  WAIT_BLOCKING,
  WAIT_KIND_COUNT
} WaitKind;

static inline const char* wait_kind_name(WaitKind kind) {
  switch (kind) {
    case WAIT_TASK_GET: return "Task.get";
    case WAIT_SLEEP: return "sleep extern";
    case WAIT_SOCKET: return "socket/poller extern";
    case WAIT_JOIN: return "task join";
    case WAIT_BLOCKING: return "blocking extern";
    default: return "none";
  }
}

typedef struct {
  const AstDecl* decl;
  /* A specialization: the template's concrete arguments (NULL otherwise),
   * and their key ("BenchRouter", "List(Int)") */
  const AstTypeRef* concrete_args;
  char* key;
  int next_same_decl;   /* the next node of the same decl, -1 at the end */
  int* callees;
  size_t callee_count;
  size_t callee_cap;
  WaitKind direct;      /* the first primitive used directly, or WAIT_NONE */
  bool may_wait;
  int via;              /* the callee that made it may-wait; -1 when direct */
  bool spawned;         /* the target of some `spawn` */
  bool reachable;       /* reachable from a spawn */
  bool walked;
  signed char pool_safe; /* may_wait_spawn_on_pool's answer: -1 not asked, 0, 1 */
  /* The realtime effects (realtime_check.c): what this function itself does
   * (`effects`, a set of EFFECT_* bits), where it first does each (a line,
   * a column and what it is), and after the fixed point, what it or anything
   * it reaches does (`reached`) and through which callee (`effect_via`, -1:
   * itself). */
  unsigned char effects;
  unsigned char reached;
  /* Calls only the realtime check follows: method calls sema leaves to the
   * backend (a generic container's `list.add(...)`), resolved here by name.
   * The may-wait analysis keeps to `callees`, so its answers do not move. */
  int* effect_callees;
  size_t effect_callee_count;
  size_t effect_callee_cap;
  int effect_line[5];
  int effect_column[5];
  const char* effect_what[5];
  int effect_via[5];
  /* Runs off the main thread (docs/realtime-language-design.md §4.3): called
   * in a parallelLoop body (`parallel_called`; `spawned` above is the other
   * way), and where it was first spawned or called so */
  bool parallel_called;
  const char* off_main_file;
  int off_main_line;
  int off_main_column;
} WaitNode;

/* The effects real-time code may not have (docs/realtime-language-design.md §2) */
enum { EFFECT_ALLOCATES = 1, EFFECT_FREES = 2, EFFECT_LOCKS = 4, EFFECT_WAITS = 8, EFFECT_ALL = 15 };
/* Not a real-time effect: it reaches a call that only the main thread may
 * make (an extern or function marked `mainThread`, §4.3); bit 4 of the arrays */
enum { EFFECT_MAIN_THREAD = 16, EFFECT_MAIN_THREAD_BIT = 4 };

typedef struct {
  const AstDecl* decl;
  int first;            /* first node of this decl */
} DeclSlot;

typedef struct WaitGraph {
  CompilerContext* ctx;
  const AstModule* merged;
  WaitNode* nodes;
  size_t count;
  size_t cap;
  DeclSlot* table;      /* decl -> its first node, open addressing */
  size_t table_cap;
  /* [0] in the program, [1] in lib/ */
  size_t primitive_sites[2][WAIT_KIND_COUNT];
  size_t spawn_sites[2];
  size_t approximate_calls[2];
} WaitGraph;


/* Shared helpers (may_wait.c) */
bool wait_graph_is_stdlib(const WaitGraph* graph, const AstDecl* decl);
void wait_graph_build(WaitGraph* graph, CompilerContext* ctx, const AstModule* merged);
void wait_graph_release(WaitGraph* graph);
const char* wait_graph_short_lib_path(const char* path);
const char* wait_graph_file_name(const char* path);

#endif
