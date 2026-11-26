/* ast_clone.c - copies of statements and expressions.
 *
 * The switches have no `default`: a new kind of node is a compiler warning here
 * until it is copied. */
#include "ast_clone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c_decl_index.h"
#include "mangler.h"
#include "sema.h"

#define NEW(cloner, type) ((type*)arena_alloc((cloner)->ctx->ast_arena, sizeof(type)))

static const AstTypeRef* argument_of(const AstCloner* cloner, Str name) {
  const AstTypeRef* arg = cloner->args;
  for (const AstIdentifierPart* p = cloner->params; p && arg; p = p->next, arg = arg->next)
    if (str_eq(p->text, name)) return arg;
  return NULL;
}

/* ---- types ---------------------------------------------------------------- */

static AstTypeRef* clone_one_type(const AstCloner* cloner, const AstTypeRef* type) {
  AstTypeRef* copy = NEW(cloner, AstTypeRef);
  const AstTypeRef* arg = type->parts && !type->parts->next && !type->generic_args
      ? argument_of(cloner, type->parts->text) : NULL;
  if (arg) {
    /* `T`, `view T`, `opt T`: the argument, with the modes written here */
    AstCloner written = { .ctx = cloner->ctx };
    *copy = *clone_one_type(&written, arg);
    copy->is_view |= type->is_view;
    copy->is_mod |= type->is_mod;
    copy->is_val |= type->is_val;
    copy->is_own |= type->is_own;
    copy->is_copy |= type->is_copy;
    copy->is_opt |= type->is_opt;
    copy->line = type->line;
    copy->column = type->column;
    /* (an Array's TypeInfo, kept below, is of the plain type) */
    if (type->is_view || type->is_mod || type->is_opt) copy->resolved_type = NULL;
    return copy;
  }
  *copy = *type;
  copy->next = NULL;
  /* An Array's count is in its TypeInfo, not in its name: without it the copy
   * cannot be resolved again, and every `cap` has one C name */
  copy->resolved_type = type->resolved_type && type->resolved_type->kind == TYPE_ARRAY ? type->resolved_type : NULL;
  copy->parts = clone_parts(cloner->ctx, type->parts);
  copy->generic_args = ast_clone_type_ref(cloner, type->generic_args);
  copy->value_expr = ast_clone_expr(cloner, type->value_expr);
  /* `Array(T, cap: 4)`: the template's TypeInfo is one of T */
  if (cloner->params && copy->resolved_type) copy->resolved_type = NULL;
  return copy;
}

AstTypeRef* ast_clone_type_ref(const AstCloner* cloner, const AstTypeRef* type) {
  AstTypeRef* head = NULL;
  AstTypeRef** tail = &head;
  for (; type; type = type->next) {
    *tail = clone_one_type(cloner, type);
    tail = &(*tail)->next;
  }
  return head;
}

/* ---- types named in expressions: `T.default()`, `sizeof(Entry(T))` --------- */

static bool has_optional(const AstTypeRef* type) {
  for (; type; type = type->next)
    if (type->is_opt || has_optional(type->generic_args)) return true;
  return false;
}

/* `arg` as an expression that names the type, as a program writes it: `Int`,
 * `List(Int)`, `Array(Int, cap: 4)` */
