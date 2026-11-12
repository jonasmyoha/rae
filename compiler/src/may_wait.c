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

static DeclSlot* decl_slot(WaitGraph* graph, const AstDecl* decl, bool create) {
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
static int graph_node(WaitGraph* graph, const AstDecl* decl, const char* key, const AstTypeRef* concrete_args,
                      bool create) {
  DeclSlot* slot = decl_slot(graph, decl, create);
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

static void node_add_callee(WaitNode* node, int callee) {
  for (size_t i = 0; i < node->callee_count; i++) {
    if (node->callees[i] == callee) return;
  }
  if (node->callee_count == node->callee_cap) {
    node->callee_cap = node->callee_cap ? node->callee_cap * 2 : 8;
    node->callees = realloc(node->callees, node->callee_cap * sizeof(int));
  }
  node->callees[node->callee_count++] = callee;
}

static bool type_is_task(const TypeInfo* type) {
  while (type && type->kind == TYPE_REF) type = type->as.ref.base;
  return type && type->kind == TYPE_TASK;
}

/* The primitive an extern is, by its C symbol */
static WaitKind extern_wait_kind(const AstFuncDecl* func) {
  if (func->is_extern && func->is_blocking) return WAIT_BLOCKING;
  if (!func->is_extern || !func->extern_symbol) return WAIT_NONE;
  const char* symbol = func->extern_symbol;
  if (strcmp(symbol, "rae_ext_rae_sleep") == 0 || strcmp(symbol, "rae_ext_Time_sleepNs") == 0) return WAIT_SLEEP;
  if (strcmp(symbol, "rae_ext_NetSys_pollOne") == 0 || strcmp(symbol, "rae_ext_NetSys_pollerWait") == 0) return WAIT_SOCKET;
  return WAIT_NONE;
}

/* ----- type keys under a specialization's bindings ---------------------- */

typedef struct {
  const AstIdentifierPart* params;   /* the template's type parameters */
  const AstTypeRef* args;            /* bound to these */
} Bindings;

typedef struct {
  char text[512];
  size_t length;
} KeyBuffer;

static void key_append(KeyBuffer* key, const char* text, size_t length) {
  if (key->length + length + 1 >= sizeof(key->text)) length = sizeof(key->text) - key->length - 1;
  memcpy(key->text + key->length, text, length);
  key->length += length;
  key->text[key->length] = '\0';
}

static void key_of_type(KeyBuffer* key, const AstTypeRef* type, const Bindings* bindings);

/* The concrete argument bound to `name`, or NULL */
static const AstTypeRef* binding_of(const Bindings* bindings, Str name) {
  if (!bindings) return NULL;
  const AstTypeRef* arg = bindings->args;
  for (const AstIdentifierPart* param = bindings->params; param && arg; param = param->next, arg = arg->next) {
    if (str_eq(param->text, name)) return arg;
  }
  return NULL;
}

static void key_of_type(KeyBuffer* key, const AstTypeRef* type, const Bindings* bindings) {
  if (!type) return;
  if (!type->parts && type->resolved_type && type->resolved_type->name.data) {
    key_append(key, type->resolved_type->name.data, type->resolved_type->name.len);
    return;
  }
  if (!type->parts) return;
  if (!type->parts->next && !type->generic_args) {
    const AstTypeRef* bound = binding_of(bindings, type->parts->text);
    if (bound) {
      key_of_type(key, bound, NULL);
      return;
    }
  }
  for (const AstIdentifierPart* part = type->parts; part; part = part->next) {
    key_append(key, part->text.data, part->text.len);
    if (part->next) key_append(key, ".", 1);
  }
  if (type->generic_args) {
    key_append(key, "(", 1);
    for (const AstTypeRef* arg = type->generic_args; arg; arg = arg->next) {
      key_of_type(key, arg, bindings);
      if (arg->next) key_append(key, ",", 1);
    }
    key_append(key, ")", 1);
  }
}

/* The same key from a sema type: TypeInfo names generic instances by their
 * mangled spelling (`ComponentTable_Socket`), so rebuild `Base(Arg)` */
static void key_of_typeinfo(KeyBuffer* key, const TypeInfo* type) {
  while (type && type->kind == TYPE_REF) type = type->as.ref.base;
  if (!type) return;
  if (type->kind == TYPE_STRUCT && type->as.structure.decl && type->as.structure.generic_count > 0) {
    const AstDecl* decl = type->as.structure.decl;
    if (decl->kind == AST_DECL_TYPE && decl->as.type_decl.generic_template) decl = decl->as.type_decl.generic_template;
    Str name = decl->kind == AST_DECL_TYPE ? decl->as.type_decl.name : type->name;
    key_append(key, name.data, name.len);
    key_append(key, "(", 1);
    for (size_t i = 0; i < type->as.structure.generic_count; i++) {
      if (i) key_append(key, ",", 1);
      key_of_typeinfo(key, type->as.structure.generic_args[i]);
    }
    key_append(key, ")", 1);
    return;
  }
  if (type->name.data) key_append(key, type->name.data, type->name.len);
}

/* ----- the walk over one body ------------------------------------------- */

typedef struct {
  WaitGraph* graph;
  int node;
  int scope;                 /* 0: the program, 1: lib/ */
  const AstFuncDecl* func;   /* the function being walked (its params) */
  Bindings bindings;         /* its type parameters, for a specialization */
  bool cold;                 /* inside a misuse report's arguments: no effects */
  int parallel_depth;        /* inside a parallelLoop body: its calls run on workers */
  size_t parallel_line;      /* the outermost such loop's line and column */
  size_t parallel_column;
} WalkContext;

/* A realtime effect this function itself has, at (line, column): the first
 * site of each kind is kept (realtime_check.c names it in its error) */
static void note_effect(WalkContext* walk, int effects, size_t line, size_t column, const char* what) {
  if (walk->cold) return;
  WaitNode* node = &walk->graph->nodes[walk->node];
  for (int bit = 0; bit < 5; bit++) {
    if (!(effects & (1 << bit)) || (node->effects & (1 << bit))) continue;
    node->effects |= (unsigned char)(1 << bit);
    node->effect_line[bit] = (int)line;
    node->effect_column[bit] = (int)column;
    node->effect_what[bit] = what;
  }
}

/* lib/core's misuse reports (an index out of range): the crash path of
 * docs/realtime-language-design.md §3.6, where real-time rules do not apply,
 * as the program is already wrong there. Calls to them add no edge and no
 * effect. */
static bool is_misuse_report(const WaitGraph* graph, const AstDecl* target) {
  if (!target || target->kind != AST_DECL_FUNC || !target->origin_file) return false;
  Str name = target->as.func_decl.name;
  if (!str_eq_cstr(name, "runtimeWarning") && !str_eq_cstr(name, "indexOutOfRangeWarning")) return false;
  (void)graph;
  return strstr(target->origin_file, "lib/core/") != NULL;
}

static void note_wait(WalkContext* walk, WaitKind kind) {
  walk->graph->primitive_sites[walk->scope][kind]++;
  if (walk->cold) return;
  WaitNode* node = &walk->graph->nodes[walk->node];
  if (node->direct == WAIT_NONE) node->direct = kind;
}

static void add_edge(WalkContext* walk, int callee) {
  if (callee < 0) return;
  node_add_callee(&walk->graph->nodes[walk->node], callee);
  if (walk->parallel_depth > 0) {
    /* a call in a parallelLoop body runs on a worker (§4.3) */
    WaitNode* target = &walk->graph->nodes[callee];
    if (!target->parallel_called && !target->spawned) {
      target->off_main_file = walk->func ? walk->func->origin_file : NULL;
      target->off_main_line = (int)walk->parallel_line;
      target->off_main_column = (int)walk->parallel_column;
    }
    target->parallel_called = true;
  }
}

/* Every specialization of a generic template: the conservative edge */
static void edge_to_all_specializations(WalkContext* walk, const AstDecl* template_decl) {
  DeclSlot* slot = decl_slot(walk->graph, template_decl, false);
  if (!slot) return;
  for (int index = slot->first; index >= 0; index = walk->graph->nodes[index].next_same_decl) add_edge(walk, index);
}

/* The concrete type name of an argument expression, or NULL */
static bool concrete_type_of(WalkContext* walk, const AstExpr* value, KeyBuffer* key) {
  if (!value) return false;
  if (value->kind == AST_EXPR_IDENT && walk->func) {
    for (const AstParam* param = walk->func->params; param; param = param->next) {
      if (str_eq(param->name, value->as.ident) && param->type) {
        key_of_type(key, param->type, &walk->bindings);
        return key->length > 0;
      }
    }
  }
  const TypeInfo* type = value->resolved_type;
  while (type && type->kind == TYPE_REF) type = type->as.ref.base;
  if (type && type->kind != TYPE_GENERIC_PARAM) {
    key_of_typeinfo(key, type);
    return key->length > 0;
  }
  return false;
}

/* Infer type parameter `param` of a generic call from its named arguments:
 * an argument whose parameter type is `param` itself, or `X(param)` one level
 * deep (`this: ComponentTable(T)`). Appends its key; false when no argument
 * decides it. */
static bool infer_type_argument(WalkContext* walk, const AstFuncDecl* func, Str param, const AstCallArg* args,
                                const AstExpr* receiver, KeyBuffer* key) {
  size_t position_of_formal = 0;
  for (const AstParam* formal = func->params; formal; formal = formal->next, position_of_formal++) {
    const AstTypeRef* type = formal->type;
    if (!type || !type->parts || type->parts->next) continue;
    const AstExpr* value = NULL;
    if (receiver && formal == func->params) value = receiver;
    for (const AstCallArg* arg = args; arg && !value; arg = arg->next) {
      if (arg->name.len && str_eq(arg->name, formal->name)) value = arg->value;
    }
    /* ... or the argument written in its position, unnamed */
    size_t written_position = receiver ? 1 : 0;
    for (const AstCallArg* arg = args; arg && !value; arg = arg->next, written_position++) {
      if (written_position == position_of_formal && !arg->name.len) value = arg->value;
    }
    if (!value) continue;
    if (!type->generic_args && str_eq(type->parts->text, param)) {
      KeyBuffer found = { { 0 }, 0 };
      if (concrete_type_of(walk, value, &found)) {
        key_append(key, found.text, found.length);
        return true;
      }
      continue;
    }
    size_t position = 0;
    for (const AstTypeRef* inner = type->generic_args; inner; inner = inner->next, position++) {
      if (!inner->parts || inner->parts->next || inner->generic_args || !str_eq(inner->parts->text, param)) continue;
      /* An enclosing parameter passed on (`grow(this)` in a generic body):
       * its declared type, with this specialization's bindings */
      if (value->kind == AST_EXPR_IDENT && walk->func) {
        for (const AstParam* own = walk->func->params; own; own = own->next) {
          if (!str_eq(own->name, value->as.ident) || !own->type) continue;
          const AstTypeRef* own_inner = own->type->generic_args;
          for (size_t skip = 0; own_inner && skip < position; skip++) own_inner = own_inner->next;
          if (own_inner) {
            KeyBuffer found = { { 0 }, 0 };
            key_of_type(&found, own_inner, &walk->bindings);
            if (found.length && !(own_inner->parts && !own_inner->parts->next && !binding_of(&walk->bindings, own_inner->parts->text) &&
                                  !own_inner->generic_args && own_inner->parts->text.len == 1)) {
              key_append(key, found.text, found.length);
              return true;
            }
          }
        }
      }
      const TypeInfo* actual_type = value->resolved_type;
      while (actual_type && actual_type->kind == TYPE_REF) actual_type = actual_type->as.ref.base;
      if (actual_type && actual_type->kind == TYPE_STRUCT && position < actual_type->as.structure.generic_count) {
        const TypeInfo* bound = actual_type->as.structure.generic_args[position];
        if (bound && bound->kind != TYPE_GENERIC_PARAM) {
          key_of_typeinfo(key, bound);
          return key->length > 0;
        }
      }
    }
  }
  return false;
}

/* An edge to `target`, a function decl, from a call whose leading unnamed
 * arguments may be its type arguments */
static void edge_to_decl(WalkContext* walk, const AstDecl* target, const AstCallArg* args, const AstTypeRef* generic_args,
                         const AstExpr* receiver) {
  if (!target || target->kind != AST_DECL_FUNC) return;
  const AstFuncDecl* func = &target->as.func_decl;
  WaitKind primitive = extern_wait_kind(func);
  if (primitive != WAIT_NONE) walk->graph->primitive_sites[walk->scope][primitive]++;
  if (!func->generic_params) {
    add_edge(walk, graph_node(walk->graph, target, NULL, NULL, true));
    return;
  }
  /* A generic extern (the buffer intrinsics) has no body and no
   * specializations; one that does not wait cannot change the result */
  if (func->is_extern) return;
  /* A generic template: find the specialization from the type arguments */
  KeyBuffer key = { { 0 }, 0 };
  size_t wanted = 0, found = 0;
  for (const AstIdentifierPart* param = func->generic_params; param; param = param->next) wanted++;
  if (generic_args) {
    for (const AstTypeRef* arg = generic_args; arg && found < wanted; arg = arg->next, found++) {
      if (found) key_append(&key, ",", 1);
      key_of_type(&key, arg, &walk->bindings);
    }
  } else {
    for (const AstCallArg* arg = args; arg && found < wanted; arg = arg->next) {
      if (arg->name.len || !arg->value || arg->value->kind != AST_EXPR_IDENT) break;
      /* A written type argument: a bound type parameter, or a type name
       * (PascalCase, where every value is camelCase) */
      const AstTypeRef* bound = binding_of(&walk->bindings, arg->value->as.ident);
      Str text = arg->value->as.ident;
      if (!bound && !(text.len && text.data[0] >= 'A' && text.data[0] <= 'Z')) break;
      if (found) key_append(&key, ",", 1);
      if (bound) {
        key_of_type(&key, bound, NULL);
      } else {
        key_append(&key, arg->value->as.ident.data, arg->value->as.ident.len);
      }
      found++;
    }
    if (found == 0) {
      /* No written type arguments: infer them from the named arguments */
      for (const AstIdentifierPart* param = func->generic_params; param; param = param->next) {
        if (found) key_append(&key, ",", 1);
        if (!infer_type_argument(walk, func, param->text, args, receiver, &key)) break;
        found++;
      }
    }
  }
  int index = found == wanted ? graph_node(walk->graph, target, key.text, NULL, false) : -1;
  if (index >= 0) {
    add_edge(walk, index);
  } else {
    walk->graph->approximate_calls[walk->scope]++;
    if (getenv("RAE_REPORT_WAITS_DEBUG")) fprintf(stderr, "approx generic %.*s key=[%s] found=%zu/%zu args0=%.*s\n", (int)func->name.len, func->name.data, key.text, found, wanted, args && args->name.len ? (int)args->name.len : 0, args && args->name.len ? args->name.data : "");
    edge_to_all_specializations(walk, target);
  }
}

/* A call sema left unresolved (it depends on a type parameter): resolve it
 * by name. One candidate is exact. Among several, the non-generic ones are
 * told apart by the concrete type of the first argument (the receiver of a
 * method call); a generic candidate infers its own type arguments. */
static void edge_by_name(WalkContext* walk, Str name, const AstCallArg* args, const AstTypeRef* generic_args,
                         const AstExpr* receiver) {
  const AstDecl* candidates[64];
  size_t candidate_count = 0;
  for (const AstDecl* decl = walk->graph->merged->decls; decl; decl = decl->next) {
    if (decl->kind != AST_DECL_FUNC || !str_eq(decl->as.func_decl.name, name)) continue;
    if (candidate_count < 64) candidates[candidate_count++] = decl;
  }
  if (candidate_count == 0) return;
  if (candidate_count == 1) {
    edge_to_decl(walk, candidates[0], args, generic_args, receiver);
    return;
  }
  const AstExpr* first_argument = receiver ? receiver : (args ? args->value : NULL);
  KeyBuffer wanted = { { 0 }, 0 };
  bool typed = concrete_type_of(walk, first_argument, &wanted);
  size_t matched = 0;
  if (typed) {
    for (size_t i = 0; i < candidate_count; i++) {
      const AstFuncDecl* func = &candidates[i]->as.func_decl;
      if (func->generic_params || !func->params) continue;
      KeyBuffer have = { { 0 }, 0 };
      key_of_type(&have, func->params->type, NULL);
      if (strcmp(have.text, wanted.text) == 0) {
        edge_to_decl(walk, candidates[i], args, generic_args, receiver);
        matched++;
      }
    }
    if (matched) return;
  }
  bool any_body = false;
  for (size_t i = 0; i < candidate_count; i++) {
    if (!candidates[i]->as.func_decl.is_extern || extern_wait_kind(&candidates[i]->as.func_decl) != WAIT_NONE) any_body = true;
  }
  if (!any_body) return;
  walk->graph->approximate_calls[walk->scope]++;
  if (getenv("RAE_REPORT_WAITS_DEBUG")) fprintf(stderr, "approx byname %.*s wanted=[%s] candidates=%zu\n", (int)name.len, name.data, wanted.text, candidate_count);
  for (size_t i = 0; i < candidate_count; i++) edge_to_decl(walk, candidates[i], args, generic_args, receiver);
}

static void walk_block(WalkContext* walk, const AstBlock* block);
static void walk_stmt(WalkContext* walk, const AstStmt* stmt);

/* A method call sema left to the backend, outside a generic body: its
 * targets by name, as effect-only edges (wait_graph.h). The may-wait edges
 * and counters are put back as they were. */
static void effect_edges_by_name(WalkContext* walk, const AstExpr* expr) {
  WaitGraph* graph = walk->graph;
  size_t saved_sites[2][WAIT_KIND_COUNT];
  size_t saved_approximate[2] = { graph->approximate_calls[0], graph->approximate_calls[1] };
  memcpy(saved_sites, graph->primitive_sites, sizeof saved_sites);
  WaitNode* holder = &graph->nodes[walk->node];
  int* saved = holder->callees;
  size_t saved_count = holder->callee_count, saved_cap = holder->callee_cap;
  holder->callees = holder->effect_callees;
  holder->callee_count = holder->effect_callee_count;
  holder->callee_cap = holder->effect_callee_cap;
  edge_by_name(walk, expr->as.method_call.method_name, expr->as.method_call.args, expr->as.method_call.generic_args,
               expr->as.method_call.object);
  holder = &graph->nodes[walk->node];
  holder->effect_callees = holder->callees;
  holder->effect_callee_count = holder->callee_count;
  holder->effect_callee_cap = holder->callee_cap;
  holder->callees = saved;
  holder->callee_count = saved_count;
  holder->callee_cap = saved_cap;
  memcpy(graph->primitive_sites, saved_sites, sizeof saved_sites);
  graph->approximate_calls[0] = saved_approximate[0];
  graph->approximate_calls[1] = saved_approximate[1];
}

static void walk_expr(WalkContext* walk, const AstExpr* expr) {
  if (!expr) return;
  switch (expr->kind) {
    case AST_EXPR_BINARY:
      walk_expr(walk, expr->as.binary.lhs);
      walk_expr(walk, expr->as.binary.rhs);
      break;
    case AST_EXPR_UNARY:
      if (expr->as.unary.op == AST_UNARY_SPAWN) {
        const AstExpr* call = expr->as.unary.operand;
        walk->graph->spawn_sites[walk->scope]++;
        note_effect(walk, EFFECT_ALLOCATES | EFFECT_WAITS, expr->line, expr->column, "a spawn (it starts a task)");
        if (call && call->kind == AST_EXPR_CALL) {
          /* The call runs in the task: its target is marked spawned and is
           * not an edge of this function. Its arguments are evaluated here. */
          WaitNode* holder = &walk->graph->nodes[walk->node];
          int* saved = holder->callees;
          size_t saved_count = holder->callee_count, saved_cap = holder->callee_cap;
          holder->callees = NULL;
          holder->callee_count = 0;
          holder->callee_cap = 0;
          const AstDecl* target = call->decl_link ? call->decl_link : (call->as.call.callee ? call->as.call.callee->decl_link : NULL);
          edge_to_decl(walk, target, call->as.call.args, call->as.call.generic_args, NULL);
          holder = &walk->graph->nodes[walk->node];
          for (size_t i = 0; i < holder->callee_count; i++) {
            WaitNode* spawned = &walk->graph->nodes[holder->callees[i]];
            if (!spawned->spawned && !spawned->parallel_called) {
              spawned->off_main_file = walk->func ? walk->func->origin_file : NULL;
              spawned->off_main_line = (int)expr->line;
              spawned->off_main_column = (int)expr->column;
            }
            spawned->spawned = true;
          }
          free(holder->callees);
          holder->callees = saved;
          holder->callee_count = saved_count;
          holder->callee_cap = saved_cap;
          for (const AstCallArg* arg = call->as.call.args; arg; arg = arg->next) walk_expr(walk, arg->value);
        }
        break;
      }
      walk_expr(walk, expr->as.unary.operand);
      break;
    /* `own x`, a box and an unbox evaluate their operand (an owned `if let`
     * wraps its value in one) */
    case AST_EXPR_OWN:
    case AST_EXPR_BOX:
    case AST_EXPR_UNBOX:
      walk_expr(walk, expr->as.unary.operand);
      break;
    case AST_EXPR_CAST:
      walk_expr(walk, expr->as.cast.operand);
      break;
    case AST_EXPR_CALL: {
      const AstDecl* target = expr->decl_link ? expr->decl_link : (expr->as.call.callee ? expr->as.call.callee->decl_link : NULL);
      if (is_misuse_report(walk->graph, target)) {
        bool was_cold = walk->cold;
        walk->cold = true;
        for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) walk_expr(walk, arg->value);
        walk->cold = was_cold;
        break;
      }
      if (target) {
        edge_to_decl(walk, target, expr->as.call.args, expr->as.call.generic_args, NULL);
      } else if (expr->as.call.callee && expr->as.call.callee->kind == AST_EXPR_IDENT) {
        edge_by_name(walk, expr->as.call.callee->as.ident, expr->as.call.args, expr->as.call.generic_args, NULL);
      }
      for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) walk_expr(walk, arg->value);
      break;
    }
    case AST_EXPR_METHOD_CALL:
      if (expr->as.method_call.object && type_is_task(expr->as.method_call.object->resolved_type) &&
          expr->as.method_call.method_name.len == 3 && memcmp(expr->as.method_call.method_name.data, "get", 3) == 0) {
        note_wait(walk, WAIT_TASK_GET);
        note_effect(walk, EFFECT_WAITS, expr->line, expr->column, "Task.get (it waits for the task)");
      } else if (expr->decl_link) {
        edge_to_decl(walk, expr->decl_link, expr->as.method_call.args, expr->as.method_call.generic_args,
                     expr->as.method_call.object);
      } else if (walk->bindings.params) {
        edge_by_name(walk, expr->as.method_call.method_name, expr->as.method_call.args, expr->as.method_call.generic_args,
                     expr->as.method_call.object);
      } else {
        effect_edges_by_name(walk, expr);
      }
      walk_expr(walk, expr->as.method_call.object);
      for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) walk_expr(walk, arg->value);
      break;
    case AST_EXPR_MEMBER:
      walk_expr(walk, expr->as.member.object);
      break;
    case AST_EXPR_OBJECT:
      for (const AstObjectField* field = expr->as.object_literal.fields; field; field = field->next) walk_expr(walk, field->value);
      break;
    case AST_EXPR_MATCH:
      walk_expr(walk, expr->as.match_expr.subject);
      for (const AstMatchArm* arm = expr->as.match_expr.arms; arm; arm = arm->next) {
        walk_expr(walk, arm->pattern);
        walk_expr(walk, arm->value);
      }
      break;
    case AST_EXPR_LIST:
      note_effect(walk, EFFECT_ALLOCATES, expr->line, expr->column, "a list literal");
      for (const AstExprList* item = expr->as.list; item; item = item->next) walk_expr(walk, item->value);
      break;
    case AST_EXPR_INDEX:
      walk_expr(walk, expr->as.index.target);
      walk_expr(walk, expr->as.index.index);
      break;
    case AST_EXPR_COLLECTION_LITERAL:
      note_effect(walk, EFFECT_ALLOCATES, expr->line, expr->column, "a collection literal");
      for (const AstCollectionElement* element = expr->as.collection.elements; element; element = element->next) walk_expr(walk, element->value);
      break;
    case AST_EXPR_INTERP:
      note_effect(walk, EFFECT_ALLOCATES | EFFECT_FREES, expr->line, expr->column, "a String interpolation (made, and freed after its use)");
      for (const AstInterpPart* part = expr->as.interp.parts; part; part = part->next) walk_expr(walk, part->value);
      break;
    default:
      break;
  }
}

