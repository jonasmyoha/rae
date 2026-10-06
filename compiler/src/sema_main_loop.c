/* `mainLoop state { body }` (docs/web-frame-loop.md §5): where it may stand
 * and what its body may use. The body becomes its own C function, called once
 * per frame — natively from a loop, in a browser by the page — so it can see
 * nothing of `main` but the state it is handed, and `main` must end with it. */
#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "sema.h"

int rae_count_ident_refs_in_block(const AstBlock* block, Str name);  // c_stmt.c

static void main_loop_error(AstModule* module, const char* file, const AstStmt* stmt, const char* message) {
  diag_error(file, (int)stmt->line, (int)stmt->column, message);
  module->had_error = true;
}

static void check_block_nested(AstModule* module, const char* file, const AstBlock* block,
                               bool in_main_loop_body);

/* A statement below main's own block: a mainLoop here is misplaced, and a
 * `ret` inside a mainLoop body would leave the frame function. */
static void check_stmt_nested(AstModule* module, const char* file, const AstStmt* stmt,
                              bool in_main_loop_body) {
  switch (stmt->kind) {
    case AST_STMT_RET:
      if (in_main_loop_body)
        main_loop_error(module, file, stmt,
                        "'ret' cannot leave a mainLoop body: it runs as one frame at a time; 'break' ends the loop");
      break;
    case AST_STMT_IF:
      check_block_nested(module, file, stmt->as.if_stmt.then_block, in_main_loop_body);
      check_block_nested(module, file, stmt->as.if_stmt.else_block, in_main_loop_body);
      break;
    case AST_STMT_LOOP:
      if (stmt->as.loop_stmt.is_main_loop)
        main_loop_error(module, file, stmt,
                        "mainLoop must be a statement of 'main' itself, not inside another block");
      check_block_nested(module, file, stmt->as.loop_stmt.body, in_main_loop_body);
      break;
    case AST_STMT_MATCH:
      for (const AstMatchCase* match_case = stmt->as.match_stmt.cases; match_case; match_case = match_case->next)
        check_block_nested(module, file, match_case->block, in_main_loop_body);
      break;
    case AST_STMT_DEFER: check_block_nested(module, file, stmt->as.defer_stmt.block, in_main_loop_body); break;
    case AST_STMT_UNSAFE: check_block_nested(module, file, stmt->as.unsafe_stmt.block, in_main_loop_body); break;
    default: break;
  }
}

static void check_block_nested(AstModule* module, const char* file, const AstBlock* block,
                               bool in_main_loop_body) {
  if (!block) return;
  for (const AstStmt* stmt = block->first; stmt; stmt = stmt->next)
    check_stmt_nested(module, file, stmt, in_main_loop_body);
}

/* The state: an owned `var` of a struct type declared in main's own block
 * before the loop. */
static void check_main_loop_state(AstModule* module, const char* file, const AstStmt* loop,
                                  const AstStmt* state_decl) {
  char message[512];
  Str name = loop->as.loop_stmt.main_loop_state;
  if (!state_decl) {
    snprintf(message, sizeof message,
             "mainLoop's state '%.*s' must be a 'var' declared in main before the loop",
             (int)name.len, name.data);
    main_loop_error(module, file, loop, message);
    return;
  }
  const AstTypeRef* type = state_decl->as.let_stmt.type;
  if (state_decl->as.let_stmt.is_bind || !state_decl->as.let_stmt.is_var
      || (type && (type->is_view || type->is_mod || type->is_opt))) {
    snprintf(message, sizeof message,
             "mainLoop's state '%.*s' must be an owned value declared with 'var' (the loop takes it over and drops it when it ends)",
             (int)name.len, name.data);
    main_loop_error(module, file, loop, message);
    return;
  }
  const TypeInfo* resolved = type ? type->resolved_type : NULL;
  if (!resolved || resolved->kind != TYPE_STRUCT) {
    Str type_name = type ? get_base_type_name(type) : (Str){0};
    snprintf(message, sizeof message,
             "mainLoop's state '%.*s' must be a struct value (one type that owns the program's state), not '%.*s'",
             (int)name.len, name.data, (int)type_name.len, type_name.data);
    main_loop_error(module, file, loop, message);
  }
}

/* A local of main that the body names, other than the state. */
static void check_main_loop_captures(AstModule* module, const char* file, const AstStmt* loop,
                                     const AstFuncDecl* func) {
  Str state = loop->as.loop_stmt.main_loop_state;
  const AstBlock* body = loop->as.loop_stmt.body;
  for (const AstStmt* stmt = func->body->first; stmt && stmt != loop; stmt = stmt->next) {
    Str names[64];
    size_t name_count = 0;
    if (stmt->kind == AST_STMT_LET) names[name_count++] = stmt->as.let_stmt.name;
    if (stmt->kind == AST_STMT_DESTRUCT) {
      for (const AstDestructureBinding* binding = stmt->as.destruct_stmt.bindings;
           binding && name_count < 64; binding = binding->next)
        names[name_count++] = binding->local_name;
    }
    for (size_t i = 0; i < name_count; i++) {
      if (str_eq(names[i], state) || rae_count_ident_refs_in_block(body, names[i]) == 0) continue;
      char message[512];
      snprintf(message, sizeof message,
               "mainLoop's body may use only its state '%.*s' and constants, but it uses main's local '%.*s'; move it into the state",
               (int)state.len, state.data, (int)names[i].len, names[i].data);
      main_loop_error(module, file, loop, message);
    }
  }
}

void sema_check_main_loops(AstModule* module, const char* file, const AstFuncDecl* func) {
  if (!func->body) return;
  bool is_main = str_eq_cstr(func->name, "main");
  for (const AstStmt* stmt = func->body->first; stmt; stmt = stmt->next) {
    if (stmt->kind != AST_STMT_LOOP || !stmt->as.loop_stmt.is_main_loop) {
      check_stmt_nested(module, file, stmt, false);
      continue;
    }
    if (!is_main) {
      main_loop_error(module, file, stmt,
                      "mainLoop is the program's frame loop, so it may stand only in 'main'");
      check_block_nested(module, file, stmt->as.loop_stmt.body, true);
      continue;
    }
    const AstStmt* after = stmt->next;
    if (after && (after->kind != AST_STMT_RET || after->next)) {
      main_loop_error(module, file, after,
                      "mainLoop must be main's last statement; only 'ret <exit code>' may follow it (in a browser, main returns before the first frame)");
    }
    const AstStmt* state_decl = NULL;
    for (const AstStmt* before = func->body->first; before != stmt; before = before->next) {
      if (before->kind == AST_STMT_LET && str_eq(before->as.let_stmt.name, stmt->as.loop_stmt.main_loop_state))
        state_decl = before;
    }
    check_main_loop_state(module, file, stmt, state_decl);
    check_main_loop_captures(module, file, stmt, func);
    check_block_nested(module, file, stmt->as.loop_stmt.body, true);
  }
}
