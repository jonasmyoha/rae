#include "vm_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "vm.h"

typedef struct {
  Str name;
  uint16_t offset;
  uint16_t param_count;
  uint16_t* patches;
  size_t patch_count;
  size_t patch_capacity;
} FunctionEntry;

typedef struct {
  FunctionEntry* entries;
  size_t count;
  size_t capacity;
} FunctionTable;

typedef struct {
  Chunk* chunk;
  const char* file_path;
  bool had_error;
  FunctionTable functions;
  const AstFuncDecl* current_function;
  struct {
    Str name;
    uint16_t slot;
  } locals[256];
  uint16_t local_count;
} BytecodeCompiler;

#define INVALID_OFFSET UINT16_MAX

static void free_function_table(FunctionTable* table);
static FunctionEntry* function_table_find(FunctionTable* table, Str name);
static bool function_table_add(FunctionTable* table, Str name, uint16_t param_count);
static bool function_entry_add_patch(FunctionEntry* entry, uint16_t patch_offset);
static bool patch_function_calls(FunctionTable* table, Chunk* chunk, const char* file_path);
static bool collect_function_entries(const AstModule* module, FunctionTable* table);
static bool compile_function(BytecodeCompiler* compiler, const AstDecl* decl);
static bool emit_function_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line,
                               int column, uint8_t arg_count);
static bool emit_return(BytecodeCompiler* compiler, bool has_value, int line);
static bool compiler_add_local(BytecodeCompiler* compiler, Str name);
static int compiler_find_local(BytecodeCompiler* compiler, Str name);
static void compiler_reset_locals(BytecodeCompiler* compiler);

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

static void emit_short(BytecodeCompiler* compiler, uint16_t value, int line) {
  emit_byte(compiler, (uint8_t)((value >> 8) & 0xFF), line);
  emit_byte(compiler, (uint8_t)(value & 0xFF), line);
}

static void write_short_at(Chunk* chunk, uint16_t offset, uint16_t value) {
  if (offset + 1 >= chunk->code_count) return;
  chunk->code[offset] = (uint8_t)((value >> 8) & 0xFF);
  chunk->code[offset + 1] = (uint8_t)(value & 0xFF);
}

static bool str_matches(Str a, Str b) {
  return a.len == b.len && strncmp(a.data, b.data, a.len) == 0;
}

static FunctionEntry* function_table_find(FunctionTable* table, Str name) {
  if (!table) return NULL;
  for (size_t i = 0; i < table->count; ++i) {
    if (str_matches(table->entries[i].name, name)) {
      return &table->entries[i];
    }
  }
  return NULL;
}

static bool function_table_add(FunctionTable* table, Str name, uint16_t param_count) {
  FunctionEntry* existing = function_table_find(table, name);
  if (existing) {
    existing->param_count = param_count;
    return true;
  }
  if (table->count + 1 > table->capacity) {
    size_t old_cap = table->capacity;
    size_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
    FunctionEntry* resized = realloc(table->entries, new_cap * sizeof(FunctionEntry));
    if (!resized) {
      return false;
    }
    table->entries = resized;
    table->capacity = new_cap;
  }
  FunctionEntry* entry = &table->entries[table->count++];
  entry->name = name;
  entry->offset = INVALID_OFFSET;
  entry->param_count = param_count;
  entry->patches = NULL;
  entry->patch_count = 0;
  entry->patch_capacity = 0;
  return true;
}

static bool function_entry_add_patch(FunctionEntry* entry, uint16_t patch_offset) {
  if (entry->patch_count + 1 > entry->patch_capacity) {
    size_t old_cap = entry->patch_capacity;
    size_t new_cap = old_cap < 4 ? 4 : old_cap * 2;
    uint16_t* resized = realloc(entry->patches, new_cap * sizeof(uint16_t));
    if (!resized) {
      return false;
    }
    entry->patches = resized;
    entry->patch_capacity = new_cap;
  }
  entry->patches[entry->patch_count++] = patch_offset;
  return true;
}

static bool patch_function_calls(FunctionTable* table, Chunk* chunk, const char* file_path) {
  for (size_t i = 0; i < table->count; ++i) {
    FunctionEntry* entry = &table->entries[i];
    if (entry->offset == INVALID_OFFSET) {
      char buffer[128];
      snprintf(buffer, sizeof(buffer), "function '%.*s' missing implementation",
               (int)entry->name.len, entry->name.data);
      diag_error(file_path, 0, 0, buffer);
      return false;
    }
    for (size_t j = 0; j < entry->patch_count; ++j) {
      write_short_at(chunk, entry->patches[j], entry->offset);
    }
  }
  return true;
}

static uint16_t count_params(const AstParam* param) {
  uint16_t count = 0;
  while (param) {
    if (count == UINT16_MAX) {
      return UINT16_MAX;
    }
    count += 1;
    param = param->next;
  }
  return count;
}

