/* Bounds-check elimination in counted loops, by loop versioning.
 *
 * A List read or write, and a String's `byteAt`, is checked against the
 * length on every call (`copyAtFallback` answers the fallback past the end,
 * `set` warns, `byteAt` answers -1). In a byte loop those checks, and the
 * reloads of the length and data that C's aliasing rules force after every
 * byte store, cost as much as the work: base64 decoding written in C with
 * Rae's checks takes 3x the unchecked loop. When the compiler can bound every
 * index the loop will use, it checks the whole range ONCE before the loop and
 * runs a copy of the loop whose proven accesses are a plain load or store
 * through data held in a local. When the range check fails, the loop runs as
 * written, so a program that reads or writes out of range still sees the
 * fallback, the warning or the -1.
 *
 * What is proven (everything else keeps its checks, in both copies):
 * - the loop: a counted `loop cond { }` or `loop var i, cond, ++i { }`,
 *   whose condition is `p < E`, `p <= E`, `p + c < E` or `p + c <= E`, with
 *   E unchanged by the body and p an induction variable;
 * - induction variables: Int locals the body changes exactly once, at its top
 *   level and unconditionally, by `v = v + K` (K a positive literal), or the
 *   loop's own `++i`; the body has no `continue` (it would skip an update);
 * - accesses at index `v`, `v + c` or `v - c`: `L.copyAtFallback` /
 *   `copyAtDefault` / `set` and the `if let … = L.copyAt / viewAt / modAt`
 *   forms on a List local or parameter `L` of plain elements, and
 *   `S.byteAt` on a String `S` the body does not assign;
 * - table reads: a List read whose index is a proven `byteAt` (0..255 once
 *   unchecked), when the list has at least 256 elements;
 * - nothing in the body may move a list's storage: no add/remove/clear/…
 *   on any list (except directly before a `ret`/`break`, which leaves the
 *   loop), no assignment to a list or an induction variable, no argument
 *   passed to a `mod` parameter (other than the list a `set` / `modAt` is
 *   called on), and no call whose declaration is unknown (a List's and a
 *   String's own methods excepted, which reach no other list).
 *
 * RAE_NO_LOOP_VERSIONING=1 turns it off (to compare). */
#include "c_backend.h"
#include "c_backend_internal.h"
#include "sema.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

#define LV_MAX_IVS 8
#define LV_MAX_ACCESSES 64
#define LV_MAX_LISTS 8
#define LV_AFTER_BODY 1000000

typedef enum { LV_LIST_AT_IV, LV_STRING_AT_IV, LV_LIST_AT_BYTE } LvKind;

typedef struct {
  Str name;
  long long step;
  int update_pos;            /* top-level statement index of the update; LV_AFTER_BODY for ++i */
  int updates;
  const AstExpr* ident;      /* an expression naming it, to emit its C value */
} LvIv;

typedef struct {
  LvKind kind;
  const AstExpr* call;       /* the method call */
  int list;                  /* index into lists (a list or a String) */
  int iv;
  long long offset;
  int pos;                   /* top-level statement index containing it */
} LvAccess;

typedef struct {
  Str name;
  const AstExpr* expr;       /* the identifier */
  const AstTypeRef* type;
  bool is_string;
} LvList;

typedef struct LoopVersionPlan {
  int id;
  LvIv ivs[LV_MAX_IVS];
  int iv_count;
  int primary;
  bool strict;
  long long cond_offset;
  const AstExpr* bound;
  LvAccess accesses[LV_MAX_ACCESSES];
  int access_count;
  LvList lists[LV_MAX_LISTS];
  int list_count;
  bool ok;
} LoopVersionPlan;

static bool lv_trace(void) { return getenv("RAE_LOOP_VERSIONING_TRACE") != NULL; }

static bool lv_int_literal(const AstExpr* e, long long* value) {
  if (!e || e->kind != AST_EXPR_INTEGER) return false;
  char buf[32];
  size_t n = e->as.integer.len < sizeof buf - 1 ? e->as.integer.len : sizeof buf - 1;
  memcpy(buf, e->as.integer.data, n);
  buf[n] = 0;
  char* end = NULL;
  long long v = strtoll(buf, &end, 10);
  if (!end || *end) return false;
  *value = v;
  return true;
}

