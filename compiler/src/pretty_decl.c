/* pretty_decl.c - statements and declarations of the Rae formatter (#916).
 * Parameters, return items, return values, type fields and enum members use
 * the same pp_print_list policy as call arguments and literals. */

#include "pretty_internal.h"

#include <string.h>

/* ---- parameters / returns (shared by func decls) ---- */

/* A function's parameter list: the type parameters (`T: type`) first, then
 * the value parameters — the parser keeps them apart, the source writes one
 * list. */
typedef struct {
  const AstIdentifierPart* generics;
  const AstParam* params;
} ParamList;

static int count_param_list(const ParamList* list) {
  int n = 0;
  for (const AstIdentifierPart* g = list->generics; g; g = g->next) n++;
  for (const AstParam* p = list->params; p; p = p->next) n++;
  return n;
}

static void params_oneline(PrettyPrinter* pp, const void* items) {
  const ParamList* list = items;
  bool first = true;
  for (const AstIdentifierPart* g = list->generics; g; g = g->next) {
    if (!first) pp_write(pp, ", ");
    pp_write_str(pp, g->text);
    pp_write(pp, ": type");
    first = false;
  }
  const AstParam* current = list->params;
  while (current) {
    if (!first) pp_write(pp, ", ");
    pp_write_str(pp, current->name);
    pp_write(pp, ": ");
    pp_write_type(pp, current->type);
    first = false;
    current = current->next;
  }
}

static void params_vertical(PrettyPrinter* pp, const void* items) {
  const ParamList* list = items;
  for (const AstIdentifierPart* g = list->generics; g; g = g->next) {
    pp_write_str(pp, g->text);
    pp_write(pp, ": type");
    pp_newline(pp);
  }
  const AstParam* current = list->params;
  while (current) {
    if (current->type) pp_flush_comments_before(pp, current->type->line);
    size_t lines_before = pp->lines_written;
    pp_write_str(pp, current->name);
    pp_write(pp, ": ");
    pp_write_type(pp, current->type);
    if (current->type) pp_flush_trailing_comment(pp, current->type->line, lines_before);
    pp_newline(pp);
    current = current->next;
  }
}

static void return_items_oneline(PrettyPrinter* pp, const void* items) {
  const AstReturnItem* current = items;
  bool first = true;
  while (current) {
    if (!first) pp_write(pp, ", ");
    if (current->has_name) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
    }
    pp_write_type(pp, current->type);
    first = false;
    current = current->next;
  }
}

static void return_items_vertical(PrettyPrinter* pp, const void* items) {
  const AstReturnItem* current = items;
  while (current) {
    if (current->has_name) {
      pp_write_str(pp, current->name);
      pp_write(pp, ": ");
    }
    pp_write_type(pp, current->type);
    pp_newline(pp);
    current = current->next;
  }
}

static void return_args_oneline(PrettyPrinter* pp, const void* items) {
  const AstReturnArg* current = items;
  bool first = true;
  while (current) {
    if (!first) pp_write(pp, ", ");
    if (current->has_label) {
      pp_write_str(pp, current->label);
      pp_write(pp, ": ");
    }
    pp_expr(pp, current->value);
    first = false;
    current = current->next;
  }
}

static void return_args_vertical(PrettyPrinter* pp, const void* items) {
  const AstReturnArg* current = items;
  while (current) {
    if (current->value) pp_flush_comments_before(pp, current->value->line);
    size_t lines_before = pp->lines_written;
    if (current->has_label) {
      pp_write_str(pp, current->label);
      pp_write(pp, ": ");
    }
    pp_expr(pp, current->value);
    if (current->value) pp_flush_trailing_comment(pp, current->value->line, lines_before);
    pp_newline(pp);
    current = current->next;
  }
}

static void type_fields_vertical(PrettyPrinter* pp, const void* items) {
  const AstTypeField* field = items;
  while (field) {
    if (field->type) pp_flush_comments_before(pp, field->type->line);
    size_t lines_before = pp->lines_written;
    pp_write_str(pp, field->name);
    pp_write(pp, ": ");
    pp_write_type(pp, field->type);
    if (field->default_value) {
      pp_write(pp, " = ");
      pp_expr(pp, field->default_value);
    }
    if (field->type) pp_flush_trailing_comment(pp, field->type->line, lines_before);
    pp_newline(pp);
    field = field->next;
  }
}

