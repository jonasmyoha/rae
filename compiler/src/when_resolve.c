/* Compile-time `when` (docs/platform-conditional-code.md §3.2-§3.3).
 *
 * Runs on the merged program after parsing and before sema. Every `when`
 * condition is evaluated here, and only the selected branch is kept:
 *
 *   - a declaration-level `when` is replaced in the declaration list by the
 *     selected branch's declarations (none when no branch is selected), so
 *     a declaration in another branch does not exist for sema, and naming
 *     it is the ordinary unknown-name error;
 *   - a statement-level `when` becomes `if true { <selected block> }`, the
 *     same lowering `taskScope` uses, so it keeps its own scope.
 *
 * Sema, the backend and `rae build --report-waits` therefore only ever see
 * the selected code. `rae format` works on the parsed tree and prints every
 * branch.
 *
 * A condition is a compile-time Bool: `true` / `false`, a module-level
 * `const`, an enum case, `is`, `is not`, `and`, `or`, `not`. Anything else is
 * an error that names the part that is not compile-time. A const may come
 * from another `when`, so declaration-level conditions are resolved until
 * nothing changes. */
#include "when_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"

typedef enum { VALUE_UNKNOWN, VALUE_BOOL, VALUE_ENUM, VALUE_ERROR, VALUE_MISSING } ValueKind;

typedef struct {
  ValueKind kind;
  bool b;
  Str enum_type;
  Str enum_member;
  /* VALUE_ERROR / VALUE_MISSING: what is wrong, and where */
  char message[320];
  size_t line;
  size_t column;
} WhenValue;

typedef struct {
  Arena* arena;
  AstModule* merged;
  const char* file;          /* the file the condition is written in */
  bool had_error;
  int depth;
} WhenResolver;

static WhenValue value_error(const AstExpr* at, ValueKind kind, const char* format, Str name) {
  WhenValue value = { .kind = kind };
  if (at) {
    value.line = at->line;
    value.column = at->column;
  }
  snprintf(value.message, sizeof value.message, format, (int)name.len, name.data);
  return value;
}

static bool str_is(Str a, const char* b) {
  size_t length = strlen(b);
  return a.len == length && memcmp(a.data, b, length) == 0;
}

/* Does `module_name` answer to the qualifier `Name` (the module itself, or
 * the last component of a package path: `net/Poller` answers to `Poller`)? */
static bool module_matches(const char* module_name, Str qualifier) {
  if (!module_name) return false;
  if (str_is(qualifier, module_name)) return true;
  const char* slash = strrchr(module_name, '/');
  return slash && str_is(qualifier, slash + 1);
}

static WhenValue evaluate(WhenResolver* resolver, const AstExpr* expr);

static WhenValue evaluate_const_decl(WhenResolver* resolver, const AstDecl* decl, const AstExpr* at) {
  if (!decl->as.let_decl.is_const) {
    return value_error(at, VALUE_ERROR, "'%.*s' is not a const, so it is not known at compile time", decl->as.let_decl.name);
  }
  if (resolver->depth > 32) return value_error(at, VALUE_ERROR, "'%.*s' is defined in a cycle", decl->as.let_decl.name);
  const char* saved = resolver->file;
  resolver->file = decl->origin_file;
  resolver->depth++;
  WhenValue value = evaluate(resolver, decl->as.let_decl.value);
  resolver->depth--;
  resolver->file = saved;
  return value;
}

/* A bare name: a module-level const of this file, or the one visible
 * elsewhere. Target's names are written qualified (`Target.isApple`). */
static WhenValue evaluate_name(WhenResolver* resolver, const AstExpr* expr) {
  Str name = expr->as.ident;
  const AstDecl* same_file = NULL;
  const AstDecl* elsewhere = NULL;
  size_t elsewhere_count = 0;
  bool only_target = false;
  for (const AstDecl* decl = resolver->merged->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC && str_eq(decl->as.func_decl.name, name)) {
      return value_error(expr, VALUE_ERROR, "'%.*s' is a function; a `when` condition cannot call or name one", name);
    }
    if (decl->kind != AST_DECL_GLOBAL_LET || !str_eq(decl->as.let_decl.name, name)) continue;
    if (resolver->file && decl->origin_file && strcmp(resolver->file, decl->origin_file) == 0) {
      same_file = decl;
    } else if (decl->module_name && strcmp(decl->module_name, "Target") == 0) {
      only_target = true;
    } else {
      elsewhere = decl;
      elsewhere_count++;
    }
  }
  if (same_file) return evaluate_const_decl(resolver, same_file, expr);
  if (elsewhere_count == 1) return evaluate_const_decl(resolver, elsewhere, expr);
  if (elsewhere_count > 1) {
    return value_error(expr, VALUE_ERROR, "'%.*s' is declared in more than one module: qualify it (`Module.%.*s`)", name);
  }
  if (only_target) {
    char message[320];
    snprintf(message, sizeof message, "'%.*s' is in module Target, which is not opened here: write `Target.%.*s`",
             (int)name.len, name.data, (int)name.len, name.data);
    WhenValue value = { .kind = VALUE_ERROR, .line = expr->line, .column = expr->column };
    memcpy(value.message, message, sizeof message);
    return value;
  }
  /* Perhaps declared inside a `when` not resolved yet */
  return value_error(expr, VALUE_MISSING, "'%.*s' is not a known const", name);
}