static int lv_iv_index(const LoopVersionPlan* plan, Str name) {
  for (int i = 0; i < plan->iv_count; i++) if (str_eq(plan->ivs[i].name, name)) return i;
  return -1;
}

/* `v = v + K` / `v = K + v` with K > 0: the name and K */
static bool lv_step_assignment(const AstStmt* s, Str* name, long long* step) {
  if (s->kind != AST_STMT_ASSIGN || s->as.assign_stmt.is_bind) return false;
  const AstExpr* target = s->as.assign_stmt.target;
  const AstExpr* value = s->as.assign_stmt.value;
  if (!target || target->kind != AST_EXPR_IDENT || !value || value->kind != AST_EXPR_BINARY
      || value->as.binary.op != AST_BIN_ADD) return false;
  const AstExpr* l = value->as.binary.lhs;
  const AstExpr* r = value->as.binary.rhs;
  long long k = 0;
  if (l->kind == AST_EXPR_IDENT && str_eq(l->as.ident, target->as.ident) && lv_int_literal(r, &k)) {}
  else if (r->kind == AST_EXPR_IDENT && str_eq(r->as.ident, target->as.ident) && lv_int_literal(l, &k)) {}
  else return false;
  if (k <= 0) return false;
  *name = target->as.ident;
  *step = k;
  return true;
}

/* `++v` / `v++`: the name */
static bool lv_increment(const AstExpr* e, Str* name) {
  if (!e || e->kind != AST_EXPR_UNARY) return false;
  if (e->as.unary.op != AST_UNARY_PRE_INC && e->as.unary.op != AST_UNARY_POST_INC) return false;
  if (!e->as.unary.operand || e->as.unary.operand->kind != AST_EXPR_IDENT) return false;
  *name = e->as.unary.operand->as.ident;
  return true;
}

static Str lv_base(CFuncContext* ctx, const AstExpr* e) {
  const AstTypeRef* t = e ? infer_expr_type_ref(ctx, e) : NULL;
  if (!t || t->is_opt) return (Str){0};
  return get_base_type_name(t);
}

static bool lv_is_int(CFuncContext* ctx, const AstExpr* e) {
  Str base = lv_base(ctx, e);
  return str_eq_cstr(base, "Int") || str_eq_cstr(base, "Int64");
}

static bool lv_is_list(CFuncContext* ctx, const AstExpr* e) {
  return e && e->kind == AST_EXPR_IDENT && str_eq_cstr(lv_base(ctx, e), "List");
}

/* Names the body assigns (anything but the induction-variable updates) */
typedef struct {
  Str names[64];
  int count;
  bool overflow;
} LvAssigned;

static void lv_note_assigned(LvAssigned* a, Str name) {
  for (int i = 0; i < a->count; i++) if (str_eq(a->names[i], name)) return;
  if (a->count < 64) a->names[a->count++] = name; else a->overflow = true;
}

static bool lv_assigned(const LvAssigned* a, Str name) {
  if (a->overflow) return true;
  for (int i = 0; i < a->count; i++) if (str_eq(a->names[i], name)) return true;
  return false;
}

static Str lv_root_name(const AstExpr* e) {
  while (e && (e->kind == AST_EXPR_MEMBER || e->kind == AST_EXPR_INDEX))
    e = e->kind == AST_EXPR_MEMBER ? e->as.member.object : e->as.index.target;
  if (e && e->kind == AST_EXPR_IDENT) return e->as.ident;
  return (Str){0};
}

typedef struct {
  CFuncContext* ctx;
  LoopVersionPlan* plan;
  LvAssigned assigned;
  int pos;
} LvWalk;

static void lv_walk_expr(LvWalk* w, const AstExpr* e);