static void enum_members_oneline(PrettyPrinter* pp, const void* items) {
  const AstEnumMember* member = items;
  bool first = true;
  while (member) {
    if (!first) pp_write(pp, ", ");
    pp_write_str(pp, member->name);
    first = false;
    member = member->next;
  }
}

static void enum_members_vertical(PrettyPrinter* pp, const void* items) {
  const AstEnumMember* member = items;
  while (member) {
    pp_write_str(pp, member->name);
    pp_newline(pp);
    member = member->next;
  }
}

static void pp_param_list(PrettyPrinter* pp, const AstIdentifierPart* generics, const AstParam* params,
                          int tail_width) {
  ParamList list = { generics, params };
  PpListStyle style = { PP_WRAP_ARGS, "(", ")", false, tail_width, NULL, false };
  pp_print_list(pp, &list, count_param_list(&list), style, params_oneline, params_vertical);
}

/* ---- statements ---- */

static void pp_print_stmt(PrettyPrinter* pp, const AstStmt* stmt);

void pp_print_block_body(PrettyPrinter* pp, const AstBlock* block) {
  const AstStmt* current = block ? block->first : NULL;
  while (current) {
    pp_print_stmt(pp, current);
    current = current->next;
  }
}

/* Comments and the (at most one) blank line the source had before the
 * statement, then the statement is no longer the first in its block. */
static size_t pp_stmt_prologue(PrettyPrinter* pp, size_t line) {
  pp_flush_comments_before(pp, line);
  pp_blank_line_before(pp, line);
  pp->block_start = false;
  pp->stmt_line = line;
  pp->stmt_indent = pp->indent;
  return pp->lines_written;
}

/* A statement ends with its trailing comment (if any) and a newline. */
static void pp_stmt_epilogue(PrettyPrinter* pp, size_t line, size_t lines_before) {
  pp_flush_trailing_comment(pp, line, lines_before);
  if (!pp->start_of_line) pp_newline(pp);
}

static void pp_print_binding(PrettyPrinter* pp, const AstStmt* stmt) {
  if (stmt->as.let_stmt.is_const) {
    pp_write(pp, "const ");
  } else if (stmt->as.let_stmt.is_var) {
    pp_write(pp, "var ");
  } else {
    pp_write(pp, "let ");
  }
  pp_write_str(pp, stmt->as.let_stmt.name);
  if (stmt->as.let_stmt.type) {
    pp_write(pp, ": ");
    pp_write_type(pp, stmt->as.let_stmt.type);
  }
  if (stmt->as.let_stmt.value) {
    pp_write(pp, stmt->as.let_stmt.is_bind ? " => " : " = ");
    pp_expr(pp, stmt->as.let_stmt.value);
  }
}

static void pp_print_let_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  size_t lines_before = pp_stmt_prologue(pp, stmt->line);
  pp_print_binding(pp, stmt);
  pp_stmt_epilogue(pp, stmt->line, lines_before);
}

static void pp_print_destruct_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  size_t lines_before = pp_stmt_prologue(pp, stmt->line);
  const AstDestructureBinding* binding = stmt->as.destruct_stmt.bindings;
  bool first = true;
  while (binding) {
    if (!first) pp_write(pp, ", ");
    pp_write(pp, "let ");
    pp_write_str(pp, binding->local_name);
    pp_write(pp, ": ");
    pp_write_str(pp, binding->return_label);
    first = false;
    binding = binding->next;
  }
  pp_write(pp, " = ");
  pp_expr(pp, stmt->as.destruct_stmt.call);
  pp_stmt_epilogue(pp, stmt->line, lines_before);
}

static void pp_print_return_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  size_t lines_before = pp_stmt_prologue(pp, stmt->line);
  pp_write(pp, "ret");
  const AstReturnArg* arg = stmt->as.ret_stmt.values;
  if (arg) {
    int count = 0;
    for (const AstReturnArg* a = arg; a; a = a->next) count++;
    /* `ret a, b` has no delimiters: the list opens with the space after `ret`
     * and, when vertical, closes with nothing. */
    PpListStyle style = { PP_WRAP_ARGS, " ", "", false, 0, NULL, false };
    style.never_vertical = true;  /* `ret` + newline ends the statement */
    pp_print_list(pp, arg, count, style, return_args_oneline, return_args_vertical);
  }
  pp_stmt_epilogue(pp, stmt->line, lines_before);
}

static void pp_print_if_body(PrettyPrinter* pp, const AstStmt* stmt);

