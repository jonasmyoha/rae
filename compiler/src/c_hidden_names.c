/* A C name of its own for a local that hides an outer one.
 *
 * A nested block may bind a name again (`let name` in the function, `let name`
 * in an `if`). The C of a `ret`, `break` or `continue` there drops the outer
 * local, and runs the defers that use it, inside the inner C block, where the
 * plain name is the inner local. So the inner one is renamed, `name__h1`,
 * binding and uses together, in the AST: once sema has analyzed the function
 * and before the move check (sema_lifecycle_post_pass), which goes by name
 * too; a generic function's template before its C is emitted. A function that
 * was renamed has nothing hidden left, so a second run changes nothing.
 *
 * rae_shown_name gives a diagnostic the name as it was written. */
#include <stdio.h>
#include <string.h>

#include "c_backend_internal.h"

#define HIDDEN_MARK "__h"
#define MAX_BOUND 1024

typedef struct {
  Str source;  /* as written */
  Str name;    /* as emitted */
} Bound;

typedef struct {
  CompilerContext* ctx;
  Bound bound[MAX_BOUND];
  size_t count;
  int renamed;
} Hider;

Str rae_shown_name(Str name) {
  size_t digits = name.len;
  while (digits > 0 && name.data[digits - 1] >= '0' && name.data[digits - 1] <= '9') digits--;
  size_t mark = sizeof(HIDDEN_MARK) - 1;
  if (digits == name.len || digits <= mark || memcmp(name.data + digits - mark, HIDDEN_MARK, mark) != 0) return name;
  return (Str){ name.data, digits - mark };
}

static const Bound* find_bound(const Hider* h, Str source) {
  for (size_t i = h->count; i > 0; i--) {
    if (str_eq(h->bound[i - 1].source, source)) return &h->bound[i - 1];
  }
  return NULL;
}

static void bind_name(Hider* h, Str* name) {
  if (!name->data || name->len == 0 || h->count >= MAX_BOUND) return;
  Str source = *name;
  if (find_bound(h, source)) {
    size_t cap = source.len + 16;
    char* text = arena_alloc(h->ctx->ast_arena, cap);
    int length = snprintf(text, cap, "%.*s%s%d", (int)source.len, source.data, HIDDEN_MARK, ++h->renamed);
    *name = (Str){ text, (size_t)length };
  }
  h->bound[h->count++] = (Bound){ .source = source, .name = *name };
}

static void walk_block(Hider* h, AstBlock* block);
static void walk_stmt(Hider* h, AstStmt* s);

static void walk_expr(Hider* h, AstExpr* e) {
  if (!e) return;
  switch (e->kind) {
    case AST_EXPR_IDENT: {
      const Bound* bound = find_bound(h, e->as.ident);
      if (bound) e->as.ident = bound->name;
      return;
    }
    case AST_EXPR_BINARY:
      walk_expr(h, e->as.binary.lhs);
      walk_expr(h, e->as.binary.rhs);
      return;
    case AST_EXPR_UNARY: case AST_EXPR_BOX: case AST_EXPR_UNBOX: case AST_EXPR_OWN:
      walk_expr(h, e->as.unary.operand);
      return;
    case AST_EXPR_CAST:
      walk_expr(h, e->as.cast.operand);
      return;
    case AST_EXPR_CALL:
      /* A callee is a function, not a local */
      if (e->as.call.callee && e->as.call.callee->kind != AST_EXPR_IDENT) walk_expr(h, e->as.call.callee);
      for (AstCallArg* arg = e->as.call.args; arg; arg = arg->next) walk_expr(h, arg->value);
      return;
    case AST_EXPR_METHOD_CALL:
      walk_expr(h, e->as.method_call.object);
      for (AstCallArg* arg = e->as.method_call.args; arg; arg = arg->next) walk_expr(h, arg->value);
      return;
    case AST_EXPR_MEMBER:
      walk_expr(h, e->as.member.object);
      return;
    case AST_EXPR_INDEX:
      walk_expr(h, e->as.index.target);
      walk_expr(h, e->as.index.index);
      return;
    case AST_EXPR_OBJECT:
      for (AstObjectField* field = e->as.object_literal.fields; field; field = field->next) walk_expr(h, field->value);
      return;
    case AST_EXPR_LIST:
      for (AstExprList* item = e->as.list; item; item = item->next) walk_expr(h, item->value);
      return;
    case AST_EXPR_COLLECTION_LITERAL:
      for (AstCollectionElement* element = e->as.collection.elements; element; element = element->next) {
        walk_expr(h, element->value);
      }
      return;
    case AST_EXPR_INTERP:
      for (AstInterpPart* part = e->as.interp.parts; part; part = part->next) walk_expr(h, part->value);
      return;
    case AST_EXPR_MATCH:
      walk_expr(h, e->as.match_expr.subject);
      for (AstMatchArm* arm = e->as.match_expr.arms; arm; arm = arm->next) {
        walk_expr(h, arm->pattern);
        walk_expr(h, arm->value);
      }
      return;
    default:
      return;
  }
}