static bool lv_param_is_mod(const AstExpr* call, Str arg_name, bool is_receiver) {
  const AstDecl* d = call->decl_link;
  if (!d || d->kind != AST_DECL_FUNC) return false;
  const AstParam* p = d->as.func_decl.params;
  if (is_receiver) return p && p->type && p->type->is_mod;
  for (; p; p = p->next)
    if (str_eq(p->name, arg_name)) return p->type && p->type->is_mod;
  return false;
}

static bool lv_list_method_moves_storage(Str method) {
  static const char* keeps[] = { "copyAt", "copyAtFallback", "copyAtDefault", "viewAt", "modAt",
                                 "set", "length", "isEmpty", "contains", "indexOf", "prefetch", NULL };
  for (int i = 0; keeps[i]; i++) if (str_eq_cstr(method, keeps[i])) return false;
  return true;
}

static const AstExpr* lv_arg(const AstExpr* call, const char* name) {
  for (const AstCallArg* a = call->as.method_call.args; a; a = a->next)
    if (str_eq_cstr(a->name, name)) return a->value;
  return NULL;
}

/* `v`, `v + c`, `c + v`, `v - c` with v an induction variable */
static bool lv_iv_index_expr(const LoopVersionPlan* plan, const AstExpr* index, int* iv, long long* offset) {
  if (!index) return false;
  const AstExpr* iv_expr = NULL;
  long long k = 0;
  *offset = 0;
  if (index->kind == AST_EXPR_IDENT) {
    iv_expr = index;
  } else if (index->kind == AST_EXPR_BINARY
             && (index->as.binary.op == AST_BIN_ADD || index->as.binary.op == AST_BIN_SUB)) {
    if (index->as.binary.lhs->kind == AST_EXPR_IDENT && lv_int_literal(index->as.binary.rhs, &k)) {
      iv_expr = index->as.binary.lhs;
      *offset = index->as.binary.op == AST_BIN_ADD ? k : -k;
    } else if (index->as.binary.op == AST_BIN_ADD && index->as.binary.rhs->kind == AST_EXPR_IDENT
               && lv_int_literal(index->as.binary.lhs, &k)) {
      iv_expr = index->as.binary.rhs;
      *offset = k;
    }
  }
  if (!iv_expr) return false;
  *iv = lv_iv_index(plan, iv_expr->as.ident);
  return *iv >= 0;
}

static int lv_list_slot(LoopVersionPlan* plan, const AstExpr* object, const AstTypeRef* type, bool is_string) {
  for (int i = 0; i < plan->list_count; i++)
    if (str_eq(plan->lists[i].name, object->as.ident)) return i;
  if (plan->list_count >= LV_MAX_LISTS) return -1;
  int slot = plan->list_count++;
  plan->lists[slot] = (LvList){ object->as.ident, object, type, is_string };
  return slot;
}

static const LvAccess* lv_find_access(const LoopVersionPlan* plan, const AstExpr* call) {
  for (int i = 0; i < plan->access_count; i++) if (plan->accesses[i].call == call) return &plan->accesses[i];
  return NULL;
}

static void lv_add_access(LvWalk* w, LvKind kind, const AstExpr* call, int list, int iv, long long offset) {
  LoopVersionPlan* plan = w->plan;
  if (list < 0 || plan->access_count >= LV_MAX_ACCESSES) return;
  plan->accesses[plan->access_count++] = (LvAccess){ kind, call, list, iv, offset, w->pos };
}

/* `S.byteAt(index: v + c)` on a String identifier */
static void lv_consider_string_read(LvWalk* w, const AstExpr* call) {
  if (!str_eq_cstr(call->as.method_call.method_name, "byteAt")) return;
  const AstExpr* object = call->as.method_call.object;
  if (!object || object->kind != AST_EXPR_IDENT || !str_eq_cstr(lv_base(w->ctx, object), "String")) return;
  int iv;
  long long offset;
  if (!lv_iv_index_expr(w->plan, lv_arg(call, "index"), &iv, &offset)) return;
  lv_add_access(w, LV_STRING_AT_IV, call,
                lv_list_slot(w->plan, object, infer_expr_type_ref(w->ctx, object), true), iv, offset);
}