static AstExpr* type_as_expr(const AstCloner* cloner, const AstTypeRef* arg, const AstExpr* at) {
  AstExpr* name = NEW(cloner, AstExpr);
  name->kind = AST_EXPR_IDENT;
  name->line = at->line;
  name->column = at->column;
  name->as.ident = arg->parts ? arg->parts->text : (Str){0};
  name->is_written_out = true;
  if (!arg->generic_args) return name;
  AstExpr* call = NEW(cloner, AstExpr);
  call->kind = AST_EXPR_CALL;
  call->line = at->line;
  call->column = at->column;
  call->as.call.callee = name;
  call->is_written_out = true;
  AstCallArg** tail = &call->as.call.args;
  for (const AstTypeRef* inner = arg->generic_args; inner; inner = inner->next) {
    AstCallArg* given = NEW(cloner, AstCallArg);
    if (inner->is_value_arg) {
      given->name = inner->value_name;
      if (inner->value_expr) {
        given->value = ast_clone_expr(cloner, inner->value_expr);
      } else {
        char text[32];
        int length = snprintf(text, sizeof text, "%lld", (long long)inner->value_folded);
        char* digits = arena_alloc(cloner->ctx->ast_arena, (size_t)length + 1);
        memcpy(digits, text, (size_t)length + 1);
        given->value = NEW(cloner, AstExpr);
        given->value->kind = AST_EXPR_INTEGER;
        given->value->line = at->line;
        given->value->column = at->column;
        given->value->as.integer = (Str){ digits, (size_t)length };
      }
    } else {
      given->value = type_as_expr(cloner, inner, at);
    }
    *tail = given;
    tail = &given->next;
  }
  return call;
}

/* ---- expressions ----------------------------------------------------------- */

static AstCallArg* clone_args(const AstCloner* cloner, const AstCallArg* arg) {
  AstCallArg* head = NULL;
  AstCallArg** tail = &head;
  for (; arg; arg = arg->next) {
    AstCallArg* copy = NEW(cloner, AstCallArg);
    *copy = *arg;
    copy->value = ast_clone_expr(cloner, arg->value);
    copy->next = NULL;
    *tail = copy;
    tail = &copy->next;
  }
  return head;
}

/* The arguments of a call. A leading type argument that is an optional has no
 * spelling as an expression: it and those before it become type arguments. */
static void clone_call_args(const AstCloner* cloner, const AstCallArg* args, AstTypeRef** types, AstCallArg** values) {
  const AstCallArg* rest = args;
  if (!*types) {
    bool optional = false;
    for (; rest && !rest->name.len && rest->value && rest->value->kind == AST_EXPR_IDENT; rest = rest->next) {
      const AstTypeRef* arg = argument_of(cloner, rest->value->as.ident);
      if (!arg) break;
      optional |= arg->is_opt || has_optional(arg->generic_args);
    }
    if (optional) {
      AstCloner written = { .ctx = cloner->ctx };
      AstTypeRef** tail = types;
      for (const AstCallArg* a = args; a != rest; a = a->next) {
        *tail = clone_one_type(&written, argument_of(cloner, a->value->as.ident));
        tail = &(*tail)->next;
      }
    } else {
      rest = args;
    }
  }
  *values = clone_args(cloner, rest);
}

