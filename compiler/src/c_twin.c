/* Resumable twins (docs/lightweight-spawn-design.md §4.2, §13): a function
 * that may wait and is reachable from a spawn gets, beside its normal C
 * function, a FRAME and a STEP function, so a task on the scheduler's pool
 * suspends at a wait instead of holding its worker.
 *
 *   typedef struct F__frame { int state; RaePoolSave pool; void* child;
 *                             void* slots[N]; int64_t hold[4]; void* blocking;
 *                             RetT result; } F__frame;
 *   static int F__step(F__frame* __rae_frame);   1: finished, 0: suspended
 *
 * The step function is the normal body, emitted by the normal statement
 * emitter inside `switch (__rae_frame->state) { case 0: ... }`, with a
 * `case K:` label after every wait (C may jump into a block, past its
 * declarations, as Duff's device does). The locals stay C locals: at a wait
 * every local in scope is SAVED into its frame slot, and after the label
 * RESTORED from it, so a resume on any worker finds them. A slot is heap
 * storage that never moves (rae_frame_slot), so a `view` / `mod` argument of
 * a child twin points into the parent's slot, and stays valid while the
 * parent is suspended.
 *
 * Waits, hoisted to the start of their statement (in order):
 *   - `t.get()` on a Task named by a path: suspend until `t` is done; the
 *     statement then runs unchanged on a task that is done;
 *   - a sleep extern (Core.sleep's, Time.waitUntil's): suspend until the
 *     deadline; the call itself then reads as 0;
 *   - a socket / poller wait (lib/net's systemPollOne, systemPollerWait):
 *     its arguments are held in the frame, the task suspends until the
 *     socket is ready or the timeout passes (the scheduler's readiness
 *     thread wakes it), and the call then runs with a timeout of 0, so it
 *     answers what is ready without waiting;
 *   - a call to a `blocking` extern (C that may wait long): its arguments are
 *     packed, the call runs on a blocking-call thread while the task
 *     suspends, and the call reads as its result (c_blocking.c);
 *   - a call to another twin: a child frame is stepped until it finishes,
 *     the parent suspending whenever the child does; the call reads as the
 *     child's result.
 * The frame also carries its String-pool temporaries across a suspension
 * (rae_string_pool_detach / _reattach): a statement's own pool scope never
 * spans a wait, the function's does.
 *
 * What does not suspend: a wait the transform cannot reach where it stands
 * (inside a collection loop, a loop condition, a deferred block, the right
 * side of `and` / `or`, while a statement temporary is alive, with an alias
 * local in scope, a twin call with complex arguments or a String result).
 * There a `get()` keeps the scheduler's help-first wait, and a twin call
 * calls the normal function. When such a wait can reach a sleep, the
 * function is not a twin at all (a sleeping worker would hold the pool), and
 * a spawn reaching it stays a thread (c_spawn_on_pool). Which functions are
 * twins is a fixed point: a function whose transform fails is dropped, and
 * the twins calling it are tried again.
 *
 * A Task dropped at the end of its scope, and a taskScope, are joins too:
 * they keep the help-first wait. Rae has no cancellation, so a frame is only
 * freed after it finished: its locals were dropped by its own code. */
#include "c_backend_internal.h"
#include "may_wait.h"
#include "mangler.h"
#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const AstDecl* decl;
  const char* mangled;
  bool twinnable;
  bool reaches_sleep;
} TwinEntry;

typedef struct TwinPlan {
  TwinEntry* entries;
  size_t count;
} TwinPlan;

#define TWIN_MAX_SLOTS 512

typedef struct TwinSlots {
  Str names[TWIN_MAX_SLOTS];
  int count;
} TwinSlots;

static TwinEntry* twin_entry(CompilerContext* ctx, const AstDecl* decl) {
  TwinPlan* plan = ctx->twin_plan;
  if (!plan || !decl) return NULL;
  for (size_t i = 0; i < plan->count; i++) {
    if (plan->entries[i].decl == decl) return &plan->entries[i];
  }
  return NULL;
}

bool c_twin_is(CompilerContext* ctx, const AstDecl* decl) {
  TwinEntry* entry = twin_entry(ctx, decl);
  return entry && entry->twinnable;
}

/* ----- slots ------------------------------------------------------------ */

static int twin_slot(CFuncContext* ctx, Str name) {
  TwinSlots* slots = ctx->twin_slots;
  for (int i = 0; i < slots->count; i++) {
    if (str_eq(slots->names[i], name)) return i;
  }
  if (slots->count >= TWIN_MAX_SLOTS) {
    ctx->twin_failed = true;
    return 0;
  }
  slots->names[slots->count] = name;
  return slots->count++;
}

bool c_twin_slot_ref(CFuncContext* ctx, Str name, Str* out) {
  if (!ctx->twin || !ctx->twin_slots) return false;
  int slot = twin_slot(ctx, name);
  char* text = malloc(name.len * 2 + 96);
  snprintf(text, name.len * 2 + 96, "(*(__typeof__(%.*s)*)__rae_frame->slots[%d])", (int)name.len, name.data,
           slot);
  *out = str_from_cstr(text);
  return true;
}

static Str drop_flag_name(Str name) {
  char* text = malloc(name.len + 16);
  snprintf(text, name.len + 16, "__rae_live_%.*s", (int)name.len, name.data);
  return str_from_cstr(text);
}

/* Every local in scope can be saved: no name twice (an inner one would hide
 * the outer from the C code), and no alias local (`=>` / a `view` or `mod`
 * let), whose pointer may point at another local's C storage */