/* A List access worth proving: by an induction variable, or by a proven byte */
static void lv_consider_list_access(LvWalk* w, const AstExpr* call) {
  Str method = call->as.method_call.method_name;
  bool write = str_eq_cstr(method, "set") || str_eq_cstr(method, "modAt");
  bool fast_method = write || str_eq_cstr(method, "copyAtFallback") || str_eq_cstr(method, "copyAtDefault")
      || str_eq_cstr(method, "copyAt") || str_eq_cstr(method, "viewAt");
  if (!fast_method) return;
  const AstExpr* object = call->as.method_call.object;
  if (!lv_is_list(w->ctx, object)) return;
  const AstTypeRef* list_type = infer_expr_type_ref(w->ctx, object);
  if (!c_list_plain_element(w->ctx, list_type)) return;
  const AstExpr* index = lv_arg(call, "index");
  int iv;
  long long offset;
  if (lv_iv_index_expr(w->plan, index, &iv, &offset)) {
    lv_add_access(w, LV_LIST_AT_IV, call, lv_list_slot(w->plan, object, list_type, false), iv, offset);
    return;
  }
  /* a read indexed by a byte (the string read is walked first, so it is
   * already an access when it is provable) */
  if (!write && index && index->kind == AST_EXPR_METHOD_CALL) {
    const LvAccess* byte = lv_find_access(w->plan, index);
    if (byte && byte->kind == LV_STRING_AT_IV)
      lv_add_access(w, LV_LIST_AT_BYTE, call, lv_list_slot(w->plan, object, list_type, false), -1, 0);
  }
}

static void lv_walk_call_args(LvWalk* w, const AstExpr* call, const AstCallArg* args) {
  if (!call->decl_link) w->plan->ok = false;  /* cannot see what it may change */
  for (const AstCallArg* a = args; a; a = a->next) {
    /* anything handed to a `mod` parameter may move a list's storage */
    if (lv_param_is_mod(call, a->name, false)) w->plan->ok = false;
    lv_walk_expr(w, a->value);
  }
}

static void lv_walk_expr(LvWalk* w, const AstExpr* e) {
  if (!e || !w->plan->ok) return;
  switch (e->kind) {
    case AST_EXPR_BINARY:
      lv_walk_expr(w, e->as.binary.lhs);
      lv_walk_expr(w, e->as.binary.rhs);
      break;
    case AST_EXPR_UNARY: {
      Str name;
      if (lv_increment(e, &name) || e->as.unary.op == AST_UNARY_PRE_DEC || e->as.unary.op == AST_UNARY_POST_DEC) {
        Str target = lv_root_name(e->as.unary.operand);
        if (target.len) lv_note_assigned(&w->assigned, target);
        if (target.len && lv_iv_index(w->plan, target) >= 0) w->plan->ok = false;
      }
      if (e->as.unary.op == AST_UNARY_SPAWN) w->plan->ok = false;
      lv_walk_expr(w, e->as.unary.operand);
      break;
    }
    case AST_EXPR_CAST: lv_walk_expr(w, e->as.cast.operand); break;
    case AST_EXPR_MEMBER: lv_walk_expr(w, e->as.member.object); break;
    case AST_EXPR_INDEX:
      lv_walk_expr(w, e->as.index.target);
      lv_walk_expr(w, e->as.index.index);
      break;
    case AST_EXPR_CALL:
      lv_walk_call_args(w, e, e->as.call.args);
      break;
    case AST_EXPR_METHOD_CALL: {
      const AstExpr* object = e->as.method_call.object;
      Str object_base = lv_base(w->ctx, object);
      bool on_list = str_eq_cstr(object_base, "List");
      bool on_string = str_eq_cstr(object_base, "String");
      bool moves = on_list && lv_list_method_moves_storage(e->as.method_call.method_name);
      /* a list method that moves storage, or any other `mod` receiver */
      if (moves || (!on_list && lv_param_is_mod(e, (Str){0}, true))) {
        w->plan->ok = false;  /* (a move directly before ret/break is allowed by the statement walk) */
        return;
      }
      bool list_argument = false;
      for (const AstCallArg* a = e->as.method_call.args; a; a = a->next)
        if (str_eq_cstr(lv_base(w->ctx, a->value), "List")) list_argument = true;
      lv_walk_expr(w, object);
      if (on_list || (on_string && !list_argument)) {
        /* List's own methods that keep storage take no `mod` argument (an
         * index, a fallback, a value), and as generics carry no decl_link; a
         * String's methods reach no list unless one is passed to them */
        for (const AstCallArg* a = e->as.method_call.args; a; a = a->next) lv_walk_expr(w, a->value);
      } else {
        lv_walk_call_args(w, e, e->as.method_call.args);
      }
      if (on_string) lv_consider_string_read(w, e);
      if (on_list) lv_consider_list_access(w, e);
      break;
    }
    case AST_EXPR_INTERP:
      for (const AstInterpPart* p = e->as.interp.parts; p; p = p->next) lv_walk_expr(w, p->value);
      break;
    case AST_EXPR_IDENT: case AST_EXPR_INTEGER: case AST_EXPR_FLOAT: case AST_EXPR_STRING:
    case AST_EXPR_CHAR: case AST_EXPR_BOOL: case AST_EXPR_NONE:
      break;
    default:
      /* object/collection literals, match, box…: not analysed, so not allowed */
      w->plan->ok = false;
      break;
  }
}