/* `Enum.case`, or a qualified const `Module.name` */
static WhenValue evaluate_member(WhenResolver* resolver, const AstExpr* expr) {
  const AstExpr* object = expr->as.member.object;
  Str member = expr->as.member.member;
  if (!object || object->kind != AST_EXPR_IDENT) {
    return value_error(expr, VALUE_ERROR, "'.%.*s' of an expression is not known at compile time", member);
  }
  Str qualifier = object->as.ident;
  for (const AstDecl* decl = resolver->merged->decls; decl; decl = decl->next) {
    if (decl->kind != AST_DECL_ENUM || !str_eq(decl->as.enum_decl.name, qualifier)) continue;
    for (const AstEnumMember* m = decl->as.enum_decl.members; m; m = m->next) {
      if (str_eq(m->name, member)) {
        return (WhenValue){ .kind = VALUE_ENUM, .enum_type = qualifier, .enum_member = member };
      }
    }
    char message[320];
    snprintf(message, sizeof message, "enum %.*s has no case '%.*s'", (int)qualifier.len, qualifier.data,
             (int)member.len, member.data);
    WhenValue value = { .kind = VALUE_ERROR, .line = expr->line, .column = expr->column };
    memcpy(value.message, message, sizeof message);
    return value;
  }
  for (const AstDecl* decl = resolver->merged->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_GLOBAL_LET && str_eq(decl->as.let_decl.name, member) &&
        module_matches(decl->module_name, qualifier)) {
      return evaluate_const_decl(resolver, decl, expr);
    }
  }
  char message[320];
  snprintf(message, sizeof message, "'%.*s.%.*s' is not a known const or enum case", (int)qualifier.len, qualifier.data,
           (int)member.len, member.data);
  WhenValue value = { .kind = VALUE_MISSING, .line = expr->line, .column = expr->column };
  memcpy(value.message, message, sizeof message);
  return value;
}

static const char* binary_spelling(AstBinaryOp op) {
  switch (op) {
    case AST_BIN_ADD: return "+";
    case AST_BIN_SUB: return "-";
    case AST_BIN_MUL: return "*";
    case AST_BIN_DIV: return "/";
    case AST_BIN_MOD: return "%";
    case AST_BIN_LT: return "<";
    case AST_BIN_GT: return ">";
    case AST_BIN_LE: return "<=";
    case AST_BIN_GE: return ">=";
    default: return "this operator";
  }
}