/* A `let` / `var` of a type that owns heap: it is freed at the end of its
 * scope, and when it is set from a place (not a call or an `own`), that is a
 * deep copy. An alias (view / mod) owns nothing. */
static void note_let_effects(WalkContext* walk, const AstStmt* stmt) {
  const AstTypeRef* type = stmt->as.let_stmt.type;
  if (!type || type->is_view || type->is_mod || stmt->as.let_stmt.is_bind) return;
  const AstTypeRef* concrete = type;
  if (walk->bindings.params && walk->bindings.args) {
    concrete = substitute_type_ref(walk->graph->ctx, walk->bindings.params, walk->bindings.args, type);
  }
  if (!type_needs_cascade_drop(walk->graph->ctx, walk->graph->merged, concrete, 0)) return;
  note_effect(walk, EFFECT_FREES, stmt->line, stmt->column, "a local that owns heap, freed at the end of its scope");
  const AstExpr* value = stmt->as.let_stmt.value;
  if (value && (value->kind == AST_EXPR_IDENT || value->kind == AST_EXPR_MEMBER)) {
    note_effect(walk, EFFECT_ALLOCATES, stmt->line, stmt->column,
                "a deep copy of a place (bind a view instead: `let x: view T = place`)");
  }
}

static void walk_stmt(WalkContext* walk, const AstStmt* stmt) {
  switch (stmt->kind) {
    case AST_STMT_LET: {
      const AstExpr* value = stmt->as.let_stmt.value;
      walk_expr(walk, value);
      /* A Task held in a local is joined when it goes out of scope */
      if (value && (type_is_task(value->resolved_type) ||
                    (value->kind == AST_EXPR_UNARY && value->as.unary.op == AST_UNARY_SPAWN))) {
        note_wait(walk, WAIT_JOIN);
        note_effect(walk, EFFECT_WAITS, stmt->line, stmt->column, "a Task joined at the end of its scope");
      }
      note_let_effects(walk, stmt);
      break;
    }
    case AST_STMT_DESTRUCT:
      walk_expr(walk, stmt->as.destruct_stmt.call);
      break;
    case AST_STMT_EXPR: {
      const AstExpr* value = stmt->as.expr_stmt;
      walk_expr(walk, value);
      /* A bare `spawn f()` statement is joined on drop at once */
      if (value && value->kind == AST_EXPR_UNARY && value->as.unary.op == AST_UNARY_SPAWN) note_wait(walk, WAIT_JOIN);
      break;
    }
    case AST_STMT_RET:
      for (const AstReturnArg* arg = stmt->as.ret_stmt.values; arg; arg = arg->next) walk_expr(walk, arg->value);
      break;
    case AST_STMT_IF:
      if (stmt->as.if_stmt.binding) walk_stmt(walk, stmt->as.if_stmt.binding);
      walk_expr(walk, stmt->as.if_stmt.condition);
      walk_block(walk, stmt->as.if_stmt.then_block);
      walk_block(walk, stmt->as.if_stmt.else_block);
      if (stmt->as.if_stmt.is_task_scope) note_wait(walk, WAIT_JOIN);
      break;
    case AST_STMT_LOOP:
      if (stmt->as.loop_stmt.init) walk_stmt(walk, stmt->as.loop_stmt.init);
      walk_expr(walk, stmt->as.loop_stmt.condition);
      walk_expr(walk, stmt->as.loop_stmt.increment);
      if (stmt->as.loop_stmt.is_parallel) {
        if (walk->parallel_depth++ == 0) {
          walk->parallel_line = stmt->line;
          walk->parallel_column = stmt->column;
        }
        walk_block(walk, stmt->as.loop_stmt.body);
        walk->parallel_depth--;
      } else {
        walk_block(walk, stmt->as.loop_stmt.body);
      }
      break;
    case AST_STMT_MATCH:
      if (stmt->as.match_stmt.binding) walk_stmt(walk, stmt->as.match_stmt.binding);
      walk_expr(walk, stmt->as.match_stmt.subject);
      for (const AstMatchCase* match_case = stmt->as.match_stmt.cases; match_case; match_case = match_case->next) {
        walk_expr(walk, match_case->pattern);
        walk_block(walk, match_case->block);
      }
      break;
    case AST_STMT_ASSIGN:
      walk_expr(walk, stmt->as.assign_stmt.target);
      walk_expr(walk, stmt->as.assign_stmt.value);
      break;
    case AST_STMT_DEFER:
      walk_block(walk, stmt->as.defer_stmt.block);
      break;
    case AST_STMT_UNSAFE:
      walk_block(walk, stmt->as.unsafe_stmt.block);
      break;
    default:
      break;
  }
}