/* A statement that moves a list's storage, directly followed by `ret` or
 * `break`: the loop ends right after it, so no proven access runs on the
 * moved storage */
static bool lv_storage_move_then_exit(LvWalk* w, const AstStmt* s) {
  if (s->kind != AST_STMT_EXPR || !s->as.expr_stmt || s->as.expr_stmt->kind != AST_EXPR_METHOD_CALL)
    return false;
  const AstExpr* call = s->as.expr_stmt;
  if (!lv_is_list(w->ctx, call->as.method_call.object)
      || !lv_list_method_moves_storage(call->as.method_call.method_name)) return false;
  const AstStmt* next = s->next;
  if (!next || (next->kind != AST_STMT_RET && next->kind != AST_STMT_BREAK)) return false;
  for (const AstCallArg* a = call->as.method_call.args; a; a = a->next) lv_walk_expr(w, a->value);
  return true;
}

static void lv_walk_block(LvWalk* w, const AstBlock* block);

static void lv_walk_stmt(LvWalk* w, const AstStmt* s) {
  if (!s || !w->plan->ok) return;
  switch (s->kind) {
    case AST_STMT_LET:
      if (lv_iv_index(w->plan, s->as.let_stmt.name) >= 0) { w->plan->ok = false; return; }
      lv_note_assigned(&w->assigned, s->as.let_stmt.name);
      lv_walk_expr(w, s->as.let_stmt.value);
      break;
    case AST_STMT_EXPR:
      if (lv_storage_move_then_exit(w, s)) break;
      lv_walk_expr(w, s->as.expr_stmt);
      break;
    case AST_STMT_ASSIGN: {
      Str target = lv_root_name(s->as.assign_stmt.target);
      if (!target.len) { w->plan->ok = false; return; }
      Str target_base = lv_base(w->ctx, s->as.assign_stmt.target);
      if (str_eq_cstr(target_base, "List")) w->plan->ok = false;
      lv_note_assigned(&w->assigned, target);
      lv_walk_expr(w, s->as.assign_stmt.target);
      lv_walk_expr(w, s->as.assign_stmt.value);
      break;
    }
    case AST_STMT_IF:
      if (s->as.if_stmt.binding) lv_walk_stmt(w, s->as.if_stmt.binding);
      lv_walk_expr(w, s->as.if_stmt.condition);
      lv_walk_block(w, s->as.if_stmt.then_block);
      lv_walk_block(w, s->as.if_stmt.else_block);
      break;
    case AST_STMT_UNSAFE:
      lv_walk_block(w, s->as.unsafe_stmt.block);
      break;
    case AST_STMT_RET:
      for (const AstReturnArg* r = s->as.ret_stmt.values; r; r = r->next) lv_walk_expr(w, r->value);
      break;
    case AST_STMT_BREAK:
      break;
    default:
      /* nested loops, continue, match, defer, destructuring: not analysed */
      w->plan->ok = false;
      break;
  }
}

