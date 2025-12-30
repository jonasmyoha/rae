#include "vm_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "vm.h"

typedef struct {
  Chunk* chunk;
  const char* file_path;
  bool had_error;
} BytecodeCompiler;

static void emit_byte(BytecodeCompiler* compiler, uint8_t byte, int line) {
  chunk_write(compiler->chunk, byte, line);
}

static void emit_op(BytecodeCompiler* compiler, OpCode op, int line) {
  emit_byte(compiler, (uint8_t)op, line);
}

static void emit_constant(BytecodeCompiler* compiler, Value value, int line) {
  uint16_t index = chunk_add_constant(compiler->chunk, value);
  emit_op(compiler, OP_CONSTANT, line);
  emit_byte(compiler, (uint8_t)((index >> 8) & 0xFF), line);
  emit_byte(compiler, (uint8_t)(index & 0xFF), line);
}

static bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr);
static bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt);

static Value make_string_value(Str literal) {
  size_t out_capacity = literal.len > 2 ? literal.len - 2 : 0;
  char* buffer = malloc(out_capacity + 1);
  if (!buffer) {
    return value_string_copy("", 0);
  }
  size_t out_len = 0;
  for (size_t i = 1; i + 1 < literal.len; ++i) {
    char c = literal.data[i];
    if (c == '\\' && i + 1 < literal.len - 1) {
      i += 1;
      char esc = literal.data[i];
      switch (esc) {
        case 'n':
          c = '\n';
          break;
        case 't':
          c = '\t';
          break;
        case '\\':
          c = '\\';
          break;
        case '"':
          c = '"';
          break;
        default:
          c = esc;
          break;
      }
    }
    buffer[out_len++] = c;
  }
  buffer[out_len] = '\0';
  return value_string_take(buffer, out_len);
}

static bool compile_call(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (expr->as.call.callee->kind != AST_EXPR_IDENT) {
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
               "VM currently only supports direct function calls");
    compiler->had_error = true;
    return false;
  }
  Str name = expr->as.call.callee->as.ident;
  if (!str_eq_cstr(name, "print")) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "unsupported call '%.*s' in VM mode",
             (int)name.len, name.data);
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
    compiler->had_error = true;
    return false;
  }

  const AstCallArg* arg = expr->as.call.args;
  if (!arg || arg->next) {
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
               "print currently expects exactly one argument");
    compiler->had_error = true;
    return false;
  }
  if (!compile_expr(compiler, arg->value)) {
    return false;
  }
  emit_op(compiler, OP_PRINT, (int)expr->line);
  return true;
}

static bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (!expr) return false;
  switch (expr->kind) {
    case AST_EXPR_STRING: {
      Value string_value = make_string_value(expr->as.string_lit);
      emit_constant(compiler, string_value, (int)expr->line);
      return true;
    }
    case AST_EXPR_INTEGER: {
      long long parsed = strtoll(expr->as.integer.data, NULL, 10);
      emit_constant(compiler, value_int(parsed), (int)expr->line);
      return true;
    }
    case AST_EXPR_BOOL: {
      emit_constant(compiler, value_bool(expr->as.boolean), (int)expr->line);
      return true;
    }
    case AST_EXPR_CALL:
      return compile_call(compiler, expr);
    default:
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "expression not supported in VM yet");
      compiler->had_error = true;
      return false;
  }
}

static bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt) {
  switch (stmt->kind) {
    case AST_STMT_EXPR:
      return compile_expr(compiler, stmt->as.expr_stmt);
    default:
      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                 "statement not supported in VM yet");
      compiler->had_error = true;
      return false;
  }
}

bool vm_compile_module(const AstModule* module, Chunk* chunk, const char* file_path) {
  if (!module || !chunk) return false;
  chunk_init(chunk);
  BytecodeCompiler compiler = {
      .chunk = chunk,
      .file_path = file_path,
      .had_error = false,
  };

  const AstDecl* decl = module->decls;
  const AstDecl* main_decl = NULL;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC && str_eq_cstr(decl->as.func_decl.name, "main")) {
      main_decl = decl;
      break;
    }
    decl = decl->next;
  }

  if (!main_decl) {
    diag_error(file_path, 0, 0, "no `func main` found for VM execution");
    return false;
  }

  const AstFuncDecl* main_func = &main_decl->as.func_decl;
  const AstStmt* stmt = main_func->body ? main_func->body->first : NULL;
  while (stmt) {
    if (!compile_stmt(&compiler, stmt)) {
      break;
    }
    stmt = stmt->next;
  }

  emit_op(&compiler, OP_RETURN, (int)(main_decl->line));
  if (compiler.had_error) {
    chunk_free(chunk);
  }
  return !compiler.had_error;
}