static bool twin_locals_saveable(CFuncContext* ctx) {
  for (size_t i = 0; i < ctx->local_count; i++) {
    for (size_t j = i + 1; j < ctx->local_count; j++) {
      if (str_eq(ctx->locals[i], ctx->locals[j])) return false;
    }
    bool is_let = ctx->func_first_let_idx != (size_t)-1 && i >= ctx->func_first_let_idx;
    const AstTypeRef* type = ctx->local_type_refs[i];
    if (is_let && (ctx->local_is_ptr[i] || (type && (type->is_view || type->is_mod)))) return false;
  }
  return true;
}

static void emit_save_one(CFuncContext* ctx, FILE* out, Str name) {
  int slot = twin_slot(ctx, name);
  fprintf(out, "  memcpy(rae_frame_slot(&__rae_frame->slots[%d], sizeof(%.*s)), (const void*)&%.*s, sizeof(%.*s));\n",
          slot, (int)name.len, name.data, (int)name.len, name.data, (int)name.len, name.data);
}

static void emit_restore_one(CFuncContext* ctx, FILE* out, Str name) {
  int slot = twin_slot(ctx, name);
  fprintf(out, "  memcpy((void*)&%.*s, __rae_frame->slots[%d], sizeof(%.*s));\n", (int)name.len, name.data, slot,
          (int)name.len, name.data);
}

static void emit_save_all(CFuncContext* ctx, FILE* out) {
  for (size_t i = 0; i < ctx->local_count; i++) {
    emit_save_one(ctx, out, ctx->locals[i]);
    if (ctx->local_drop_flag[i]) emit_save_one(ctx, out, drop_flag_name(ctx->locals[i]));
  }
  for (int i = 0; i < ctx->twin_hoist_count; i++) emit_save_one(ctx, out, ctx->twin_hoist_temps[i]);
}

static void emit_restore_all(CFuncContext* ctx, FILE* out) {
  for (size_t i = 0; i < ctx->local_count; i++) {
    emit_restore_one(ctx, out, ctx->locals[i]);
    if (ctx->local_drop_flag[i]) emit_restore_one(ctx, out, drop_flag_name(ctx->locals[i]));
  }
  for (int i = 0; i < ctx->twin_hoist_count; i++) emit_restore_one(ctx, out, ctx->twin_hoist_temps[i]);
}

/* ----- finding the waits of a statement --------------------------------- */

typedef enum { POINT_JOIN, POINT_SLEEP, POINT_POLL, POINT_BLOCKING, POINT_CALL } PointKind;

typedef struct {
  PointKind kind;
  const AstExpr* expr;      /* the get / the call */
  bool hoistable;           /* evaluated once, unconditionally, by the statement */
  bool sleep_in_ns;         /* POINT_SLEEP: Time_sleepNs (else milliseconds) */
  const AstDecl* callee;    /* POINT_SLEEP / POINT_CALL */
} TwinPoint;

typedef struct {
  TwinPoint items[16];
  int count;
  bool overflow;
} TwinPoints;

static const AstDecl* call_target(const AstExpr* call) {
  if (!call || call->kind != AST_EXPR_CALL) return NULL;
  const AstDecl* target = call->decl_link ? call->decl_link : (call->as.call.callee ? call->as.call.callee->decl_link : NULL);
  return target && target->kind == AST_DECL_FUNC ? target : NULL;
}

static bool is_task_type(const TypeInfo* type) {
  while (type && type->kind == TYPE_REF) type = type->as.ref.base;
  return type && type->kind == TYPE_TASK;
}

/* A path: `t`, `a.b.c` (no calls, nothing evaluated twice when named twice) */
static bool expr_is_path(const AstExpr* expr) {
  while (expr && expr->kind == AST_EXPR_MEMBER) expr = expr->as.member.object;
  return expr && expr->kind == AST_EXPR_IDENT;
}

/* An argument a child frame can take: names, literals and arithmetic on
 * them, nothing that calls or builds */
static bool expr_is_simple(const AstExpr* expr) {
  if (!expr) return true;
  switch (expr->kind) {
    case AST_EXPR_IDENT:
    case AST_EXPR_INTEGER:
    case AST_EXPR_FLOAT:
    case AST_EXPR_STRING:
    case AST_EXPR_BOOL:
    case AST_EXPR_CHAR:
      return true;
    case AST_EXPR_MEMBER:
      return expr_is_simple(expr->as.member.object);
    case AST_EXPR_BINARY:
      return expr_is_simple(expr->as.binary.lhs) && expr_is_simple(expr->as.binary.rhs);
    case AST_EXPR_UNARY:
      return expr->as.unary.op != AST_UNARY_SPAWN && expr_is_simple(expr->as.unary.operand);
    case AST_EXPR_CAST:
      return expr_is_simple(expr->as.cast.operand);
    default:
      return false;
  }
}

static const char* sleep_symbol(const AstDecl* decl) {
  if (!decl || !decl->as.func_decl.is_extern) return NULL;
  const char* symbol = decl->as.func_decl.extern_symbol;
  if (!symbol) return NULL;
  if (strcmp(symbol, "rae_ext_rae_sleep") == 0 || strcmp(symbol, "rae_ext_Time_sleepNs") == 0) return symbol;
  return NULL;
}

/* A socket / poller wait: which argument is the descriptor, which the poll
 * events (-1: wait until readable) and which the timeout in milliseconds */