static void walk_stmt(Hider* h, AstStmt* s) {
  if (!s) return;
  size_t scope = h->count;
  switch (s->kind) {
    case AST_STMT_LET:
      walk_expr(h, s->as.let_stmt.value);
      bind_name(h, &s->as.let_stmt.name);
      return;
    case AST_STMT_DESTRUCT:
      walk_expr(h, s->as.destruct_stmt.call);
      for (AstDestructureBinding* b = s->as.destruct_stmt.bindings; b; b = b->next) bind_name(h, &b->local_name);
      return;
    case AST_STMT_EXPR:
      walk_expr(h, s->as.expr_stmt);
      return;
    case AST_STMT_RET:
      for (AstReturnArg* arg = s->as.ret_stmt.values; arg; arg = arg->next) walk_expr(h, arg->value);
      return;
    case AST_STMT_ASSIGN:
      walk_expr(h, s->as.assign_stmt.target);
      walk_expr(h, s->as.assign_stmt.value);
      return;
    case AST_STMT_IF:
      /* The binding of an `if let` is there in the condition (its test) and
       * the branch; the `else` sees what it hides */
      walk_stmt(h, s->as.if_stmt.binding);
      walk_expr(h, s->as.if_stmt.condition);
      walk_block(h, s->as.if_stmt.then_block);
      h->count = scope;
      walk_block(h, s->as.if_stmt.else_block);
      return;
    case AST_STMT_LOOP:
      /* A collection is evaluated outside the loop's binding */
      if (s->as.loop_stmt.is_range) walk_expr(h, s->as.loop_stmt.condition);
      walk_stmt(h, s->as.loop_stmt.init);
      if (!s->as.loop_stmt.is_range) walk_expr(h, s->as.loop_stmt.condition);
      walk_expr(h, s->as.loop_stmt.step_target);
      walk_expr(h, s->as.loop_stmt.increment);
      walk_block(h, s->as.loop_stmt.body);
      h->count = scope;
      return;
    case AST_STMT_MATCH:
      walk_stmt(h, s->as.match_stmt.binding);
      walk_expr(h, s->as.match_stmt.subject);
      for (AstMatchCase* c = s->as.match_stmt.cases; c; c = c->next) {
        walk_expr(h, c->pattern);
        for (AstCasePattern* p = c->or_patterns; p; p = p->next) walk_expr(h, p->expr);
        walk_block(h, c->block);
      }
      h->count = scope;
      return;
    case AST_STMT_DEFER:
      walk_block(h, s->as.defer_stmt.block);
      return;
    case AST_STMT_UNSAFE:
      walk_block(h, s->as.unsafe_stmt.block);
      return;
    default:
      return;
  }
}

static void walk_block(Hider* h, AstBlock* block) {
  if (!block) return;
  size_t scope = h->count;
  for (AstStmt* s = block->first; s; s = s->next) walk_stmt(h, s);
  h->count = scope;
}

void c_rename_hidden_names(CompilerContext* ctx, AstFuncDecl* func) {
  /* A specialization shares nodes with its template, and is not what the C is
   * emitted from */
  if (!func->body || func->specialization_args) return;
  Hider h = { .ctx = ctx };
  for (AstParam* p = func->params; p; p = p->next) bind_name(&h, &p->name);
  walk_block(&h, func->body);
}