static void pp_print_if_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_stmt_prologue(pp, stmt->line);
  pp_print_if_body(pp, stmt);
}

static void pp_print_if_body(PrettyPrinter* pp, const AstStmt* stmt) {
  if (stmt->as.if_stmt.is_task_scope) {
    pp_write(pp, "taskScope ");
  } else {
    pp_write(pp, "if ");
    if (stmt->as.if_stmt.binding) {
      /* `if let name: T = expr` / `if let name: view T => place`. The parser
       * marks the reference form's type `opt` for its own null test; the
       * source never spells it, so it is not printed. */
      const AstStmt* bind = stmt->as.if_stmt.binding;
      pp_write(pp, "let ");
      pp_write_str(pp, bind->as.let_stmt.name);
      pp_write(pp, ": ");
      AstTypeRef shown = *bind->as.let_stmt.type;
      if (bind->as.let_stmt.is_bind) shown.is_opt = false;
      pp_write_type(pp, &shown);
      pp_write(pp, bind->as.let_stmt.is_bind ? " => " : " = ");
      pp_expr(pp, bind->as.let_stmt.value);
    } else {
      pp_expr(pp, stmt->as.if_stmt.condition);
    }
    pp_space(pp);
  }
  pp_begin_block(pp);
  pp_print_block_body(pp, stmt->as.if_stmt.then_block);
  pp_end_block(pp, stmt->as.if_stmt.then_block ? stmt->as.if_stmt.then_block->end_line : 0);
  const AstBlock* else_block = stmt->as.if_stmt.else_block;
  if (else_block) {
    pp_write(pp, " else ");
    /* `else if`: the parser wraps the nested if in a synthesised block. */
    if (else_block->end_line == 0 && else_block->first && else_block->first->kind == AST_STMT_IF &&
        !else_block->first->next) {
      pp_print_if_body(pp, else_block->first);
      return;
    }
    pp_begin_block(pp);
    pp_print_block_body(pp, else_block);
    pp_end_block(pp, else_block->end_line);
  }
  pp_newline(pp);
}

static void pp_print_match_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_stmt_prologue(pp, stmt->line);
  pp_write(pp, "match ");
  pp_expr(pp, stmt->as.match_stmt.subject);
  pp_space(pp);
  pp_begin_block(pp);
  const AstMatchCase* current = stmt->as.match_stmt.cases;
  while (current) {
    if (current->pattern) {
      pp_stmt_prologue(pp, current->pattern->line);
      pp_write(pp, "case ");
      pp_expr(pp, current->pattern);
      for (const AstCasePattern* op = current->or_patterns; op; op = op->next) {
        pp_write(pp, ", ");
        pp_expr(pp, op->expr);
      }
    } else {
      pp->block_start = false;
      pp_write(pp, "default");
    }
    pp_space(pp);
    pp_begin_block(pp);
    pp_print_block_body(pp, current->block);
    pp_end_block(pp, current->block ? current->block->end_line : 0);
    pp_newline(pp);
    current = current->next;
  }
  pp_end_block(pp, 0);
  pp_newline(pp);
}

static void pp_print_loop_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  pp_stmt_prologue(pp, stmt->line);
  pp_write(pp, stmt->as.loop_stmt.is_parallel ? "parallelLoop" : "loop");
  if (stmt->as.loop_stmt.query_bindings) {
    /* The ECS query loop sugar (#807): print the source bindings, not the
     * hoisted `let` + accessor expansion the parser produced. */
    pp_write(pp, " let ");
    bool first = true;
    for (const AstQueryLoopBinding* qb = stmt->as.loop_stmt.query_bindings; qb; qb = qb->next) {
      if (!first) pp_write(pp, ", ");
      pp_write_str(pp, qb->name);
      pp_write(pp, ": ");
      pp_write_type(pp, qb->type);
      first = false;
    }
    pp_write(pp, " in ");
    pp_expr(pp, stmt->as.loop_stmt.query_iterable);
  } else if (stmt->as.loop_stmt.init) {
    const AstStmt* init = stmt->as.loop_stmt.init;
    if (init->kind == AST_STMT_LET) {
      pp_space(pp);
      if (stmt->as.loop_stmt.is_range) {
        pp_write(pp, init->as.let_stmt.is_var ? "var " : "let ");
      } else {
        pp_write(pp, "var ");
      }
      pp_write_str(pp, init->as.let_stmt.name);
      if (init->as.let_stmt.type) {
        pp_write(pp, ": ");
        pp_write_type(pp, init->as.let_stmt.type);
      }
      if (stmt->as.loop_stmt.is_range) {
        pp_write(pp, " in ");
      } else {
        if (init->as.let_stmt.value) {
          pp_write(pp, " = ");
          pp_expr(pp, init->as.let_stmt.value);
        }
        pp_write(pp, ", ");
      }
    } else if (init->kind == AST_STMT_EXPR) {
      pp_expr(pp, init->as.expr_stmt);
      pp_write(pp, ", ");
    }
  }
  if (!stmt->as.loop_stmt.query_bindings) {
    if (stmt->as.loop_stmt.condition) {
      if (!stmt->as.loop_stmt.init) pp_space(pp);
      pp_expr(pp, stmt->as.loop_stmt.condition);
    }
    if (stmt->as.loop_stmt.increment) {
      pp_write(pp, ", ");
      pp_expr(pp, stmt->as.loop_stmt.increment);
    }
  }
  pp_space(pp);
  pp_begin_block(pp);
  pp_print_block_body(pp, stmt->as.loop_stmt.body);
  pp_end_block(pp, stmt->as.loop_stmt.body ? stmt->as.loop_stmt.body->end_line : 0);
  pp_newline(pp);
}