typedef struct {
  const char* symbol;
  int descriptor;
  int events;
  int timeout;
} PollWait;

static const PollWait poll_waits[] = {
  { "rae_ext_NetSys_pollOne", 0, 1, 2 },
  { "rae_ext_NetSys_pollerWait", 0, -1, 3 },
};

static const PollWait* poll_wait_of(const AstDecl* decl) {
  if (!decl || !decl->as.func_decl.is_extern || !decl->as.func_decl.extern_symbol) return NULL;
  for (size_t i = 0; i < sizeof poll_waits / sizeof poll_waits[0]; i++) {
    if (strcmp(decl->as.func_decl.extern_symbol, poll_waits[i].symbol) == 0) return &poll_waits[i];
  }
  return NULL;
}

/* The value the statement being scanned computes as a whole (a `let`'s, an
 * assignment's, an expression statement's, a one-value `ret`'s): a wait that
 * is this value runs after everything else the statement evaluates, so
 * hoisting it with all its arguments changes no order */
static const AstExpr* s_stmt_value = NULL;

static void add_point(TwinPoints* points, TwinPoint point) {
  if (points->count >= 16) {
    points->overflow = true;
    return;
  }
  points->items[points->count++] = point;
}

static void scan_expr(CFuncContext* ctx, const AstExpr* expr, bool hoistable, TwinPoints* points);

static void scan_args(CFuncContext* ctx, const AstCallArg* args, bool hoistable, TwinPoints* points) {
  for (const AstCallArg* arg = args; arg; arg = arg->next) scan_expr(ctx, arg->value, hoistable, points);
}

static void scan_expr(CFuncContext* ctx, const AstExpr* expr, bool hoistable, TwinPoints* points) {
  if (!expr) return;
  switch (expr->kind) {
    case AST_EXPR_UNARY:
      /* A spawned call runs as its own task: not a wait of this one */
      if (expr->as.unary.op == AST_UNARY_SPAWN) return;
      scan_expr(ctx, expr->as.unary.operand, hoistable, points);
      return;
    case AST_EXPR_BINARY: {
      scan_expr(ctx, expr->as.binary.lhs, hoistable, points);
      /* `a and b` evaluates b only sometimes: a wait there cannot move */
      bool short_circuit = expr->as.binary.op == AST_BIN_AND || expr->as.binary.op == AST_BIN_OR;
      scan_expr(ctx, expr->as.binary.rhs, hoistable && !short_circuit, points);
      return;
    }
    case AST_EXPR_CAST:
      scan_expr(ctx, expr->as.cast.operand, hoistable, points);
      return;
    case AST_EXPR_MEMBER:
      scan_expr(ctx, expr->as.member.object, hoistable, points);
      return;
    case AST_EXPR_INDEX:
      scan_expr(ctx, expr->as.index.target, hoistable, points);
      scan_expr(ctx, expr->as.index.index, hoistable, points);
      return;
    case AST_EXPR_METHOD_CALL: {
      const AstExpr* object = expr->as.method_call.object;
      if (object && is_task_type(object->resolved_type) && expr->as.method_call.method_name.len == 3 &&
          memcmp(expr->as.method_call.method_name.data, "get", 3) == 0) {
        if (expr_is_path(object)) {
          TwinPoint point = { POINT_JOIN, expr, hoistable, false, NULL };
          add_point(points, point);
        }
        return;
      }
      /* A method call to a function that may sleep is not transformed */
      if (expr->decl_link && ctx->compiler_ctx->wait_graph &&
          may_wait_reaches_sleep(ctx->compiler_ctx->wait_graph, expr->decl_link)) {
        ctx->twin_failed = true;
      }
      scan_expr(ctx, object, hoistable, points);
      scan_args(ctx, expr->as.method_call.args, hoistable, points);
      return;
    }
    case AST_EXPR_CALL: {
      const AstDecl* target = call_target(expr);
      if (target && target->as.func_decl.is_extern && target->as.func_decl.is_blocking &&
          !target->as.func_decl.generic_params) {
        TwinPoint point = { POINT_BLOCKING, expr, hoistable, false, target };
        bool simple = true;
        for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) {
          if (!expr_is_simple(arg->value)) simple = false;
        }
        if (!simple && expr != s_stmt_value) point.hoistable = false;
        add_point(points, point);
        return;
      }
      if (poll_wait_of(target)) {
        TwinPoint point = { POINT_POLL, expr, hoistable, false, target };
        bool simple = true;
        for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) {
          if (!expr_is_simple(arg->value)) simple = false;
        }
        /* Its arguments are evaluated once, before the suspension */
        if (!simple && expr != s_stmt_value) point.hoistable = false;
        add_point(points, point);
        return;
      }
      const char* sleep = sleep_symbol(target);
      if (sleep) {
        TwinPoint point = { POINT_SLEEP, expr, hoistable, strcmp(sleep, "rae_ext_Time_sleepNs") == 0, target };
        bool simple = expr->as.call.args && expr_is_simple(expr->as.call.args->value);
        if (!simple) point.hoistable = false;
        add_point(points, point);
        return;
      }
      if (target && c_twin_is(ctx->compiler_ctx, target)) {
        bool simple = true;
        for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) {
          if (!expr_is_simple(arg->value)) simple = false;
        }
        /* A String (or other pool-carried) result is not handed over */
        const AstTypeRef* returns = target->as.func_decl.returns ? target->as.func_decl.returns->type : NULL;
        if (returns) {
          Str base = get_base_type_name(returns);
          if (str_eq_cstr(base, "String") || str_eq_cstr(base, "Any") || returns->is_opt || returns->is_view ||
              returns->is_mod) {
            simple = false;
          }
        }
        TwinPoint point = { POINT_CALL, expr, hoistable && simple, false, target };
        add_point(points, point);
        return;
      }
      if (target && ctx->compiler_ctx->wait_graph && may_wait_reaches_sleep(ctx->compiler_ctx->wait_graph, target)) {
        /* It may sleep, and has no twin to suspend through */
        ctx->twin_failed = true;
      }
      scan_args(ctx, expr->as.call.args, hoistable, points);
      return;
    }
    case AST_EXPR_INTERP:
      for (const AstInterpPart* part = expr->as.interp.parts; part; part = part->next) {
        scan_expr(ctx, part->value, hoistable, points);
      }
      return;
    case AST_EXPR_OBJECT:
      for (const AstObjectField* field = expr->as.object_literal.fields; field; field = field->next) {
        scan_expr(ctx, field->value, hoistable, points);
      }
      return;
    case AST_EXPR_LIST:
      for (const AstExprList* item = expr->as.list; item; item = item->next) scan_expr(ctx, item->value, hoistable, points);
      return;
    case AST_EXPR_COLLECTION_LITERAL:
      for (const AstCollectionElement* element = expr->as.collection.elements; element; element = element->next) {
        scan_expr(ctx, element->value, hoistable, points);
      }
      return;
    case AST_EXPR_MATCH:
      scan_expr(ctx, expr->as.match_expr.subject, hoistable, points);
      for (const AstMatchArm* arm = expr->as.match_expr.arms; arm; arm = arm->next) {
        scan_expr(ctx, arm->value, false, points);
      }
      return;
    default:
      return;
  }
}