static void walk_block(WalkContext* walk, const AstBlock* block) {
  if (!block) return;
  for (const AstStmt* stmt = block->first; stmt; stmt = stmt->next) walk_stmt(walk, stmt);
}

static bool is_stdlib(const WaitGraph* graph, const AstDecl* decl) {
  const char* stdlib = graph->ctx->stdlib_dir;
  if (!decl->origin_file) return false;
  if (stdlib && stdlib[0] && strncmp(decl->origin_file, stdlib, strlen(stdlib)) == 0) return true;
  return strstr(decl->origin_file, "/lib/") != NULL || strncmp(decl->origin_file, "lib/", 4) == 0;
}

/* Walk every node not walked yet (walking adds the callees it meets) */
static void graph_walk_all(WaitGraph* graph) {
  for (size_t i = 0; i < graph->count; i++) {
    if (graph->nodes[i].walked) continue;
    graph->nodes[i].walked = true;
    const AstFuncDecl* func = &graph->nodes[i].decl->as.func_decl;
    WaitKind primitive = extern_wait_kind(func);
    if (primitive != WAIT_NONE) graph->nodes[i].direct = primitive;
    for (int bit = 0; bit < 5; bit++) graph->nodes[i].effect_via[bit] = -1;
    if (func->is_extern) {
      /* C: what it promises, or all four (docs/realtime-language-design.md §3.4) */
      int effects = func->is_realtime ? 0 : func->is_no_alloc ? (EFFECT_LOCKS | EFFECT_WAITS) : EFFECT_ALL;
      WalkContext extern_walk = { graph, (int)i, 0, func, { NULL, NULL }, false, 0, 0, 0 };
      note_effect(&extern_walk, effects, graph->nodes[i].decl->line, graph->nodes[i].decl->column,
                  func->is_no_alloc ? "C marked noAlloc, which may still lock or wait" : "C not marked realtime");
    }
    if (func->is_main_thread) {
      /* a call only the main thread may make: the extern, or a Rae function
       * that promises it (§4.3) */
      WalkContext mark_walk = { graph, (int)i, 0, func, { NULL, NULL }, false, 0, 0, 0 };
      note_effect(&mark_walk, EFFECT_MAIN_THREAD, graph->nodes[i].decl->line, graph->nodes[i].decl->column,
                  func->is_extern ? "C that only the main thread may call" : "a function marked mainThread");
    }
    WalkContext walk = { graph, (int)i, is_stdlib(graph, graph->nodes[i].decl) ? 1 : 0, func, { NULL, NULL }, false, 0, 0, 0 };
    if (graph->nodes[i].key) {
      walk.bindings.params = func->generic_params;
      walk.bindings.args = graph->nodes[i].concrete_args;
    } else if (func->generic_template && func->specialization_args) {
      /* sema's own specialization: a cloned body that still spells T */
      walk.bindings.params = func->generic_template->as.func_decl.generic_params;
      walk.bindings.args = func->specialization_args;
    }
    walk_block(&walk, func->body);
  }
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
    KeyBuffer key = { { 0 }, 0 };
    for (const AstTypeRef* arg = spec->concrete_args; arg; arg = arg->next) {
      if (key.length) key_append(&key, ",", 1);
      key_of_type(&key, arg, NULL);
    }
    const AstDecl* decl = decl_of_func(spec->decl);
    if (graph_node(graph, decl, key.text, NULL, false) < 0) graph_node(graph, decl, key.text, spec->concrete_args, true);
  }
  for (const AstDecl* decl = merged->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC && !decl->as.func_decl.generic_params) graph_node(graph, decl, NULL, NULL, true);
  }
  if (ctx->type_registry) {
    for (SpecializationEntry* spec = ctx->type_registry->specializations; spec; spec = spec->next) {
      if (spec->specialized_decl && spec->specialized_decl->kind == AST_DECL_FUNC) {
        graph_node(graph, spec->specialized_decl, NULL, NULL, true);
      }
    }
  }
  graph_walk_all(graph);
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
  if (extern_wait_kind(func) != WAIT_NONE) return false;
  if (!is_stdlib(graph, decl)) return false;
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
    if (extern_wait_kind(func) != WAIT_NONE) return !allow_sleep;
    return !extern_is_pool_safe(graph, node->decl);
  }
  return false;
}

static int decl_node(WaitGraph* graph, const AstDecl* decl) {
  if (!graph || !decl || decl->kind != AST_DECL_FUNC || decl->as.func_decl.generic_params) return -1;
  return graph_node(graph, decl, NULL, NULL, false);
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

bool wait_graph_is_stdlib(const WaitGraph* graph, const AstDecl* decl) {
  return is_stdlib(graph, decl);
}

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