static WhenValue evaluate(WhenResolver* resolver, const AstExpr* expr) {
  if (!expr) return (WhenValue){ .kind = VALUE_ERROR, .message = "missing condition" };
  switch (expr->kind) {
    case AST_EXPR_BOOL:
      return (WhenValue){ .kind = VALUE_BOOL, .b = expr->as.boolean };
    case AST_EXPR_IDENT:
      return evaluate_name(resolver, expr);
    case AST_EXPR_MEMBER:
      return evaluate_member(resolver, expr);
    case AST_EXPR_UNARY: {
      if (expr->as.unary.op != AST_UNARY_NOT) break;
      WhenValue operand = evaluate(resolver, expr->as.unary.operand);
      if (operand.kind != VALUE_BOOL) {
        if (operand.kind == VALUE_ENUM) return value_error(expr, VALUE_ERROR, "`not` needs a Bool, not an enum case%.*s", (Str){ "", 0 });
        return operand;
      }
      return (WhenValue){ .kind = VALUE_BOOL, .b = !operand.b };
    }
    case AST_EXPR_BINARY: {
      AstBinaryOp op = expr->as.binary.op;
      if (op != AST_BIN_IS && op != AST_BIN_NEQ && op != AST_BIN_AND && op != AST_BIN_OR) {
        char message[320];
        snprintf(message, sizeof message,
                 "`%s` is not allowed in a `when` condition (use `is`, `is not`, `and`, `or`, `not`)", binary_spelling(op));
        WhenValue value = { .kind = VALUE_ERROR, .line = expr->line, .column = expr->column };
        memcpy(value.message, message, sizeof message);
        return value;
      }
      WhenValue left = evaluate(resolver, expr->as.binary.lhs);
      if (left.kind == VALUE_ERROR || left.kind == VALUE_MISSING) return left;
      WhenValue right = evaluate(resolver, expr->as.binary.rhs);
      if (right.kind == VALUE_ERROR || right.kind == VALUE_MISSING) return right;
      if (op == AST_BIN_AND || op == AST_BIN_OR) {
        if (left.kind != VALUE_BOOL || right.kind != VALUE_BOOL) {
          return value_error(expr, VALUE_ERROR, "`and` / `or` need Bools on both sides%.*s", (Str){ "", 0 });
        }
        return (WhenValue){ .kind = VALUE_BOOL, .b = op == AST_BIN_AND ? (left.b && right.b) : (left.b || right.b) };
      }
      bool equal;
      if (left.kind == VALUE_BOOL && right.kind == VALUE_BOOL) {
        equal = left.b == right.b;
      } else if (left.kind == VALUE_ENUM && right.kind == VALUE_ENUM) {
        if (!str_eq(left.enum_type, right.enum_type)) {
          return value_error(expr, VALUE_ERROR, "`is` compares two different enum types%.*s", (Str){ "", 0 });
        }
        equal = str_eq(left.enum_member, right.enum_member);
      } else {
        return value_error(expr, VALUE_ERROR, "`is` needs two Bools or two cases of one enum%.*s", (Str){ "", 0 });
      }
      return (WhenValue){ .kind = VALUE_BOOL, .b = op == AST_BIN_IS ? equal : !equal };
    }
    case AST_EXPR_CALL: {
      Str name = { "", 0 };
      if (expr->as.call.callee && expr->as.call.callee->kind == AST_EXPR_IDENT) name = expr->as.call.callee->as.ident;
      return value_error(expr, VALUE_ERROR, "a call ('%.*s(...)') is not known at compile time", name);
    }
    case AST_EXPR_METHOD_CALL:
      return value_error(expr, VALUE_ERROR, "a call ('.%.*s(...)') is not known at compile time",
                         expr->as.method_call.method_name);
    case AST_EXPR_INTEGER:
    case AST_EXPR_FLOAT:
      return value_error(expr, VALUE_ERROR, "a number is not a Bool%.*s", (Str){ "", 0 });
    case AST_EXPR_STRING:
    case AST_EXPR_INTERP:
      return value_error(expr, VALUE_ERROR, "a String is not a Bool%.*s", (Str){ "", 0 });
    default:
      break;
  }
  return value_error(expr, VALUE_ERROR, "this expression is not known at compile time%.*s", (Str){ "", 0 });
}

static void report(WhenResolver* resolver, const char* file, const WhenValue* value) {
  char message[400];
  snprintf(message, sizeof message, "`when` needs a compile-time Bool: %s", value->message);
  diag_error(file ? file : "<unknown>", (int)value->line, (int)value->column, message);
  resolver->had_error = true;
}

/* Select a branch: its index, -1 when none applies, -2 when a condition is
 * not decided yet (a const it names may still appear), -3 on an error
 * (reported when `final`) */
static int select_branch(WhenResolver* resolver, const AstWhenBranch* branches, const char* file, bool final) {
  int index = 0;
  for (const AstWhenBranch* branch = branches; branch; branch = branch->next, index++) {
    if (!branch->condition) return index;
    resolver->file = file;
    WhenValue value = evaluate(resolver, branch->condition);
    if (value.kind == VALUE_MISSING && !final) return -2;
    if (value.kind == VALUE_ERROR || value.kind == VALUE_MISSING) {
      if (final || value.kind == VALUE_ERROR) report(resolver, file, &value);
      return -3;
    }
    if (value.kind != VALUE_BOOL) {
      WhenValue wrong = { .kind = VALUE_ERROR, .line = branch->condition->line, .column = branch->condition->column };
      snprintf(wrong.message, sizeof wrong.message, "this condition is an enum case, not a Bool (compare it with `is`)");
      report(resolver, file, &wrong);
      return -3;
    }
    if (value.b) return index;
  }
  return -1;
}

static const AstWhenBranch* branch_at(const AstWhenBranch* branches, int index) {
  for (int i = 0; branches && i < index; i++) branches = branches->next;
  return branches;
}

/* The selected declarations inherit the `when`'s provenance (merge stamped
 * only the top-level list) */