/* The statement's own expressions (not those of the blocks it holds): the
 * ones it evaluates once before anything else runs are hoistable */
static void scan_stmt(CFuncContext* ctx, const AstStmt* stmt, TwinPoints* points) {
  s_stmt_value = NULL;
  switch (stmt->kind) {
    case AST_STMT_LET: s_stmt_value = stmt->as.let_stmt.value; break;
    case AST_STMT_EXPR: s_stmt_value = stmt->as.expr_stmt; break;
    case AST_STMT_ASSIGN:
      if (stmt->as.assign_stmt.target && stmt->as.assign_stmt.target->kind == AST_EXPR_IDENT) {
        s_stmt_value = stmt->as.assign_stmt.value;
      }
      break;
    case AST_STMT_RET:
      if (stmt->as.ret_stmt.values && !stmt->as.ret_stmt.values->next) s_stmt_value = stmt->as.ret_stmt.values->value;
      break;
    default: break;
  }
  switch (stmt->kind) {
    case AST_STMT_LET:
      scan_expr(ctx, stmt->as.let_stmt.value, true, points);
      return;
    case AST_STMT_DESTRUCT:
      scan_expr(ctx, stmt->as.destruct_stmt.call, true, points);
      return;
    case AST_STMT_EXPR:
      scan_expr(ctx, stmt->as.expr_stmt, true, points);
      return;
    case AST_STMT_RET:
      for (const AstReturnArg* arg = stmt->as.ret_stmt.values; arg; arg = arg->next) scan_expr(ctx, arg->value, true, points);
      return;
    case AST_STMT_ASSIGN:
      scan_expr(ctx, stmt->as.assign_stmt.target, true, points);
      scan_expr(ctx, stmt->as.assign_stmt.value, true, points);
      return;
    case AST_STMT_IF:
      if (stmt->as.if_stmt.binding) scan_stmt(ctx, stmt->as.if_stmt.binding, points);
      scan_expr(ctx, stmt->as.if_stmt.condition, true, points);
      return;
    case AST_STMT_MATCH:
      if (stmt->as.match_stmt.binding) scan_stmt(ctx, stmt->as.match_stmt.binding, points);
      scan_expr(ctx, stmt->as.match_stmt.subject, true, points);
      return;
    case AST_STMT_LOOP:
      /* The condition and increment run every iteration */
      scan_expr(ctx, stmt->as.loop_stmt.condition, false, points);
      scan_expr(ctx, stmt->as.loop_stmt.increment, false, points);
      return;
    default:
      return;
  }
}

static bool temps_alive(const CFuncContext* ctx) {
  for (const CStmtTemps* temps = ctx->stmt_temps; temps; temps = temps->parent) {
    if (temps->count > 0) return true;
  }
  for (int i = 0; i < ctx->loop_depth; i++) {
    if (ctx->loop_temps[i] && ctx->loop_temps[i]->count > 0) return true;
  }
  return false;
}

/* ----- emitting a suspension --------------------------------------------- */

/* The tail of a suspension that registered the task with a waker: the frame
 * is ready, its pool entries go with it, and nothing touches it once the
 * task is registered (another worker may already resume it) */
static void emit_register_and_suspend(FILE* out, int state, const char* register_call) {
  fprintf(out, "  __rae_frame->state = %d;\n", state);
  fprintf(out, "  rae_string_pool_detach(__rae_spm_func, &__rae_frame->pool);\n");
  fprintf(out, "  if (%s) return 0;\n", register_call);
  fprintf(out, "  rae_string_pool_reattach(&__rae_frame->pool);\n");
}