static void lv_walk_block(LvWalk* w, const AstBlock* block) {
  if (!block) return;
  for (const AstStmt* s = block->first; s; s = s->next) lv_walk_stmt(w, s);
}

/* An expression that keeps its value through the loop: literals, names the
 * body does not assign, `+ - *` of those, and `x.length()` of such a name */
static bool lv_invariant(LvWalk* w, const AstExpr* e) {
  if (!e) return false;
  switch (e->kind) {
    case AST_EXPR_INTEGER: return true;
    case AST_EXPR_IDENT:
      return lv_iv_index(w->plan, e->as.ident) < 0 && !lv_assigned(&w->assigned, e->as.ident);
    case AST_EXPR_BINARY:
      if (e->as.binary.op != AST_BIN_ADD && e->as.binary.op != AST_BIN_SUB
          && e->as.binary.op != AST_BIN_MUL) return false;
      return lv_invariant(w, e->as.binary.lhs) && lv_invariant(w, e->as.binary.rhs);
    case AST_EXPR_METHOD_CALL:
      return str_eq_cstr(e->as.method_call.method_name, "length") && !e->as.method_call.args
          && e->as.method_call.object && e->as.method_call.object->kind == AST_EXPR_IDENT
          && lv_invariant(w, e->as.method_call.object);
    case AST_EXPR_MEMBER:
      return str_eq_cstr(e->as.member.member, "length") && e->as.member.object
          && e->as.member.object->kind == AST_EXPR_IDENT && lv_invariant(w, e->as.member.object);
    default:
      return false;
  }
}

static LoopVersionPlan* lv_plan(CFuncContext* ctx, const AstStmt* stmt) {
  const AstBlock* body = stmt->as.loop_stmt.body;
  const AstExpr* cond = stmt->as.loop_stmt.condition;
  if (!body || !cond || cond->kind != AST_EXPR_BINARY) return NULL;
  if (cond->as.binary.op != AST_BIN_LT && cond->as.binary.op != AST_BIN_LE) return NULL;
  LoopVersionPlan* plan = calloc(1, sizeof *plan);
  if (!plan) return NULL;
  plan->ok = true;
  plan->strict = cond->as.binary.op == AST_BIN_LT;

  /* the induction variables: the loop's ++i, and top-level `v = v + K` */
  Str name;
  if (stmt->as.loop_stmt.increment) {
    if (!lv_increment(stmt->as.loop_stmt.increment, &name)) { free(plan); return NULL; }
    plan->ivs[plan->iv_count++] = (LvIv){ name, 1, LV_AFTER_BODY, 1, stmt->as.loop_stmt.increment->as.unary.operand };
  }
  int pos = 0;
  for (const AstStmt* s = body->first; s; s = s->next, pos++) {
    long long step = 0;
    if (!lv_step_assignment(s, &name, &step)) continue;
    int existing = lv_iv_index(plan, name);
    if (existing >= 0) { plan->ivs[existing].updates++; continue; }
    if (plan->iv_count >= LV_MAX_IVS) break;
    plan->ivs[plan->iv_count++] = (LvIv){ name, step, pos, 1, s->as.assign_stmt.target };
  }
  for (int i = 0; i < plan->iv_count; i++) {
    if (plan->ivs[i].updates != 1 || !lv_is_int(ctx, plan->ivs[i].ident)) {
      plan->ivs[i] = plan->ivs[--plan->iv_count];  /* not a clean induction variable */
      i--;
    }
  }

  /* the condition: p < E, p <= E, p + c < E, p + c <= E */
  const AstExpr* lhs = cond->as.binary.lhs;
  const AstExpr* p_expr = NULL;
  long long c = 0;
  if (lhs->kind == AST_EXPR_IDENT) p_expr = lhs;
  else if (lhs->kind == AST_EXPR_BINARY && lhs->as.binary.op == AST_BIN_ADD) {
    if (lhs->as.binary.lhs->kind == AST_EXPR_IDENT && lv_int_literal(lhs->as.binary.rhs, &c)) p_expr = lhs->as.binary.lhs;
    else if (lhs->as.binary.rhs->kind == AST_EXPR_IDENT && lv_int_literal(lhs->as.binary.lhs, &c)) p_expr = lhs->as.binary.rhs;
  }
  plan->primary = p_expr ? lv_iv_index(plan, p_expr->as.ident) : -1;
  if (plan->primary < 0) { free(plan); return NULL; }
  plan->cond_offset = c;
  plan->bound = cond->as.binary.rhs;

  /* the body: what it assigns, whether anything moves a list, the accesses */
  LvWalk w = { ctx, plan, { {{0}}, 0, false }, 0 };
  for (const AstStmt* s = body->first; s; s = s->next, w.pos++) {
    long long step;
    if (lv_step_assignment(s, &name, &step) && lv_iv_index(plan, name) >= 0) continue;
    lv_walk_stmt(&w, s);
    if (!plan->ok) break;
  }
  for (int i = 0; plan->ok && i < plan->iv_count; i++)
    if (lv_assigned(&w.assigned, plan->ivs[i].name)) plan->ok = false;
  for (int i = 0; plan->ok && i < plan->list_count; i++)
    if (lv_assigned(&w.assigned, plan->lists[i].name)) plan->ok = false;
  if (plan->ok && !lv_invariant(&w, plan->bound)) plan->ok = false;
  if (lv_trace())
    fprintf(stderr, "[loop-versioning] line %zu: %s (%d accesses)\n", stmt->line,
            plan->ok && plan->access_count ? "versioned" : "kept", plan->access_count);
  if (!plan->ok || plan->access_count == 0) { free(plan); return NULL; }
  plan->id = (int)ctx->temp_counter++;
  return plan;
}

