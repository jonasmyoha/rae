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
 *    - the implicit joins: the end of a `taskScope { }`, and a Task local
 *      joined when it goes out of scope.
 * 2. May-wait is a fixed point over the call graph: a function may wait if
 *    it uses a primitive or calls a may-wait function. The call graph is
 *    complete (whole program, no function pointers).
 * 3. Generics are analysed AFTER specialisation: every specialisation the
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

typedef enum {
  WAIT_NONE = 0,
  WAIT_TASK_GET,
  WAIT_SLEEP,
  WAIT_SOCKET,
  WAIT_JOIN,
  WAIT_KIND_COUNT
} WaitKind;

static const char* wait_kind_name(WaitKind kind) {
  switch (kind) {
    case WAIT_TASK_GET: return "Task.get";
    case WAIT_SLEEP: return "sleep extern";
    case WAIT_SOCKET: return "socket/poller extern";
    case WAIT_JOIN: return "task join";
    default: return "none";
  }
}

typedef struct {
  const AstDecl* decl;
  /* A specialisation: the template's concrete arguments (NULL otherwise),
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
} WaitNode;

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

/* The node for (decl, key); NULL key = not a specialisation. Added on first
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
  if (!func->is_extern || !func->extern_symbol) return WAIT_NONE;
  const char* symbol = func->extern_symbol;
  if (strcmp(symbol, "rae_ext_rae_sleep") == 0 || strcmp(symbol, "rae_ext_Time_sleepNs") == 0) return WAIT_SLEEP;
  if (strcmp(symbol, "rae_ext_NetSys_pollOne") == 0 || strcmp(symbol, "rae_ext_NetSys_pollerWait") == 0) return WAIT_SOCKET;
  return WAIT_NONE;
}

/* ----- type keys under a specialisation's bindings ---------------------- */

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
  Bindings bindings;         /* its type parameters, for a specialisation */
} WalkContext;

static void note_wait(WalkContext* walk, WaitKind kind) {
  walk->graph->primitive_sites[walk->scope][kind]++;
  WaitNode* node = &walk->graph->nodes[walk->node];
  if (node->direct == WAIT_NONE) node->direct = kind;
}

static void add_edge(WalkContext* walk, int callee) {
  if (callee >= 0) node_add_callee(&walk->graph->nodes[walk->node], callee);
}