static void add_override(CFuncContext* ctx, const AstExpr* expr, const char* text) {
  if (ctx->twin_override_count >= 16) {
    ctx->twin_failed = true;
    return;
  }
  ctx->twin_override_expr[ctx->twin_override_count] = expr;
  snprintf(ctx->twin_override_text[ctx->twin_override_count], sizeof ctx->twin_override_text[0], "%s", text);
  ctx->twin_override_count++;
}

static void emit_join(CFuncContext* ctx, const TwinPoint* point, FILE* out) {
  int state = ++ctx->twin_state;
  char* task_text = NULL;
  size_t task_length = 0;
  FILE* task_out = open_memstream(&task_text, &task_length);
  emit_expr(ctx, point->expr->as.method_call.object, task_out, PREC_LOWEST, false, false);
  fclose(task_out);
  char* call = malloc(task_length + 64);
  snprintf(call, task_length + 64, "rae_sched_wait_task(%s)", task_text);
  fprintf(out, "  /* resumable: wait for a task */\n");
  emit_save_all(ctx, out);
  emit_register_and_suspend(out, state, call);
  fprintf(out, "  case %d: ;\n", state);
  emit_restore_all(ctx, out);
  free(call);
  free(task_text);
}

static void emit_sleep(CFuncContext* ctx, const TwinPoint* point, FILE* out) {
  int state = ++ctx->twin_state;
  fprintf(out, "  /* resumable: sleep */\n  {\n  int64_t __rae_deadline%d = rae_sched_now_ns() + (int64_t)(", state);
  emit_expr(ctx, point->expr->as.call.args->value, out, PREC_LOWEST, false, false);
  fprintf(out, ")%s;\n", point->sleep_in_ns ? "" : " * 1000000LL");
  emit_save_all(ctx, out);
  char call[96];
  snprintf(call, sizeof call, "rae_sched_sleep_until(__rae_deadline%d)", state);
  emit_register_and_suspend(out, state, call);
  fprintf(out, "  }\n  case %d: ;\n", state);
  emit_restore_all(ctx, out);
  const AstFuncDecl* func = &point->callee->as.func_decl;
  add_override(ctx, point->expr, func->returns ? "((int64_t)0)" : "((void)0)");
}

/* A socket / poller wait: the arguments go into the frame's `hold`, the task
 * waits with the scheduler's readiness thread (rae_sched_io_wait), and the
 * call reads as itself with the held arguments and a timeout of 0. When the
 * task cannot suspend (a platform without kqueue, a timeout of 0) it is the
 * call with its own timeout, as in the normal function. */
static void emit_poll(CFuncContext* ctx, const TwinPoint* point, FILE* out) {
  const PollWait* wait = poll_wait_of(point->callee);
  const AstFuncDecl* func = &point->callee->as.func_decl;
  int state = ++ctx->twin_state;
  int count = 0;
  bool is_pointer[4] = { false, false, false, false };
  fprintf(out, "  /* resumable: socket wait */\n  {\n");
  const AstParam* param = func->params;
  for (const AstCallArg* arg = point->expr->as.call.args; arg; arg = arg->next, count++) {
    if (count >= 4 || !param) {
      ctx->twin_failed = true;
      return;
    }
    Str type_name = get_base_type_name(param->type);
    is_pointer[count] = str_eq_cstr(type_name, "Buffer") || str_eq_cstr(type_name, "Ptr");
    fprintf(out, "  __rae_frame->hold[%d] = (int64_t)%s(", count, is_pointer[count] ? "(intptr_t)" : "");
    emit_expr(ctx, arg->value, out, PREC_LOWEST, false, false);
    fprintf(out, ");\n");
    param = param->next;
  }
  fprintf(out, "  int64_t __rae_timeout%d = __rae_frame->hold[%d];\n", state, wait->timeout);
  fprintf(out, "  __rae_frame->hold[%d] = 0;\n", wait->timeout);
  emit_save_all(ctx, out);
  char call[160];
  if (wait->events >= 0) {
    snprintf(call, sizeof call, "rae_sched_io_wait(__rae_frame->hold[%d], __rae_frame->hold[%d], __rae_timeout%d)",
             wait->descriptor, wait->events, state);
  } else {
    snprintf(call, sizeof call, "rae_sched_io_wait(__rae_frame->hold[%d], -1, __rae_timeout%d)", wait->descriptor,
             state);
  }
  emit_register_and_suspend(out, state, call);
  fprintf(out, "  __rae_frame->hold[%d] = __rae_timeout%d;\n  }\n  case %d: ;\n", wait->timeout, state, state);
  emit_restore_all(ctx, out);
  char text[256];
  int length = snprintf(text, sizeof text, "%s(", func->extern_symbol);
  for (int i = 0; i < count && length < (int)sizeof text; i++) {
    length += snprintf(text + length, sizeof text - (size_t)length, "%s%s__rae_frame->hold[%d]", i ? ", " : "",
                       is_pointer[i] ? "(void*)(intptr_t)" : "", i);
  }
  if (length < (int)sizeof text) snprintf(text + length, sizeof text - (size_t)length, ")");
  add_override(ctx, point->expr, text);
}

/* A call to a `blocking` extern (c_blocking.c): the arguments are packed by
 * the normal call's own argument text, the call goes to a blocking-call
 * thread (rae_sched_blocking_call) while the task suspends, and the call
 * reads as the result it stored. Where no thread can be had it runs here. */