AstExpr* ast_clone_expr(const AstCloner* cloner, const AstExpr* expr) {
  if (!expr) return NULL;
  const AstTypeRef* named = expr->kind == AST_EXPR_IDENT ? argument_of(cloner, expr->as.ident) : NULL;
  /* (an optional has no such spelling: the name stays, the instance's scope has it) */
  if (named && !named->is_opt && !has_optional(named->generic_args)) return type_as_expr(cloner, named, expr);
  AstExpr* copy = NEW(cloner, AstExpr);
  *copy = *expr;
  copy->resolved_type = NULL;
  copy->decl_link = NULL;
  /* `T.default()` of an optional is none */
  if (expr->kind == AST_EXPR_METHOD_CALL && !expr->as.method_call.args && expr->as.method_call.object
      && expr->as.method_call.object->kind == AST_EXPR_IDENT && str_eq_cstr(expr->as.method_call.method_name, "default")) {
    const AstTypeRef* arg = argument_of(cloner, expr->as.method_call.object->as.ident);
    if (arg && arg->is_opt) {
      copy->kind = AST_EXPR_NONE;
      return copy;
    }
  }
  switch (expr->kind) {
    case AST_EXPR_IDENT: case AST_EXPR_INTEGER: case AST_EXPR_FLOAT: case AST_EXPR_STRING:
    case AST_EXPR_CHAR: case AST_EXPR_BOOL: case AST_EXPR_NONE:
      break;
    case AST_EXPR_BINARY:
      copy->as.binary.lhs = ast_clone_expr(cloner, expr->as.binary.lhs);
      copy->as.binary.rhs = ast_clone_expr(cloner, expr->as.binary.rhs);
      break;
    case AST_EXPR_UNARY: case AST_EXPR_OWN: case AST_EXPR_BOX: case AST_EXPR_UNBOX:
      copy->as.unary.operand = ast_clone_expr(cloner, expr->as.unary.operand);
      break;
    case AST_EXPR_CAST:
      copy->as.cast.operand = ast_clone_expr(cloner, expr->as.cast.operand);
      copy->as.cast.target = ast_clone_type_ref(cloner, expr->as.cast.target);
      break;
    case AST_EXPR_CALL:
      copy->as.call.callee = ast_clone_expr(cloner, expr->as.call.callee);
      copy->as.call.generic_args = ast_clone_type_ref(cloner, expr->as.call.generic_args);
      clone_call_args(cloner, expr->as.call.args, &copy->as.call.generic_args, &copy->as.call.args);
      break;
    case AST_EXPR_METHOD_CALL:
      copy->as.method_call.object = ast_clone_expr(cloner, expr->as.method_call.object);
      copy->as.method_call.generic_args = ast_clone_type_ref(cloner, expr->as.method_call.generic_args);
      clone_call_args(cloner, expr->as.method_call.args, &copy->as.method_call.generic_args, &copy->as.method_call.args);
      break;
    case AST_EXPR_MEMBER:
      copy->as.member.object = ast_clone_expr(cloner, expr->as.member.object);
      break;
    case AST_EXPR_INDEX:
      copy->as.index.target = ast_clone_expr(cloner, expr->as.index.target);
      copy->as.index.index = ast_clone_expr(cloner, expr->as.index.index);
      break;
    case AST_EXPR_OBJECT: {
      copy->as.object_literal.type = ast_clone_type_ref(cloner, expr->as.object_literal.type);
      AstObjectField** tail = &copy->as.object_literal.fields;
      for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
        AstObjectField* field = NEW(cloner, AstObjectField);
        *field = *f;
        field->value = ast_clone_expr(cloner, f->value);
        *tail = field;
        tail = &field->next;
      }
      break;
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      copy->as.collection.type = ast_clone_type_ref(cloner, expr->as.collection.type);
      AstCollectionElement** tail = &copy->as.collection.elements;
      *tail = NULL;
      for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) {
        AstCollectionElement* element = NEW(cloner, AstCollectionElement);
        *element = *e;
        if (e->key) {
          element->key = NEW(cloner, Str);
          *element->key = *e->key;
        }
        element->value = ast_clone_expr(cloner, e->value);
        *tail = element;
        tail = &element->next;
      }
      break;
    }
    case AST_EXPR_LIST: {
      AstExprList** tail = &copy->as.list;
      *tail = NULL;
      for (const AstExprList* l = expr->as.list; l; l = l->next) {
        AstExprList* item = NEW(cloner, AstExprList);
        item->value = ast_clone_expr(cloner, l->value);
        *tail = item;
        tail = &item->next;
      }
      break;
    }
    case AST_EXPR_INTERP: {
      AstInterpPart** tail = &copy->as.interp.parts;
      *tail = NULL;
      for (const AstInterpPart* p = expr->as.interp.parts; p; p = p->next) {
        AstInterpPart* part = NEW(cloner, AstInterpPart);
        part->value = ast_clone_expr(cloner, p->value);
        *tail = part;
        tail = &part->next;
      }
      break;
    }
    case AST_EXPR_MATCH: {
      copy->as.match_expr.subject = ast_clone_expr(cloner, expr->as.match_expr.subject);
      AstMatchArm** tail = &copy->as.match_expr.arms;
      *tail = NULL;
      for (const AstMatchArm* a = expr->as.match_expr.arms; a; a = a->next) {
        AstMatchArm* arm = NEW(cloner, AstMatchArm);
        arm->pattern = ast_clone_expr(cloner, a->pattern);
        arm->value = ast_clone_expr(cloner, a->value);
        *tail = arm;
        tail = &arm->next;
      }
      break;
    }
  }
  return copy;
}