static void pp_print_assign_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  size_t lines_before = pp_stmt_prologue(pp, stmt->line);
  pp_expr(pp, stmt->as.assign_stmt.target);
  pp_write(pp, stmt->as.assign_stmt.is_bind ? " => " : " = ");
  pp_expr(pp, stmt->as.assign_stmt.value);
  pp_stmt_epilogue(pp, stmt->line, lines_before);
}

static void pp_print_block_stmt(PrettyPrinter* pp, const AstStmt* stmt, const char* keyword,
                                const AstBlock* block) {
  pp_stmt_prologue(pp, stmt->line);
  pp_write(pp, keyword);
  pp_begin_block(pp);
  pp_print_block_body(pp, block);
  pp_end_block(pp, block ? block->end_line : 0);
  pp_newline(pp);
}

static void pp_print_stmt(PrettyPrinter* pp, const AstStmt* stmt) {
  if (stmt->is_synthetic) return;  /* parser-generated (query-loop expansion) */
  switch (stmt->kind) {
    case AST_STMT_LET: pp_print_let_stmt(pp, stmt); break;
    case AST_STMT_DESTRUCT: pp_print_destruct_stmt(pp, stmt); break;
    case AST_STMT_EXPR: {
      size_t lines_before = pp_stmt_prologue(pp, stmt->line);
      pp_expr(pp, stmt->as.expr_stmt);
      pp_stmt_epilogue(pp, stmt->line, lines_before);
      break;
    }
    case AST_STMT_RET: pp_print_return_stmt(pp, stmt); break;
    case AST_STMT_IF: pp_print_if_stmt(pp, stmt); break;
    case AST_STMT_LOOP: pp_print_loop_stmt(pp, stmt); break;
    case AST_STMT_MATCH: pp_print_match_stmt(pp, stmt); break;
    case AST_STMT_ASSIGN: pp_print_assign_stmt(pp, stmt); break;
    case AST_STMT_DEFER: pp_print_block_stmt(pp, stmt, "defer ", stmt->as.defer_stmt.block); break;
    case AST_STMT_UNSAFE: pp_print_block_stmt(pp, stmt, "unsafe ", stmt->as.unsafe_stmt.block); break;
    case AST_STMT_BREAK: {
      size_t lines_before = pp_stmt_prologue(pp, stmt->line);
      pp_write(pp, "break");
      pp_stmt_epilogue(pp, stmt->line, lines_before);
      break;
    }
    case AST_STMT_CONTINUE: {
      size_t lines_before = pp_stmt_prologue(pp, stmt->line);
      pp_write(pp, "continue");
      pp_stmt_epilogue(pp, stmt->line, lines_before);
      break;
    }
  }
}

/* ---- declarations ---- */

static void pp_print_type_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "type ");
  pp_write_str(pp, decl->as.type_decl.name);
  if (decl->as.type_decl.generic_params) pp_param_list(pp, decl->as.type_decl.generic_params, NULL, 0);
  if (decl->as.type_decl.properties) {
    pp_write(pp, ": ");
    pp_write_properties(pp, decl->as.type_decl.properties, NULL);
  }
  pp_space(pp);
  /* A type DECLARATION is a block, one field per line, like a function body —
   * the compact `{ a, b }` form is for literals (docs/rae-format-design.md,
   * "Types, objects, and collections" shows a 4-field type vertical). */
  pp_begin_block(pp);
  pp->block_start = false;
  type_fields_vertical(pp, decl->as.type_decl.fields);
  pp_end_block(pp, 0);
  pp_newline(pp);
}