static void emit_blocking(CFuncContext* ctx, const TwinPoint* point, FILE* out) {
  int state = ++ctx->twin_state;
  const char* symbol = rae_mangle_function(ctx->compiler_ctx, &point->callee->as.func_decl);
  char* text = NULL;
  size_t length = 0;
  FILE* text_out = open_memstream(&text, &length);
  emit_expr(ctx, point->expr, text_out, PREC_LOWEST, false, false);
  fclose(text_out);
  size_t start = 0, end = 0;
  /* A temporary the call takes the address of would not outlive this step */
  if (!text || !c_blocking_find_call(text, symbol, &start, &end) || strstr(text, "__rae_pw_") || temps_alive(ctx)) {
    ctx->twin_failed = true;
    free(text);
    return;
  }
  size_t symbol_length = strlen(symbol);
  fprintf(out, "  /* resumable: blocking call */\n  {\n  __rae_frame->blocking = __raeblock_pack_%s(", symbol);
  fwrite(text + start + symbol_length + 1, 1, end - start - symbol_length - 1, out);
  fprintf(out, ";\n");
  emit_save_all(ctx, out);
  char call[512];
  snprintf(call, sizeof call, "rae_sched_blocking_call(__raeblock_run_%s, __rae_frame->blocking)", symbol);
  emit_register_and_suspend(out, state, call);
  fprintf(out, "  __raeblock_run_%s(__rae_frame->blocking);\n  }\n  case %d: ;\n", symbol, state);
  emit_restore_all(ctx, out);
  bool has_result = point->callee->as.func_decl.returns != NULL;
  char name[48];
  snprintf(name, sizeof name, "__rae_wait%d", state);
  if (has_result) {
    fprintf(out, "  __typeof__(((__raeblock_args_%s*)0)->result) %s = ((__raeblock_args_%s*)__rae_frame->blocking)->result;\n",
            symbol, name, symbol);
  }
  fprintf(out, "  free(__rae_frame->blocking);\n  __rae_frame->blocking = NULL;\n");
  /* The call reads as its result, inside whatever the normal emission wraps
   * around it */
  char override[256];
  const char* value = has_result ? name : "((void)0)";
  if (start + strlen(value) + (length - end) + 1 > sizeof override) {
    ctx->twin_failed = true;
    free(text);
    return;
  }
  snprintf(override, sizeof override, "%.*s%s%s", (int)start, text, value, text + end);
  add_override(ctx, point->expr, override);
  if (has_result && ctx->twin_hoist_count < 16) ctx->twin_hoist_temps[ctx->twin_hoist_count++] = str_from_cstr(strdup(name));
  free(text);
}

static void emit_child_call(CFuncContext* ctx, const TwinPoint* point, FILE* out) {
  int state = ++ctx->twin_state;
  TwinEntry* callee = twin_entry(ctx->compiler_ctx, point->callee);
  const char* mangled = callee->mangled;
  fprintf(out, "  /* resumable: call %s */\n", mangled);
  emit_save_all(ctx, out);
  /* The child frame is made from the arguments exactly as the normal call
   * passes them (the same text, its function swapped for the frame's
   * constructor), with the locals read from their slots: a borrow points at
   * a slot, which outlives this step */
  char* call_text = NULL;
  size_t call_length = 0;
  FILE* call_out = open_memstream(&call_text, &call_length);
  ctx->twin_args_from_frame = true;
  emit_expr(ctx, point->expr, call_out, PREC_LOWEST, false, false);
  ctx->twin_args_from_frame = false;
  fclose(call_out);
  char head[512];
  snprintf(head, sizeof head, "%s(", mangled);
  const char* found = call_text ? strstr(call_text, head) : NULL;
  /* A temporary the call takes the address of would not outlive this step */
  if (!found || strstr(call_text, "__rae_pw_")) {
    ctx->twin_failed = true;
    free(call_text);
    return;
  }
  fprintf(out, "  __rae_frame->child = ");
  fwrite(call_text, 1, (size_t)(found - call_text), out);
  fprintf(out, "%s__frame_new(%s;\n", mangled, found + strlen(head));
  free(call_text);
  fprintf(out, "  __rae_frame->state = %d;\n", state);
  fprintf(out, "  case %d: ;\n", state);
  /* The child may suspend: this frame's pool entries leave first, since
   * nothing may touch the frame after the child registered the task */
  fprintf(out, "  rae_string_pool_detach(__rae_spm_func, &__rae_frame->pool);\n");
  fprintf(out, "  if (!%s__step((%s__frame*)__rae_frame->child)) return 0;\n", mangled, mangled);
  fprintf(out, "  rae_string_pool_reattach(&__rae_frame->pool);\n");
  emit_restore_all(ctx, out);
  bool has_result = point->callee->as.func_decl.returns != NULL;
  char name[48];
  snprintf(name, sizeof name, "__rae_wait%d", state);
  if (has_result) {
    fprintf(out, "  __typeof__(((%s__frame*)0)->result) %s = ((%s__frame*)__rae_frame->child)->result;\n", mangled, name,
            mangled);
  }
  fprintf(out, "  %s__frame_free((%s__frame*)__rae_frame->child);\n  __rae_frame->child = NULL;\n", mangled, mangled);
  if (has_result) {
    add_override(ctx, point->expr, name);
    if (ctx->twin_hoist_count < 16) ctx->twin_hoist_temps[ctx->twin_hoist_count++] = str_from_cstr(strdup(name));
  } else {
    add_override(ctx, point->expr, "((void)0)");
  }
}