/* ---- statements ------------------------------------------------------------- */

static AstStmt* clone_stmt(const AstCloner* cloner, const AstStmt* stmt) {
  if (!stmt) return NULL;
  AstStmt* copy = NEW(cloner, AstStmt);
  *copy = *stmt;
  copy->next = NULL;
  switch (stmt->kind) {
    case AST_STMT_BREAK: case AST_STMT_CONTINUE:
      break;
    case AST_STMT_LET:
      copy->as.let_stmt.type = ast_clone_type_ref(cloner, stmt->as.let_stmt.type);
      copy->as.let_stmt.value = ast_clone_expr(cloner, stmt->as.let_stmt.value);
      break;
    case AST_STMT_DESTRUCT: {
      AstDestructureBinding** tail = &copy->as.destruct_stmt.bindings;
      *tail = NULL;
      for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next) {
        AstDestructureBinding* binding = NEW(cloner, AstDestructureBinding);
        *binding = *b;
        binding->next = NULL;
        *tail = binding;
        tail = &binding->next;
      }
      copy->as.destruct_stmt.call = ast_clone_expr(cloner, stmt->as.destruct_stmt.call);
      break;
    }
    case AST_STMT_EXPR:
      copy->as.expr_stmt = ast_clone_expr(cloner, stmt->as.expr_stmt);
      break;
    case AST_STMT_RET: {
      AstReturnArg** tail = &copy->as.ret_stmt.values;
      *tail = NULL;
      for (const AstReturnArg* v = stmt->as.ret_stmt.values; v; v = v->next) {
        AstReturnArg* value = NEW(cloner, AstReturnArg);
        *value = *v;
        value->value = ast_clone_expr(cloner, v->value);
        value->next = NULL;
        *tail = value;
        tail = &value->next;
      }
      break;
    }
    case AST_STMT_IF:
      copy->as.if_stmt.binding = clone_stmt(cloner, stmt->as.if_stmt.binding);
      copy->as.if_stmt.condition = ast_clone_expr(cloner, stmt->as.if_stmt.condition);
      copy->as.if_stmt.then_block = ast_clone_block(cloner, stmt->as.if_stmt.then_block);
      copy->as.if_stmt.else_block = ast_clone_block(cloner, stmt->as.if_stmt.else_block);
      break;
    case AST_STMT_LOOP: {
      copy->as.loop_stmt.init = clone_stmt(cloner, stmt->as.loop_stmt.init);
      copy->as.loop_stmt.condition = ast_clone_expr(cloner, stmt->as.loop_stmt.condition);
      copy->as.loop_stmt.increment = ast_clone_expr(cloner, stmt->as.loop_stmt.increment);
      copy->as.loop_stmt.step_target = ast_clone_expr(cloner, stmt->as.loop_stmt.step_target);
      copy->as.loop_stmt.query_iterable = ast_clone_expr(cloner, stmt->as.loop_stmt.query_iterable);
      AstQueryLoopBinding** tail = &copy->as.loop_stmt.query_bindings;
      *tail = NULL;
      for (const AstQueryLoopBinding* b = stmt->as.loop_stmt.query_bindings; b; b = b->next) {
        AstQueryLoopBinding* binding = NEW(cloner, AstQueryLoopBinding);
        binding->name = b->name;
        binding->type = ast_clone_type_ref(cloner, b->type);
        *tail = binding;
        tail = &binding->next;
      }
      copy->as.loop_stmt.body = ast_clone_block(cloner, stmt->as.loop_stmt.body);
      break;
    }
    case AST_STMT_MATCH: {
      copy->as.match_stmt.binding = clone_stmt(cloner, stmt->as.match_stmt.binding);
      copy->as.match_stmt.subject = ast_clone_expr(cloner, stmt->as.match_stmt.subject);
      AstMatchCase** tail = &copy->as.match_stmt.cases;
      *tail = NULL;
      for (const AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next) {
        AstMatchCase* arm = NEW(cloner, AstMatchCase);
        arm->pattern = ast_clone_expr(cloner, c->pattern);
        AstCasePattern** pattern_tail = &arm->or_patterns;
        for (const AstCasePattern* p = c->or_patterns; p; p = p->next) {
          AstCasePattern* pattern = NEW(cloner, AstCasePattern);
          pattern->expr = ast_clone_expr(cloner, p->expr);
          *pattern_tail = pattern;
          pattern_tail = &pattern->next;
        }
        arm->block = ast_clone_block(cloner, c->block);
        *tail = arm;
        tail = &arm->next;
      }
      break;
    }
    case AST_STMT_ASSIGN:
      copy->as.assign_stmt.target = ast_clone_expr(cloner, stmt->as.assign_stmt.target);
      copy->as.assign_stmt.value = ast_clone_expr(cloner, stmt->as.assign_stmt.value);
      break;
    case AST_STMT_DEFER:
      copy->as.defer_stmt.block = ast_clone_block(cloner, stmt->as.defer_stmt.block);
      break;
    case AST_STMT_UNSAFE:
      copy->as.unsafe_stmt.block = ast_clone_block(cloner, stmt->as.unsafe_stmt.block);
      break;
    case AST_STMT_WHEN: {
      /* (resolved before sema; a statement `when` has only blocks) */
      AstWhenBranch** tail = &copy->as.when_stmt.branches;
      *tail = NULL;
      for (const AstWhenBranch* b = stmt->as.when_stmt.branches; b; b = b->next) {
        AstWhenBranch* branch = NEW(cloner, AstWhenBranch);
        *branch = *b;
        branch->condition = ast_clone_expr(cloner, b->condition);
        branch->block = ast_clone_block(cloner, b->block);
        branch->next = NULL;
        *tail = branch;
        tail = &branch->next;
      }
      break;
    }
  }
  return copy;
}