/* A pointer to the list (`->length`, `->data`), or the String value (`.len`, `.data`) */
static void lv_emit_holder(CFuncContext* ctx, const LvList* list, FILE* out) {
  if (list->is_string) {
    fprintf(out, "(");
    emit_expr(ctx, list->expr, out, PREC_LOWEST, false, false);
    fprintf(out, ")");
    return;
  }
  bool is_ref = list->type->is_view || list->type->is_mod;
  if (!is_ref) fprintf(out, "(&(");
  emit_expr(ctx, list->expr, out, PREC_LOWEST, false, true);
  if (!is_ref) fprintf(out, "))");
}

static void lv_emit_length(CFuncContext* ctx, const LvList* list, FILE* out) {
  lv_emit_holder(ctx, list, out);
  fprintf(out, list->is_string ? ".len" : "->length");
}

/* The check before the loop. Every IV's value at the access, over the trips
 * the condition allows, must stay inside its list or String; a table read by
 * a byte needs 256 elements. 128-bit arithmetic, so no bound overflows. */
static void lv_emit_check(CFuncContext* ctx, const LoopVersionPlan* plan, FILE* out) {
  int id = plan->id;
  const LvIv* p = &plan->ivs[plan->primary];
  fprintf(out, "  __int128 __rae_vtrips%d = 0;\n  {\n    __int128 __rae_vstart = (__int128)(", id);
  emit_expr(ctx, p->ident, out, PREC_LOWEST, false, false);
  fprintf(out, ") + %lld;\n    __int128 __rae_vbound = (__int128)(", plan->cond_offset);
  emit_expr(ctx, plan->bound, out, PREC_LOWEST, false, false);
  fprintf(out, ");\n");
  if (plan->strict)
    fprintf(out, "    if (__rae_vstart < __rae_vbound) __rae_vtrips%d = (__rae_vbound - 1 - __rae_vstart) / %lld + 1;\n",
            id, p->step);
  else
    fprintf(out, "    if (__rae_vstart <= __rae_vbound) __rae_vtrips%d = (__rae_vbound - __rae_vstart) / %lld + 1;\n",
            id, p->step);
  fprintf(out, "  }\n  int __rae_vfast%d = 1;\n  if (__rae_vtrips%d > 0) {\n", id, id);
  for (int i = 0; i < plan->access_count; i++) {
    const LvAccess* a = &plan->accesses[i];
    const LvList* list = &plan->lists[a->list];
    if (a->kind == LV_LIST_AT_BYTE) {
      fprintf(out, "    if ((__int128)");
      lv_emit_length(ctx, list, out);
      fprintf(out, " < 256) __rae_vfast%d = 0;\n", id);
      continue;
    }
    const LvIv* iv = &plan->ivs[a->iv];
    /* before the update in the body's order the IV is v0 + K*j, after it v0 + K*(j+1) */
    int after = a->pos > iv->update_pos ? 1 : 0;
    fprintf(out, "    { __int128 __rae_v0 = (__int128)(");
    emit_expr(ctx, iv->ident, out, PREC_LOWEST, false, false);
    fprintf(out, ") + %lld; __int128 __rae_vlo = __rae_v0 + (__int128)%lld * %d; "
                 "__int128 __rae_vhi = __rae_v0 + (__int128)%lld * (__rae_vtrips%d - 1 + %d); "
                 "if (__rae_vlo < 0 || __rae_vhi >= (__int128)",
            a->offset, iv->step, after, iv->step, id, after);
    lv_emit_length(ctx, list, out);
    fprintf(out, ") __rae_vfast%d = 0; }\n", id);
  }
  for (int i = 0; i < plan->list_count; i++) {
    if (!plan->lists[i].is_string) continue;
    fprintf(out, "    if (!");
    lv_emit_holder(ctx, &plan->lists[i], out);
    fprintf(out, ".data) __rae_vfast%d = 0;\n", id);
  }
  fprintf(out, "  }\n");
}