void c_twin_before_stmt(CFuncContext* ctx, const AstStmt* stmt, FILE* out) {
  ctx->twin_override_count = 0;
  ctx->twin_hoist_count = 0;
  TwinPoints points = { .count = 0 };
  scan_stmt(ctx, stmt, &points);
  if (points.overflow) ctx->twin_failed = true;
  if (points.count == 0) return;
  bool can_suspend = ctx->twin_no_suspend == 0 && !temps_alive(ctx) && twin_locals_saveable(ctx);
  for (int i = 0; i < points.count; i++) {
    const TwinPoint* point = &points.items[i];
    bool reaches_sleep = point->kind == POINT_SLEEP || point->kind == POINT_POLL || point->kind == POINT_BLOCKING ||
        (point->kind == POINT_CALL && twin_entry(ctx->compiler_ctx, point->callee)->reaches_sleep);
    if (!can_suspend || !point->hoistable) {
      /* A get keeps the help-first wait; a call runs the normal function.
       * Neither may sleep on the worker. */
      if (reaches_sleep) ctx->twin_failed = true;
      continue;
    }
    switch (point->kind) {
      case POINT_JOIN: emit_join(ctx, point, out); break;
      case POINT_SLEEP: emit_sleep(ctx, point, out); break;
      case POINT_POLL: emit_poll(ctx, point, out); break;
      case POINT_BLOCKING: emit_blocking(ctx, point, out); break;
      case POINT_CALL: emit_child_call(ctx, point, out); break;
    }
  }
}

void c_twin_after_stmt(CFuncContext* ctx) {
  ctx->twin_override_count = 0;
  ctx->twin_hoist_count = 0;
}

bool c_twin_override_expr(CFuncContext* ctx, const AstExpr* expr, FILE* out) {
  for (int i = 0; i < ctx->twin_override_count; i++) {
    if (ctx->twin_override_expr[i] == expr) {
      fputs(ctx->twin_override_text[i], out);
      return true;
    }
  }
  return false;
}

/* ----- one twin ------------------------------------------------------------ */

static bool body_has_opaque_loop(const AstBlock* block);

static bool stmt_has_opaque_loop(const AstStmt* stmt) {
  switch (stmt->kind) {
    case AST_STMT_LOOP:
      if (stmt->as.loop_stmt.is_parallel || stmt->as.loop_stmt.is_main_loop) return true;
      return body_has_opaque_loop(stmt->as.loop_stmt.body);
    case AST_STMT_IF:
      return body_has_opaque_loop(stmt->as.if_stmt.then_block) || body_has_opaque_loop(stmt->as.if_stmt.else_block);
    case AST_STMT_MATCH:
      for (const AstMatchCase* match_case = stmt->as.match_stmt.cases; match_case; match_case = match_case->next) {
        if (body_has_opaque_loop(match_case->block)) return true;
      }
      return false;
    case AST_STMT_UNSAFE:
      return body_has_opaque_loop(stmt->as.unsafe_stmt.block);
    case AST_STMT_DEFER:
      return body_has_opaque_loop(stmt->as.defer_stmt.block);
    default:
      return false;
  }
}

static bool body_has_opaque_loop(const AstBlock* block) {
  if (!block) return false;
  for (const AstStmt* stmt = block->first; stmt; stmt = stmt->next) {
    if (stmt_has_opaque_loop(stmt)) return true;
  }
  return false;
}

/* Emit the twin of `entry`, its frame struct and constructor into `decls`
 * and its step function into `steps`, or (both NULL) only learn whether the
 * transform succeeds */