AstBlock* ast_clone_block(const AstCloner* cloner, const AstBlock* block) {
  if (!block) return NULL;
  AstBlock* copy = NEW(cloner, AstBlock);
  copy->end_line = block->end_line;
  AstStmt** tail = &copy->first;
  for (const AstStmt* s = block->first; s; s = s->next) {
    *tail = clone_stmt(cloner, s);
    tail = &(*tail)->next;
  }
  return copy;
}

/* ---- the check --------------------------------------------------------------- */

typedef struct {
  const AstCloner* cloner;
  PointerSet* source;  /* the template's nodes */
  bool collect;        /* of the template: fill `source`; of the copy: compare */
  const char* shared;  /* what kind of node of the template the copy has */
  Str named;           /* a type parameter the copy names */
  size_t line;
} CloneCheck;

static void see(CloneCheck* check, const void* node, const char* kind, size_t line) {
  if (check->collect) {
    pointer_set_add(check->source, node);
  } else if (!check->shared && pointer_set_contains(check->source, node)) {
    check->shared = kind;
    check->line = line;
  }
}

static void see_name(CloneCheck* check, Str name, size_t line) {
  const AstTypeRef* arg = check->collect ? NULL : argument_of(check->cloner, name);
  /* (an instance whose argument has the parameter's name: `T` for a type `T`) */
  if (!arg || check->named.len || (arg->parts && str_eq(arg->parts->text, name))) return;
  check->named = name;
  check->line = line;
}

static void check_expr(CloneCheck* check, const AstExpr* expr);
static void check_block(CloneCheck* check, const AstBlock* block);

static void check_type(CloneCheck* check, const AstTypeRef* type) {
  for (; type; type = type->next) {
    see(check, type, "type", type->line);
    if (type->parts && !type->parts->next && !type->generic_args) see_name(check, type->parts->text, type->line);
    for (const AstIdentifierPart* p = type->parts; p; p = p->next) see(check, p, "type name", type->line);
    check_type(check, type->generic_args);
    check_expr(check, type->value_expr);
  }
}