/* The header after the parameter list, measured so the parameter list can
 * decide with the exact tail: ` extern("sym") ret T {`. */
static void pp_func_tail(PrettyPrinter* pp, const AstDecl* decl) {
  if (decl->as.func_decl.properties) {
    pp_space(pp);
    pp_write_properties(pp, decl->as.func_decl.properties, decl->as.func_decl.extern_symbol);
  }
  if (decl->as.func_decl.returns) {
    int count = 0;
    for (const AstReturnItem* r = decl->as.func_decl.returns; r; r = r->next) count++;
    PpListStyle style = { PP_WRAP_ARGS, " ret ", "", false, decl->as.func_decl.body ? 2 : 0, NULL, false };
    style.open_vertical = " ret";
    pp_print_list(pp, decl->as.func_decl.returns, count, style, return_items_oneline,
                  return_items_vertical);
  }
}

static void pp_print_func_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "func ");
  pp_write_str(pp, decl->as.func_decl.name);
  int tail_width = 0;
  {
    PrettyPrinter measure = pp_measure_begin(pp);
    measure.current_col = 0;
    measure.start_of_line = 0;
    pp_func_tail(&measure, decl);
    tail_width = measure.current_col + (decl->as.func_decl.body ? 2 : 0);
  }
  pp_param_list(pp, decl->as.func_decl.generic_params, decl->as.func_decl.params, tail_width);
  pp_func_tail(pp, decl);
  if (decl->as.func_decl.body) {
    pp_space(pp);
    pp_begin_block(pp);
    pp_print_block_body(pp, decl->as.func_decl.body);
    pp_end_block(pp, decl->as.func_decl.body->end_line);
  }
  pp_newline(pp);
}

static void pp_print_enum_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "enum ");
  pp_write_str(pp, decl->as.enum_decl.name);
  pp_space(pp);
  int count = 0;
  for (const AstEnumMember* m = decl->as.enum_decl.members; m; m = m->next) count++;
  PpListStyle style = { PP_WRAP_FIELDS, "{", "}", true, 0, NULL, false };
  pp_print_list(pp, decl->as.enum_decl.members, count, style, enum_members_oneline, enum_members_vertical);
  pp_newline(pp);
}

static void pp_print_global_let_decl(PrettyPrinter* pp, const AstDecl* decl) {
  if (decl->as.let_decl.is_const) {
    pp_write(pp, "const ");
  } else if (decl->as.let_decl.is_var) {
    pp_write(pp, "var ");
  } else {
    pp_write(pp, "let ");
  }
  pp_write_str(pp, decl->as.let_decl.name);
  if (decl->as.let_decl.type) {
    pp_write(pp, ": ");
    pp_write_type(pp, decl->as.let_decl.type);
  }
  if (decl->as.let_decl.value) {
    pp_write(pp, decl->as.let_decl.is_bind ? " => " : " = ");
    pp_expr(pp, decl->as.let_decl.value);
  }
  pp_flush_trailing_comment(pp, decl->line, pp->lines_written);
  pp_newline(pp);
}

static void pp_print_alias_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_write(pp, "alias ");
  pp_write_str(pp, decl->as.alias_decl.name);
  pp_write(pp, " = ");
  pp_write_type(pp, decl->as.alias_decl.target);
  pp_flush_trailing_comment(pp, decl->line, pp->lines_written);
  pp_newline(pp);
}

void pp_print_decl(PrettyPrinter* pp, const AstDecl* decl) {
  pp_flush_comments_before(pp, decl->line);
  pp->block_start = false;
  pp->stmt_line = decl->line;
  pp->stmt_indent = pp->indent;
  switch (decl->kind) {
    case AST_DECL_TYPE: pp_print_type_decl(pp, decl); break;
    case AST_DECL_ENUM: pp_print_enum_decl(pp, decl); break;
    case AST_DECL_GLOBAL_LET: pp_print_global_let_decl(pp, decl); break;
    case AST_DECL_ALIAS: pp_print_alias_decl(pp, decl); break;
    case AST_DECL_FUNC: pp_print_func_decl(pp, decl); break;
  }
}