static bool collect_function_entries(const AstModule* module, FunctionTable* table) {
  const AstDecl* decl = module->decls;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC) {
      uint16_t param_count = count_params(decl->as.func_decl.params);
      if (!function_table_add(table, decl->as.func_decl.name, param_count)) {
        return false;
      }
    }
    decl = decl->next;
  }
  return true;
}

static void free_function_table(FunctionTable* table) {
  if (!table) return;
  for (size_t i = 0; i < table->count; ++i) {
    free(table->entries[i].patches);
    table->entries[i].patches = NULL;
    table->entries[i].patch_capacity = 0;
    table->entries[i].patch_count = 0;
  }
  free(table->entries);
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
}

static void compiler_reset_locals(BytecodeCompiler* compiler) {
  compiler->local_count = 0;
}

static bool compiler_add_local(BytecodeCompiler* compiler, Str name) {
  if (compiler->local_count >= sizeof(compiler->locals) / sizeof(compiler->locals[0])) {
    diag_error(compiler->file_path, 0, 0, "VM compiler local limit exceeded");
    compiler->had_error = true;
    return false;
  }
  compiler->locals[compiler->local_count].name = name;
  compiler->locals[compiler->local_count].slot = compiler->local_count;
  compiler->local_count += 1;
  return true;
}

static int compiler_find_local(BytecodeCompiler* compiler, Str name) {
  for (int i = (int)compiler->local_count - 1; i >= 0; --i) {
    if (str_matches(compiler->locals[i].name, name)) {
      return compiler->locals[i].slot;
    }
  }
  return -1;
}

static bool emit_function_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line,
                               int column, uint8_t arg_count) {
  if (!entry) return false;
  emit_op(compiler, OP_CALL, line);
  if (compiler->chunk->code_count > UINT16_MAX) {
    diag_error(compiler->file_path, line, column, "VM bytecode exceeds 64KB limit");
    compiler->had_error = true;
    return false;
  }
  uint16_t patch_offset = (uint16_t)compiler->chunk->code_count;
  emit_short(compiler, 0, line);
  emit_byte(compiler, arg_count, line);
  if (!function_entry_add_patch(entry, patch_offset)) {
    diag_error(compiler->file_path, line, column, "failed to record function call patch");
    compiler->had_error = true;
    return false;
  }
  return true;
}

static bool emit_return(BytecodeCompiler* compiler, bool has_value, int line) {
  emit_op(compiler, OP_RETURN, line);
  emit_byte(compiler, has_value ? 1 : 0, line);
  return true;
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
  bool is_log = str_eq_cstr(name, "log");
  bool is_log_s = str_eq_cstr(name, "logS");
  if (is_log || is_log_s) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || arg->next) {
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "log/logS currently expect exactly one argument");
      compiler->had_error = true;
      return false;
    }
    if (!compile_expr(compiler, arg->value)) {
      return false;
    }
    emit_op(compiler, is_log ? OP_LOG : OP_LOG_S, (int)expr->line);
    return true;
  }

  FunctionEntry* entry = function_table_find(&compiler->functions, name);
  if (!entry) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "unknown function '%.*s' for VM call", (int)name.len,
             name.data);
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
    compiler->had_error = true;
    return false;
  }

  uint16_t arg_count = 0;
  const AstCallArg* arg = expr->as.call.args;
  while (arg) {
    arg_count += 1;
    arg = arg->next;
  }
  if (arg_count != entry->param_count) {
    char buffer[160];
    snprintf(buffer, sizeof(buffer),
             "function '%.*s' expects %u argument(s) but call has %u",
             (int)name.len, name.data, entry->param_count, arg_count);
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
    compiler->had_error = true;
    return false;
  }
  if (arg_count > UINT8_MAX) {
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
               "VM call argument count exceeds supported limit");
    compiler->had_error = true;
    return false;
  }

  arg = expr->as.call.args;
  while (arg) {
    if (!compile_expr(compiler, arg->value)) {
      return false;
    }
    arg = arg->next;
  }
  return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column,
                            (uint8_t)arg_count);
}

static bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (!expr) return false;
  switch (expr->kind) {
    case AST_EXPR_IDENT: {
      int slot = compiler_find_local(compiler, expr->as.ident);
      if (slot < 0) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "unknown identifier '%.*s' in VM mode",
                 (int)expr->as.ident.len, expr->as.ident.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }
      emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
      emit_short(compiler, (uint16_t)slot, (int)expr->line);
      return true;
    }
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
    case AST_EXPR_BINARY: {
      if (!compile_expr(compiler, expr->as.binary.lhs)) {
        return false;
      }
      if (!compile_expr(compiler, expr->as.binary.rhs)) {
        return false;
      }
      switch (expr->as.binary.op) {
        case AST_BIN_ADD:
          emit_op(compiler, OP_ADD, (int)expr->line);
          return true;
        case AST_BIN_SUB:
          emit_op(compiler, OP_SUB, (int)expr->line);
          return true;
        case AST_BIN_MUL:
          emit_op(compiler, OP_MUL, (int)expr->line);
          return true;
        case AST_BIN_DIV:
          emit_op(compiler, OP_DIV, (int)expr->line);
          return true;
        default:
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                     "binary operator not supported in VM yet");
          compiler->had_error = true;
          return false;
      }
    }
    case AST_EXPR_UNARY: {
      if (!compile_expr(compiler, expr->as.unary.operand)) {
        return false;
      }
      if (expr->as.unary.op == AST_UNARY_NEG) {
        emit_op(compiler, OP_NEG, (int)expr->line);
        return true;
      }
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "unary operator not supported in VM yet");
      compiler->had_error = true;
      return false;
    }
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
    case AST_STMT_RET: {
      const AstReturnArg* arg = stmt->as.ret_stmt.values;
      if (!arg) {
        return emit_return(compiler, false, (int)stmt->line);
      }
      if (arg->next) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "multiple return values not supported in VM yet");
        compiler->had_error = true;
        return false;
      }
      if (arg->has_label) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "labeled returns not supported in VM yet");
        compiler->had_error = true;
        return false;
      }
      if (!compile_expr(compiler, arg->value)) {
        return false;
      }
      return emit_return(compiler, true, (int)stmt->line);
    }
    default:
      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                 "statement not supported in VM yet");
      compiler->had_error = true;
      return false;
  }
}

static bool compile_function(BytecodeCompiler* compiler, const AstDecl* decl) {
  if (!decl || decl->kind != AST_DECL_FUNC) {
    return false;
  }
  const AstFuncDecl* func = &decl->as.func_decl;
  FunctionEntry* entry = function_table_find(&compiler->functions, func->name);
  if (!entry) {
    diag_error(compiler->file_path, (int)decl->line, (int)decl->column,
               "function table entry missing during VM compilation");
    compiler->had_error = true;
    return false;
  }
  if (entry->offset != INVALID_OFFSET) {
    diag_error(compiler->file_path, (int)decl->line, (int)decl->column,
               "duplicate function definition in VM compiler");
    compiler->had_error = true;
    return false;
  }
  if (!func->body) {
    diag_error(compiler->file_path, (int)decl->line, (int)decl->column,
               "functions without a body are not supported in VM yet");
    compiler->had_error = true;
    return false;
  }
  if (compiler->chunk->code_count > UINT16_MAX) {
    diag_error(compiler->file_path, (int)decl->line, (int)decl->column,
               "VM bytecode exceeds 64KB limit");
    compiler->had_error = true;
    return false;
  }
  entry->offset = (uint16_t)compiler->chunk->code_count;

  compiler->current_function = func;
  compiler_reset_locals(compiler);
  const AstParam* param = func->params;
  while (param) {
    if (!compiler_add_local(compiler, param->name)) {
      compiler->current_function = NULL;
      return false;
    }
    param = param->next;
  }

  const AstStmt* stmt = func->body->first;
  while (stmt) {
    if (!compile_stmt(compiler, stmt)) {
      compiler->current_function = NULL;
      return false;
    }
    stmt = stmt->next;
  }
  emit_return(compiler, false, (int)decl->line);
  compiler->current_function = NULL;
  return true;
}

bool vm_compile_module(const AstModule* module, Chunk* chunk, const char* file_path) {
  if (!module || !chunk) return false;
  chunk_init(chunk);
  BytecodeCompiler compiler = {
      .chunk = chunk,
      .file_path = file_path,
      .had_error = false,
      .current_function = NULL,
      .local_count = 0,
  };

  if (!collect_function_entries(module, &compiler.functions)) {
    diag_error(file_path, 0, 0, "failed to prepare VM function table");
    compiler.had_error = true;
  }

  Str main_name = str_from_cstr("main");
  FunctionEntry* main_entry = function_table_find(&compiler.functions, main_name);
  if (!main_entry) {
    diag_error(file_path, 0, 0, "no `func main` found for VM execution");
    compiler.had_error = true;
  }

  if (!compiler.had_error) {
    if (!emit_function_call(&compiler, main_entry, 0, 0, 0)) {
      compiler.had_error = true;
    } else {
      emit_return(&compiler, false, 0);
    }
  }

  const AstDecl* decl = module->decls;
  while (!compiler.had_error && decl) {
    if (decl->kind == AST_DECL_FUNC) {
      if (!compile_function(&compiler, decl)) {
        break;
      }
    }
    decl = decl->next;
  }

  if (!compiler.had_error) {
    if (!patch_function_calls(&compiler.functions, chunk, file_path)) {
      compiler.had_error = true;
    }
  }

  if (compiler.had_error) {
    chunk_free(chunk);
  }
  free_function_table(&compiler.functions);
  return !compiler.had_error;
}