/* Emit `stmt` (a non-range loop, init already emitted) through `emit_for`:
 * versioned when its accesses can be proven; false (nothing emitted) otherwise */
bool c_loop_version_emit(CFuncContext* ctx, const AstStmt* stmt, FILE* out,
                         void (*emit_for)(CFuncContext*, const AstStmt*, FILE*)) {
  if (stmt->as.loop_stmt.is_range || stmt->as.loop_stmt.is_parallel || stmt->as.loop_stmt.is_main_loop
      || ctx->loop_version_fast || ctx->parallel_depth > 0 || getenv("RAE_NO_LOOP_VERSIONING")) return false;
  LoopVersionPlan* plan = lv_plan(ctx, stmt);
  if (!plan) return false;
  fprintf(out, "  {\n");
  lv_emit_check(ctx, plan, out);
  fprintf(out, "  if (__rae_vfast%d) {\n", plan->id);
  for (int i = 0; i < plan->list_count; i++) {
    const LvList* list = &plan->lists[i];
    fprintf(out, "    ");
    if (list->is_string) {
      fprintf(out, "const uint8_t*");
    } else {
      emit_type_ref_as_c_type(ctx, c_list_plain_element(ctx, list->type), out, false);
      fprintf(out, "*");
    }
    fprintf(out, " const __rae_vdata%d_%d = ", plan->id, i);
    lv_emit_holder(ctx, list, out);
    fprintf(out, list->is_string ? ".data;\n" : "->data;\n");
  }
  ctx->loop_version_fast = plan;
  emit_for(ctx, stmt, out);
  ctx->loop_version_fast = NULL;
  fprintf(out, "  } else {\n");
  emit_for(ctx, stmt, out);
  fprintf(out, "  }\n  }\n");
  free(plan);
  return true;
}

/* Inside the check-free copy: the C name of the hoisted data a proven
 * access reads or writes, or NULL when `access` is not one. The name is in
 * one static buffer: copy it before emitting anything else. */
const char* c_loop_version_fast_data(CFuncContext* ctx, const AstExpr* access) {
  const LoopVersionPlan* plan = ctx->loop_version_fast;
  if (!plan) return NULL;
  const LvAccess* a = lv_find_access(plan, access);
  if (!a) return NULL;
  static char name[64];
  snprintf(name, sizeof name, "__rae_vdata%d_%d", plan->id, a->list);
  return name;
}