/* Every specialisation of a generic template: the conservative edge */
static void edge_to_all_specialisations(WalkContext* walk, const AstDecl* template_decl) {
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
       * its declared type, with this specialisation's bindings */
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
   * specialisations; one that does not wait cannot change the result */
  if (func->is_extern) return;
  /* A generic template: find the specialisation from the type arguments */
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
    edge_to_all_specialisations(walk, target);
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
          for (size_t i = 0; i < holder->callee_count; i++) walk->graph->nodes[holder->callees[i]].spawned = true;
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
    case AST_EXPR_CAST:
      walk_expr(walk, expr->as.cast.operand);
      break;
    case AST_EXPR_CALL: {
      const AstDecl* target = expr->decl_link ? expr->decl_link : (expr->as.call.callee ? expr->as.call.callee->decl_link : NULL);
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
      } else if (expr->decl_link) {
        edge_to_decl(walk, expr->decl_link, expr->as.method_call.args, expr->as.method_call.generic_args,
                     expr->as.method_call.object);
      } else if (walk->bindings.params) {
        edge_by_name(walk, expr->as.method_call.method_name, expr->as.method_call.args, expr->as.method_call.generic_args,
                     expr->as.method_call.object);
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
      for (const AstExprList* item = expr->as.list; item; item = item->next) walk_expr(walk, item->value);
      break;
    case AST_EXPR_INDEX:
      walk_expr(walk, expr->as.index.target);
      walk_expr(walk, expr->as.index.index);
      break;
    case AST_EXPR_COLLECTION_LITERAL:
      for (const AstCollectionElement* element = expr->as.collection.elements; element; element = element->next) walk_expr(walk, element->value);
      break;
    case AST_EXPR_INTERP:
      for (const AstInterpPart* part = expr->as.interp.parts; part; part = part->next) walk_expr(walk, part->value);
      break;
    default:
      break;
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
      }
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
      walk_block(walk, stmt->as.loop_stmt.body);
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
    WalkContext walk = { graph, (int)i, is_stdlib(graph, graph->nodes[i].decl) ? 1 : 0, func, { NULL, NULL } };
    if (graph->nodes[i].key) {
      walk.bindings.params = func->generic_params;
      walk.bindings.args = graph->nodes[i].concrete_args;
    } else if (func->generic_template && func->specialization_args) {
      /* sema's own specialisation: a cloned body that still spells T */
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

static void print_function_name(FILE* out, const WaitNode* node) {
  const AstFuncDecl* func = &node->decl->as.func_decl;
  fprintf(out, "%.*s", (int)func->name.len, func->name.data);
  if (node->key) fprintf(out, "(%s)", node->key);
}

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

static const AstDecl* decl_of_func(const AstFuncDecl* func) {
  return (const AstDecl*)((const char*)func - offsetof(AstDecl, as.func_decl));
}

static void graph_build(WaitGraph* graph, CompilerContext* ctx, const AstModule* merged) {
  memset(graph, 0, sizeof(*graph));
  graph->ctx = ctx;
  graph->merged = merged;
  /* Every specialisation first, so a call can find its own */
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
 * Math, Parallel, Time, Channel), which never blocks for long, minus the
 * sleeps (they wait; S3b makes them suspend) */
static bool extern_is_pool_safe(const WaitGraph* graph, const AstDecl* decl) {
  const AstFuncDecl* func = &decl->as.func_decl;
  if (extern_wait_kind(func) != WAIT_NONE) return false;
  if (!is_stdlib(graph, decl)) return false;
  const char* path = short_lib_path(decl->origin_file);
  static const char* const safe[] = { "lib/core/", "lib/Math.rae", "lib/Parallel.rae", "lib/Time.rae",
                                      "lib/Channel.rae" };
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

static bool node_sleeps(WaitGraph* graph, const WaitNode* node, bool flag) {
  (void)graph; (void)flag;
  return node->direct == WAIT_SLEEP;
}

/* A node a pool task must not reach: a socket wait, C that may block, and a
 * sleep unless it can suspend (`allow_sleep`) */
static bool node_off_pool(WaitGraph* graph, const WaitNode* node, bool allow_sleep) {
  if (node->direct == WAIT_SOCKET) return true;
  if (node->direct == WAIT_SLEEP && !allow_sleep) return true;
  const AstFuncDecl* func = &node->decl->as.func_decl;
  if (func->is_extern) {
    if (extern_wait_kind(func) == WAIT_SLEEP) return !allow_sleep;
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
    if (node->direct == WAIT_SLEEP || node->direct == WAIT_SOCKET) safe = false;
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

bool may_wait_report(CompilerContext* ctx, const AstModule* merged, const char* entry, bool with_lib, FILE* out) {
  WaitGraph graph;
  graph_build(&graph, ctx, merged);

  /* Every count is split: [0] the program, [1] lib/. The default report
   * prints the program's (lib/ grows; a report on a program should not move
   * with it) and names the lib/ twins; `with_lib` adds the lib/ side. */
  size_t functions[2] = { 0, 0 }, specialisations[2] = { 0, 0 }, may_wait[2] = { 0, 0 };
  size_t spawned[2] = { 0, 0 }, reachable[2] = { 0, 0 }, twins[2] = { 0, 0 };
  for (size_t i = 0; i < graph.count; i++) {
    const WaitNode* node = &graph.nodes[i];
    int scope = is_stdlib(&graph, node->decl) ? 1 : 0;
    functions[scope]++;
    if (node->key) specialisations[scope]++;
    if (node->may_wait) may_wait[scope]++;
    if (node->spawned) spawned[scope]++;
    if (node->reachable) reachable[scope]++;
    if (node->may_wait && node->reachable) twins[scope]++;
  }
  int scopes = with_lib ? 2 : 1;
  fprintf(out, "may-wait report: %s\n", file_name(entry));
  for (int scope = 0; scope < scopes; scope++) {
    fprintf(out, "  %s\n", scope == 0 ? "the program:" : "lib/:");
    fprintf(out, "    functions: %zu (%zu generic specialisations)\n", functions[scope], specialisations[scope]);
    fprintf(out, "    wait primitive sites: Task.get %zu, sleep extern %zu, socket/poller extern %zu, task join %zu\n",
            graph.primitive_sites[scope][WAIT_TASK_GET], graph.primitive_sites[scope][WAIT_SLEEP],
            graph.primitive_sites[scope][WAIT_SOCKET], graph.primitive_sites[scope][WAIT_JOIN]);
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
      if (!(node->may_wait && node->reachable) || (is_stdlib(&graph, node->decl) ? 1 : 0) != scope) continue;
      fprintf(out, "    ");
      print_function_name(out, node);
      fprintf(out, "  %s:%zu%s\n      ", scope ? short_lib_path(node->decl->origin_file) : file_name(node->decl->origin_file),
              node->decl->line, node->spawned ? "  spawned" : "");
      print_chain(out, &graph, (int)i);
      fprintf(out, "\n");
    }
  }
  if (twins[1] && !with_lib) {
    fprintf(out, "    lib/:");
    for (size_t i = 0; i < graph.count; i++) {
      const WaitNode* node = &graph.nodes[i];
      if (!(node->may_wait && node->reachable) || !is_stdlib(&graph, node->decl)) continue;
      fprintf(out, " ");
      print_function_name(out, node);
    }
    fprintf(out, "\n");
  }
  graph_release(&graph);
  return true;
}