static void stamp_like(AstDecl* child, const AstDecl* parent) {
  if (!child->origin_file) child->origin_file = parent->origin_file;
  if (!child->module_name) child->module_name = parent->module_name;
  if (child->kind == AST_DECL_FUNC && !child->as.func_decl.module_name) {
    child->as.func_decl.module_name = child->module_name;
    child->as.func_decl.origin_file = child->origin_file;
  }
}

/* One pass over the declaration list: replace every decidable `when` by
 * its selected declarations. Returns how many it replaced. */
static int resolve_declarations(WhenResolver* resolver, bool final) {
  int replaced = 0;
  AstDecl** link = &resolver->merged->decls;
  while (*link) {
    AstDecl* decl = *link;
    if (decl->kind != AST_DECL_WHEN) {
      link = &decl->next;
      continue;
    }
    int index = select_branch(resolver, decl->as.when_decl.branches, decl->origin_file, final);
    if (index == -2) {
      link = &decl->next;
      continue;
    }
    AstDecl* selected = NULL;
    if (index >= 0) selected = branch_at(decl->as.when_decl.branches, index)->decls;
    AstDecl* after = decl->next;
    if (selected) {
      AstDecl* last = selected;
      for (AstDecl* child = selected; child; child = child->next) {
        stamp_like(child, decl);
        last = child;
      }
      last->next = after;
      *link = selected;  /* the selected declarations are checked again: a nested `when` resolves next */
    } else {
      *link = after;
    }
    replaced++;
  }
  return replaced;
}

/* ----- statements -------------------------------------------------------- */

static void resolve_block(WhenResolver* resolver, AstBlock* block, const char* file);

static void resolve_statement(WhenResolver* resolver, AstStmt* stmt, const char* file) {
  switch (stmt->kind) {
    case AST_STMT_WHEN: {
      int index = select_branch(resolver, stmt->as.when_stmt.branches, file, true);
      AstBlock* selected = NULL;
      if (index >= 0) selected = branch_at(stmt->as.when_stmt.branches, index)->block;
      if (!selected) {
        selected = arena_alloc(resolver->arena, sizeof(AstBlock));
        memset(selected, 0, sizeof(AstBlock));
      }
      AstExpr* always = arena_alloc(resolver->arena, sizeof(AstExpr));
      memset(always, 0, sizeof(AstExpr));
      always->kind = AST_EXPR_BOOL;
      always->as.boolean = true;
      always->line = stmt->line;
      always->column = stmt->column;
      stmt->kind = AST_STMT_IF;
      memset(&stmt->as.if_stmt, 0, sizeof(stmt->as.if_stmt));
      stmt->as.if_stmt.condition = always;
      stmt->as.if_stmt.then_block = selected;
      resolve_block(resolver, selected, file);
      break;
    }
    case AST_STMT_IF:
      if (stmt->as.if_stmt.binding) resolve_statement(resolver, stmt->as.if_stmt.binding, file);
      resolve_block(resolver, stmt->as.if_stmt.then_block, file);
      resolve_block(resolver, stmt->as.if_stmt.else_block, file);
      break;
    case AST_STMT_LOOP:
      if (stmt->as.loop_stmt.init) resolve_statement(resolver, stmt->as.loop_stmt.init, file);
      resolve_block(resolver, stmt->as.loop_stmt.body, file);
      break;
    case AST_STMT_MATCH:
      for (AstMatchCase* match_case = stmt->as.match_stmt.cases; match_case; match_case = match_case->next)
        resolve_block(resolver, match_case->block, file);
      break;
    case AST_STMT_DEFER:
      resolve_block(resolver, stmt->as.defer_stmt.block, file);
      break;
    case AST_STMT_UNSAFE:
      resolve_block(resolver, stmt->as.unsafe_stmt.block, file);
      break;
    default:
      break;
  }
}

static void resolve_block(WhenResolver* resolver, AstBlock* block, const char* file) {
  if (!block) return;
  for (AstStmt* stmt = block->first; stmt; stmt = stmt->next) resolve_statement(resolver, stmt, file);
}

bool when_resolve_module(Arena* arena, AstModule* merged) {
  WhenResolver resolver = { .arena = arena, .merged = merged };
  /* Declarations first, to the fixed point: a `when` may test a const that
   * another `when` declares */
  while (resolve_declarations(&resolver, false) > 0) {
  }
  resolve_declarations(&resolver, true);
  for (AstDecl* decl = merged->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_FUNC) resolve_block(&resolver, decl->as.func_decl.body, decl->origin_file);
  }
  if (resolver.had_error) merged->had_error = true;
  return !resolver.had_error;
}