static void check_args(CloneCheck* check, const AstCallArg* args) {
  for (; args; args = args->next) {
    see(check, args, "argument", args->value ? args->value->line : 0);
    check_expr(check, args->value);
  }
}

static void check_expr(CloneCheck* check, const AstExpr* expr) {
  if (!expr) return;
  see(check, expr, "expression", expr->line);
  switch (expr->kind) {
    case AST_EXPR_IDENT: {
      const AstTypeRef* arg = check->collect ? NULL : argument_of(check->cloner, expr->as.ident);
      if (arg && !arg->is_opt && !has_optional(arg->generic_args)) see_name(check, expr->as.ident, expr->line);
      break;
    }
    case AST_EXPR_INTEGER: case AST_EXPR_FLOAT: case AST_EXPR_STRING:
    case AST_EXPR_CHAR: case AST_EXPR_BOOL: case AST_EXPR_NONE:
      break;
    case AST_EXPR_BINARY:
      check_expr(check, expr->as.binary.lhs);
      check_expr(check, expr->as.binary.rhs);
      break;
    case AST_EXPR_UNARY: case AST_EXPR_OWN: case AST_EXPR_BOX: case AST_EXPR_UNBOX:
      check_expr(check, expr->as.unary.operand);
      break;
    case AST_EXPR_CAST:
      check_expr(check, expr->as.cast.operand);
      check_type(check, expr->as.cast.target);
      break;
    case AST_EXPR_CALL:
      check_expr(check, expr->as.call.callee);
      check_args(check, expr->as.call.args);
      check_type(check, expr->as.call.generic_args);
      break;
    case AST_EXPR_METHOD_CALL:
      check_expr(check, expr->as.method_call.object);
      check_args(check, expr->as.method_call.args);
      check_type(check, expr->as.method_call.generic_args);
      break;
    case AST_EXPR_MEMBER:
      check_expr(check, expr->as.member.object);
      break;
    case AST_EXPR_INDEX:
      check_expr(check, expr->as.index.target);
      check_expr(check, expr->as.index.index);
      break;
    case AST_EXPR_OBJECT:
      check_type(check, expr->as.object_literal.type);
      for (const AstObjectField* f = expr->as.object_literal.fields; f; f = f->next) {
        see(check, f, "field", expr->line);
        check_expr(check, f->value);
      }
      break;
    case AST_EXPR_COLLECTION_LITERAL:
      check_type(check, expr->as.collection.type);
      for (const AstCollectionElement* e = expr->as.collection.elements; e; e = e->next) {
        see(check, e, "element", expr->line);
        if (e->key) see(check, e->key, "key", expr->line);
        check_expr(check, e->value);
      }
      break;
    case AST_EXPR_LIST:
      for (const AstExprList* l = expr->as.list; l; l = l->next) {
        see(check, l, "list item", expr->line);
        check_expr(check, l->value);
      }
      break;
    case AST_EXPR_INTERP:
      for (const AstInterpPart* p = expr->as.interp.parts; p; p = p->next) {
        see(check, p, "text part", expr->line);
        check_expr(check, p->value);
      }
      break;
    case AST_EXPR_MATCH:
      check_expr(check, expr->as.match_expr.subject);
      for (const AstMatchArm* a = expr->as.match_expr.arms; a; a = a->next) {
        see(check, a, "match arm", expr->line);
        check_expr(check, a->pattern);
        check_expr(check, a->value);
      }
      break;
  }
}