static bool twin_emit_one(CompilerContext* ctx, const AstModule* module, TwinEntry* entry, FILE* decls, FILE* steps) {
  const AstFuncDecl* func = &entry->decl->as.func_decl;
  TwinSlots* slots = calloc(1, sizeof(TwinSlots));
  CFuncContext tctx = { .compiler_ctx = ctx, .module = module, .func_decl = func, .func_first_let_idx = (size_t)-1 };
  tctx.twin = true;
  tctx.twin_slots = slots;
  for (const AstParam* param = func->params; param; param = param->next) {
    if (tctx.local_count >= 256) break;
    tctx.locals[tctx.local_count] = param->name;
    tctx.local_type_refs[tctx.local_count] = param->type;
    tctx.local_types[tctx.local_count] = str_from_cstr(rae_mangle_type_specialized(ctx, NULL, NULL, param->type));
    if (param->type && (param->type->is_own || param->type->is_copy)) tctx.local_struct_owns_heap[tctx.local_count] = true;
    twin_slot(&tctx, param->name);
    tctx.local_count++;
  }
  size_t first_let = tctx.local_count;
  tctx.func_first_let_idx = first_let;
  tctx.func_may_pool = true;
  const char* returns = c_return_type(&tctx, func);
  bool is_void = strcmp(returns, "void") == 0;

  char* body = NULL;
  size_t body_length = 0;
  FILE* body_out = open_memstream(&body, &body_length);
  fprintf(body_out, "  switch (__rae_frame->state) {\n  case 0: ;\n");
  if (func->body) {
    for (const AstStmt* stmt = func->body->first; stmt; stmt = stmt->next) emit_stmt(&tctx, stmt, body_out);
  }
  if (tctx.defer_stack.count > 0) emit_defers(&tctx, 0, body_out);
  emit_implicit_drops_for_body(&tctx, body_out, first_let);
  emit_implicit_drops_for_own_params(&tctx, body_out, first_let);
  fprintf(body_out, "  rae_string_pool_flush(__rae_spm_func);\n");
  if (!is_void) fprintf(body_out, "  __rae_frame->result = (%s){0};\n", returns);
  fprintf(body_out, "  return 1;\n  }\n  return 1;\n");
  fclose(body_out);

  bool ok = !tctx.twin_failed;
  if (ok && decls && steps) {
    FILE* out = decls;
    const char* mangled = entry->mangled;
    int slot_count = slots->count > 0 ? slots->count : 1;
    fprintf(out, "struct %s__frame { int state; RaePoolSave pool; void* child; void* slots[%d]; int64_t hold[4]; void* blocking;", mangled, slot_count);
    if (!is_void) fprintf(out, " %s result;", returns);
    fprintf(out, " };\n");
    fprintf(out, "RAE_UNUSED static %s__frame* %s__frame_new(", mangled, mangled);
    emit_param_list(&tctx, func->params, out, false);
    fprintf(out, ") {\n  %s__frame* __rae_frame = (%s__frame*)calloc(1, sizeof(%s__frame));\n", mangled, mangled, mangled);
    for (const AstParam* param = func->params; param; param = param->next) {
      emit_save_one(&tctx, out, param->name);
    }
    fprintf(out, "  return __rae_frame;\n}\n");
    fprintf(out, "RAE_UNUSED static void %s__frame_free(%s__frame* __rae_frame) {\n", mangled, mangled);
    fprintf(out, "  rae_frame_slots_free(__rae_frame->slots, %d);\n  free(__rae_frame->pool.entries);\n"
                 "  free(__rae_frame);\n}\n", slot_count);
    out = steps;
    fprintf(out, "RAE_UNUSED static int %s__step(%s__frame* __rae_frame) {\n", mangled, mangled);
    for (const AstParam* param = func->params; param; param = param->next) {
      fprintf(out, "  ");
      emit_param_c_type(&tctx, param, out, false);
      fprintf(out, " %.*s;\n", (int)param->name.len, param->name.data);
      emit_restore_one(&tctx, out, param->name);
    }
    fprintf(out, "  int __rae_spm_func = rae_string_pool_mark();\n");
    fprintf(out, "  rae_string_pool_reattach(&__rae_frame->pool);\n");
    fputs(body, out);
    fprintf(out, "}\n\n");
  }
  free(body);
  free(slots);
  return ok;
}

/* ----- the plan -------------------------------------------------------------- */

void c_twins_plan(CompilerContext* ctx, const AstModule* module) {
  c_twins_free(ctx);
  if (!ctx->wait_graph) return;
  TwinPlan* plan = calloc(1, sizeof(TwinPlan));
  ctx->twin_plan = plan;
  for (size_t i = 0; i < ctx->all_decl_count; i++) {
    const AstDecl* decl = ctx->all_decls[i];
    if (decl->kind != AST_DECL_FUNC) continue;
    const AstFuncDecl* func = &decl->as.func_decl;
    if (func->generic_params || func->is_extern || !func->body || str_eq_cstr(func->name, "main")) continue;
    if (!may_wait_twin_candidate(ctx->wait_graph, decl)) continue;
    if (body_has_opaque_loop(func->body)) continue;
    plan->entries = realloc(plan->entries, (plan->count + 1) * sizeof(TwinEntry));
    TwinEntry* entry = &plan->entries[plan->count++];
    entry->decl = decl;
    entry->mangled = rae_mangle_function(ctx, func);
    entry->twinnable = true;
    entry->reaches_sleep = may_wait_reaches_sleep(ctx->wait_graph, decl);
  }
  /* The fixed point: drop every twin whose transform fails, until none does */
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < plan->count; i++) {
      if (!plan->entries[i].twinnable) continue;
      if (!twin_emit_one(ctx, module, &plan->entries[i], NULL, NULL)) {
        plan->entries[i].twinnable = false;
        changed = true;
      }
    }
  }
  if (getenv("RAE_TWIN_DEBUG")) {
    for (size_t i = 0; i < plan->count; i++) {
      fprintf(stderr, "twin %s: %s%s\n", plan->entries[i].mangled, plan->entries[i].twinnable ? "yes" : "no",
              plan->entries[i].reaches_sleep ? " (sleeps)" : "");
    }
  }
}

void c_twins_emit(CompilerContext* ctx, const AstModule* module, FILE* out) {
  TwinPlan* plan = ctx->twin_plan;
  if (!plan) return;
  for (size_t i = 0; i < plan->count; i++) {
    const TwinEntry* entry = &plan->entries[i];
    if (!entry->twinnable) continue;
    fprintf(out, "typedef struct %s__frame %s__frame;\n", entry->mangled, entry->mangled);
    fprintf(out, "RAE_UNUSED static int %s__step(%s__frame* __rae_frame);\n", entry->mangled, entry->mangled);
    fprintf(out, "RAE_UNUSED static void %s__frame_free(%s__frame* __rae_frame);\n", entry->mangled, entry->mangled);
  }
  /* Every frame struct and constructor first: a step names its children's */
  char* steps = NULL;
  size_t steps_length = 0;
  FILE* steps_out = open_memstream(&steps, &steps_length);
  for (size_t i = 0; i < plan->count; i++) {
    TwinEntry* entry = &plan->entries[i];
    if (entry->twinnable) twin_emit_one(ctx, module, entry, out, steps_out);
  }
  fclose(steps_out);
  if (steps) fputs(steps, out);
  free(steps);
}

void c_twins_free(CompilerContext* ctx) {
  if (!ctx->twin_plan) return;
  free(ctx->twin_plan->entries);
  free(ctx->twin_plan);
  ctx->twin_plan = NULL;
}