static void check_stmt(CloneCheck* check, const AstStmt* stmt) {
  if (!stmt) return;
  see(check, stmt, "statement", stmt->line);
  switch (stmt->kind) {
    case AST_STMT_BREAK: case AST_STMT_CONTINUE:
      break;
    case AST_STMT_LET:
      check_type(check, stmt->as.let_stmt.type);
      check_expr(check, stmt->as.let_stmt.value);
      break;
    case AST_STMT_DESTRUCT:
      for (const AstDestructureBinding* b = stmt->as.destruct_stmt.bindings; b; b = b->next)
        see(check, b, "binding", stmt->line);
      check_expr(check, stmt->as.destruct_stmt.call);
      break;
    case AST_STMT_EXPR:
      check_expr(check, stmt->as.expr_stmt);
      break;
    case AST_STMT_RET:
      for (const AstReturnArg* v = stmt->as.ret_stmt.values; v; v = v->next) {
        see(check, v, "returned value", stmt->line);
        check_expr(check, v->value);
      }
      break;
    case AST_STMT_IF:
      check_stmt(check, stmt->as.if_stmt.binding);
      check_expr(check, stmt->as.if_stmt.condition);
      check_block(check, stmt->as.if_stmt.then_block);
      check_block(check, stmt->as.if_stmt.else_block);
      break;
    case AST_STMT_LOOP:
      check_stmt(check, stmt->as.loop_stmt.init);
      check_expr(check, stmt->as.loop_stmt.condition);
      check_expr(check, stmt->as.loop_stmt.increment);
      check_expr(check, stmt->as.loop_stmt.step_target);
      check_expr(check, stmt->as.loop_stmt.query_iterable);
      for (const AstQueryLoopBinding* b = stmt->as.loop_stmt.query_bindings; b; b = b->next) {
        see(check, b, "query binding", stmt->line);
        check_type(check, b->type);
      }
      check_block(check, stmt->as.loop_stmt.body);
      break;
    case AST_STMT_MATCH:
      check_stmt(check, stmt->as.match_stmt.binding);
      check_expr(check, stmt->as.match_stmt.subject);
      for (const AstMatchCase* c = stmt->as.match_stmt.cases; c; c = c->next) {
        see(check, c, "case", stmt->line);
        check_expr(check, c->pattern);
        for (const AstCasePattern* p = c->or_patterns; p; p = p->next) {
          see(check, p, "pattern", stmt->line);
          check_expr(check, p->expr);
        }
        check_block(check, c->block);
      }
      break;
    case AST_STMT_ASSIGN:
      check_expr(check, stmt->as.assign_stmt.target);
      check_expr(check, stmt->as.assign_stmt.value);
      break;
    case AST_STMT_DEFER:
      check_block(check, stmt->as.defer_stmt.block);
      break;
    case AST_STMT_UNSAFE:
      check_block(check, stmt->as.unsafe_stmt.block);
      break;
    case AST_STMT_WHEN:
      for (const AstWhenBranch* b = stmt->as.when_stmt.branches; b; b = b->next) {
        see(check, b, "when branch", b->line);
        check_expr(check, b->condition);
        check_block(check, b->block);
      }
      break;
  }
}

static void check_block(CloneCheck* check, const AstBlock* block) {
  if (!block) return;
  see(check, block, "block", block->first ? block->first->line : 0);
  for (const AstStmt* s = block->first; s; s = s->next) check_stmt(check, s);
}

void ast_clone_check(const AstCloner* cloner, const AstBlock* source, const AstBlock* copy, Str name) {
  static int wanted = -1;
  if (wanted < 0) {
    const char* value = getenv("RAE_CHECK_CLONES");
    wanted = value && *value && strcmp(value, "0") != 0;
  }
  if (!wanted || !source || !copy) return;
  CloneCheck check = { .cloner = cloner, .source = pointer_set_create(), .collect = true };
  check_block(&check, source);
  check.collect = false;
  check_block(&check, copy);
  pointer_set_free(check.source);
  if (!check.shared && !check.named.len) return;
  if (check.shared) {
    fprintf(stderr, "internal error: the copy of '%.*s' shares a node with it: %s of line %zu (ast_clone.c)\n",
            (int)name.len, name.data, check.shared, check.line);
  } else {
    fprintf(stderr, "internal error: the instance of '%.*s' names its type parameter '%.*s' in line %zu (ast_clone.c)\n",
            (int)name.len, name.data, (int)check.named.len, check.named.data, check.line);
  }
  abort();
}
