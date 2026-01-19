#include "vm_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "str.h"
#include "vm.h"


static Str get_base_type_name(const AstTypeRef* type) {
  if (!type || !type->parts) return (Str){0};
  AstIdentifierPart* part = type->parts;
  while (part) {
    if (str_eq_cstr(part->text, "mod") || str_eq_cstr(part->text, "view") ||
        str_eq_cstr(part->text, "opt")) {
      part = part->next;
      continue;
    }
    return part->text;
  }
  return (Str){0};
}

bool compiler_ensure_local_capacity(BytecodeCompiler* compiler, uint16_t required,
                                           int line);
static uint16_t emit_jump(BytecodeCompiler* compiler, OpCode op, int line);
static void patch_jump(BytecodeCompiler* compiler, uint16_t offset);
static bool compile_block(BytecodeCompiler* compiler, const AstBlock* block);
static bool emit_native_call(BytecodeCompiler* compiler, Str name, uint8_t arg_count, int line,
                             int column);

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

static uint16_t emit_jump(BytecodeCompiler* compiler, OpCode op, int line) {
  emit_op(compiler, op, line);
  if (compiler->chunk->code_count + 2 > UINT16_MAX) {
    diag_error(compiler->file_path, line, 0, "VM bytecode exceeds 64KB limit");
    compiler->had_error = true;
    return 0;
  }
  uint16_t offset = (uint16_t)compiler->chunk->code_count;
  emit_byte(compiler, 0xFF, line);
  emit_byte(compiler, 0xFF, line);
  return offset;
}

static void patch_jump(BytecodeCompiler* compiler, uint16_t offset) {
  uint16_t target = (uint16_t)compiler->chunk->code_count;
  write_short_at(compiler->chunk, offset, target);
}

static bool str_matches(Str a, Str b) {
  return a.len == b.len && strncmp(a.data, b.data, a.len) == 0;
}

FunctionEntry* function_table_find(FunctionTable* table, Str name) {
  if (!table) return NULL;
  for (size_t i = 0; i < table->count; ++i) {
    if (str_matches(table->entries[i].name, name)) {
      return &table->entries[i];
    }
  }
  return NULL;
}

static bool function_table_add(FunctionTable* table, Str name, uint16_t param_count, bool is_extern, bool returns_ref) {
  FunctionEntry* existing = function_table_find(table, name);
  if (existing) {
    existing->param_count = param_count;
    existing->is_extern = is_extern;
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
  entry->is_extern = is_extern;
  entry->returns_ref = returns_ref;
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
    if (entry->is_extern) {
      continue;
    }
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

TypeEntry* type_table_find(TypeTable* table, Str name) {
  if (!table) return NULL;
  for (size_t i = 0; i < table->count; ++i) {
    if (str_matches(table->entries[i].name, name)) {
      return &table->entries[i];
    }
  }
  return NULL;
}

bool type_table_add(TypeTable* table, Str name, Str* field_names, const AstTypeRef** field_types, size_t field_count) {
  if (type_table_find(table, name)) return true;
  if (table->count + 1 > table->capacity) {
    size_t new_cap = table->capacity < 4 ? 4 : table->capacity * 2;
    TypeEntry* resized = realloc(table->entries, new_cap * sizeof(TypeEntry));
    if (!resized) return false;
    table->entries = resized;
    table->capacity = new_cap;
  }
  TypeEntry* entry = &table->entries[table->count++];
  entry->name = name;
  entry->field_names = field_names;
  entry->field_types = field_types;
  entry->field_count = field_count;
  return true;
}

int type_entry_find_field(const TypeEntry* entry, Str name) {
  for (size_t i = 0; i < entry->field_count; i++) {
    if (str_matches(entry->field_names[i], name)) return (int)i;
  }
  return -1;
}

static bool is_primitive_type(Str type_name) {
  return str_eq_cstr(type_name, "Int") || 
         str_eq_cstr(type_name, "Float") || 
         str_eq_cstr(type_name, "Bool") || 
         str_eq_cstr(type_name, "String");
}

bool collect_metadata(const char* file_path, const AstModule* module, FunctionTable* funcs, TypeTable* types /* GEMINI: MethodTable* methods parameter removed to fix build */) {
  // GEMINI: This function is not fully implemented yet, only a partial implementation.
  // The method table is not yet populated.
  const AstDecl* decl = module->decls;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC) {
      uint16_t param_count = count_params(decl->as.func_decl.params);
      bool returns_ref = false;
      if (decl->as.func_decl.returns) {
        // Multi-returns are not fully supported in VM, but we can check the first one
        if (decl->as.func_decl.returns->type) {
            returns_ref = decl->as.func_decl.returns->type->is_view || decl->as.func_decl.returns->type->is_mod;
        }
      }
      if (!function_table_add(funcs, decl->as.func_decl.name, param_count, decl->as.func_decl.is_extern, returns_ref)) {
        return false;
      }
    } else if (decl->kind == AST_DECL_TYPE) {
      size_t field_count = 0;
      const AstTypeField* f = decl->as.type_decl.fields;
      while (f) { field_count++; f = f->next; }
      
      Str* field_names = malloc(field_count * sizeof(Str));
      const AstTypeRef** field_types = malloc(field_count * sizeof(AstTypeRef*));
      f = decl->as.type_decl.fields;
      for (size_t i = 0; i < field_count; i++) {
        if (f->type->is_view || f->type->is_mod) {
          diag_error(file_path, (int)f->type->line, (int)f->type->column, "view/mod not allowed in struct fields");
          free(field_names);
          free(field_types);
          return false;
        }
        field_names[i] = f->name;
        field_types[i] = f->type;
        f = f->next;
      }
      if (!type_table_add(types, decl->as.type_decl.name, field_names, field_types, field_count)) {
        free(field_names);
        free(field_types);
        return false;
      }
    }
    decl = decl->next;
  }
  return true;
}

void free_type_table(TypeTable* table) {
  if (!table) return;
  for (size_t i = 0; i < table->count; i++) {
    free(table->entries[i].field_names);
    free(table->entries[i].field_types);
  }
  free(table->entries);
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
}

void free_function_table(FunctionTable* table) {
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

void compiler_reset_locals(BytecodeCompiler* compiler) {
  compiler->local_count = 0;
  compiler->allocated_locals = 0;
}

int compiler_add_local(BytecodeCompiler* compiler, Str name, Str type_name) {
  if (compiler->local_count >= sizeof(compiler->locals) / sizeof(compiler->locals[0])) {
    diag_error(compiler->file_path, 0, 0, "VM compiler local limit exceeded");
    compiler->had_error = true;
    return -1;
  }
  compiler->locals[compiler->local_count].name = name;
  compiler->locals[compiler->local_count].slot = compiler->local_count;
  compiler->locals[compiler->local_count].type_name = type_name;
  compiler->local_count += 1;
  return (int)(compiler->local_count - 1);
}

static Str get_local_type_name(BytecodeCompiler* compiler, Str name) {
  for (int i = (int)compiler->local_count - 1; i >= 0; --i) {
    if (str_matches(compiler->locals[i].name, name)) {
      return compiler->locals[i].type_name;
    }
  }
  return (Str){0};
}

int compiler_find_local(BytecodeCompiler* compiler, Str name) {
  for (int i = (int)compiler->local_count - 1; i >= 0; --i) {
    if (str_matches(compiler->locals[i].name, name)) {
      return compiler->locals[i].slot;
    }
  }
  return -1;
}

bool compiler_ensure_local_capacity(BytecodeCompiler* compiler, uint16_t required,
                                           int line) {
  if (required <= compiler->allocated_locals) {
    return true;
  }
  emit_op(compiler, OP_ALLOC_LOCAL, line);
  emit_short(compiler, required, line);
  compiler->allocated_locals = required;
  return true;
}

bool emit_function_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line,
                               int column, uint8_t arg_count) {
  if (!entry) return false;
  if (entry->is_extern) {
    return emit_native_call(compiler, entry->name, arg_count, line, column);
  }
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

static bool emit_native_call(BytecodeCompiler* compiler, Str name, uint8_t arg_count, int line,
                             int column) {
  emit_op(compiler, OP_NATIVE_CALL, line);
  char* symbol = str_to_cstr(name);
  if (!symbol) {
    diag_error(compiler->file_path, line, column, "could not allocate native symbol name");
    compiler->had_error = true;
    return false;
  }
  Value sym_value = value_string_copy(symbol, strlen(symbol));
  free(symbol);
  uint16_t index = chunk_add_constant(compiler->chunk, sym_value);
  emit_short(compiler, index, line);
  emit_byte(compiler, arg_count, line);
  return true;
}

bool emit_return(BytecodeCompiler* compiler, bool has_value, int line) {
  emit_op(compiler, OP_RETURN, line);
  emit_byte(compiler, has_value ? 1 : 0, line);
  return true;
}

static bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr);
static bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt);

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
    // log currently doesn't return a value, but compile_expr is expected to +1
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }

  bool is_rae_str = str_eq_cstr(name, "rae_str");
  bool is_rae_concat = str_eq_cstr(name, "rae_str_concat");
  if (is_rae_str || is_rae_concat) {
    uint16_t arg_count = 0;
    const AstCallArg* arg = expr->as.call.args;
    while (arg) {
      if (!compile_expr(compiler, arg->value)) return false;
      arg_count++;
      arg = arg->next;
    }
    return emit_native_call(compiler, name, (uint8_t)arg_count, (int)expr->line, (int)expr->column);
  }

  /* Intrinsic Buffer Ops */
  if (str_eq_cstr(name, "__buf_alloc")) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    emit_op(compiler, OP_BUF_ALLOC, (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_free")) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    emit_op(compiler, OP_BUF_FREE, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_get")) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    if (!arg->next || !compile_expr(compiler, arg->next->value)) return false;
    emit_op(compiler, OP_BUF_GET, (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_set")) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    if (!arg->next || !compile_expr(compiler, arg->next->value)) return false;
    if (!arg->next->next || !compile_expr(compiler, arg->next->next->value)) return false;
    emit_op(compiler, OP_BUF_SET, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_len")) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    emit_op(compiler, OP_BUF_LEN, (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_resize")) {
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    if (!arg->next || !compile_expr(compiler, arg->next->value)) return false;
    emit_op(compiler, OP_BUF_RESIZE, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_copy")) {
    const AstCallArg* arg = expr->as.call.args;
    for (int i = 0; i < 5; i++) {
        if (!arg || !compile_expr(compiler, arg->value)) return false;
        arg = arg->next;
    }
    emit_op(compiler, OP_BUF_COPY, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
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
  uint16_t current_arg_idx = 0;
  while (arg) {
    bool explicitly_referenced = (arg->value->kind == AST_EXPR_UNARY && 
                                (arg->value->as.unary.op == AST_UNARY_VIEW || 
                                 arg->value->as.unary.op == AST_UNARY_MOD));
    
    // Support positional first argument
    if (current_arg_idx == 0 && arg->name.len == 0) {
        // Unnamed first argument is allowed
    } else if (arg->name.len == 0) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "only the first argument can be passed positionally");
        compiler->had_error = true;
        return false;
    }

    if (explicitly_referenced) {
        const AstExpr* operand = arg->value->as.unary.operand;
        if (operand->kind == AST_EXPR_COLLECTION_LITERAL || operand->kind == AST_EXPR_OBJECT ||
            operand->kind == AST_EXPR_INTEGER || operand->kind == AST_EXPR_FLOAT ||
            operand->kind == AST_EXPR_STRING || operand->kind == AST_EXPR_BOOL) {
            diag_report(compiler->file_path, (int)arg->value->line, (int)arg->value->column, "cannot take reference to a temporary literal");
            compiler->had_error = true;
        }
    } else {
        // Even if not explicitly prefixed with view/mod, the parameter might be a reference.
        // For now, in our simple VM compiler, we check if the argument itself is a literal
        // being passed to what we expect might be a reference parameter.
        // HACK: for simplicity, let's just check if it's a literal being passed to a non-extern call.
        if (!entry->is_extern && (arg->value->kind == AST_EXPR_COLLECTION_LITERAL || arg->value->kind == AST_EXPR_OBJECT)) {
             // In Rae, {x: 1} is a temporary.
             diag_report(compiler->file_path, (int)arg->value->line, (int)arg->value->column, "cannot take reference to a temporary literal");
             compiler->had_error = true;
        }
    }

    if (!entry->is_extern && !explicitly_referenced && arg->value->kind == AST_EXPR_IDENT) {
        // HACK: for now, assume any ident passed to a call might need referencing
        // if we don't have full type info. 
        // This is safer than copying for the 335 test case.
        // BUT: skip primitives, they should always be copied.
        Str type_name = get_local_type_name(compiler, arg->value->as.ident);
        if (type_name.len > 0 && !is_primitive_type(type_name)) {
            int slot = compiler_find_local(compiler, arg->value->as.ident);
            if (slot >= 0) {
                emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
                emit_short(compiler, (uint16_t)slot, (int)expr->line);
            } else {
                if (!compile_expr(compiler, arg->value)) return false;
            }
        } else {
            if (!compile_expr(compiler, arg->value)) return false;
        }
    } else {
        if (!compile_expr(compiler, arg->value)) {
          return false;
        }
    }
    current_arg_idx++;
    arg = arg->next;
  }
  return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column,
                            (uint8_t)arg_count) && !compiler->had_error;
}

static bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (!expr) {
    return false;
  }
  switch (expr->kind) {
    case AST_EXPR_IDENT: {
      int local = compiler_find_local(compiler, expr->as.ident);
      if (local != -1) {
        emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
        emit_short(compiler, (uint16_t)local, (int)expr->line);
        return true;
      }
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "unknown identifier in VM");
      compiler->had_error = true;
      return false;
    }
    case AST_EXPR_STRING: {
      Value string_value = value_string_copy(expr->as.string_lit.data, expr->as.string_lit.len);
      emit_constant(compiler, string_value, (int)expr->line);
      return true;
    }
    case AST_EXPR_INTERP: {
      AstInterpPart* part = expr->as.interp.parts;
      if (!part) {
          emit_constant(compiler, value_string_copy("", 0), (int)expr->line);
          return true;
      }
      
      // Initial part (guaranteed to be a string based on parser)
      if (!compile_expr(compiler, part->value)) return false;
      part = part->next;
      
      while (part) {
          // Push next value
          if (!compile_expr(compiler, part->value)) return false;
          
          // If the next value isn't a string (it's the {expression} result), wrap it in rae_str
          if (part->value->kind != AST_EXPR_STRING) {
              emit_op(compiler, OP_NATIVE_CALL, (int)expr->line);
              const char* native_name = "rae_str";
              Str name_str = str_from_cstr(native_name);
              uint16_t name_idx = chunk_add_constant(compiler->chunk, value_string_copy(name_str.data, name_str.len));
              emit_short(compiler, name_idx, (int)expr->line);
              emit_byte(compiler, 1, (int)expr->line); // 1 arg
          }
          
          // Now stack has [lhs, rhs_str]. Call rae_str_concat
          emit_op(compiler, OP_NATIVE_CALL, (int)expr->line);
          const char* concat_name = "rae_str_concat";
          Str concat_str = str_from_cstr(concat_name);
          uint16_t concat_idx = chunk_add_constant(compiler->chunk, value_string_copy(concat_str.data, concat_str.len));
          emit_short(compiler, concat_idx, (int)expr->line);
          emit_byte(compiler, 2, (int)expr->line); // 2 args
          
          part = part->next;
      }
      return true;
    }
    case AST_EXPR_CHAR: {
      emit_constant(compiler, value_char(expr->as.char_value), (int)expr->line);
      return true;
    }
    case AST_EXPR_INTEGER: {
      long long parsed = strtoll(expr->as.integer.data, NULL, 10);
      emit_constant(compiler, value_int(parsed), (int)expr->line);
      return true;
    }
    case AST_EXPR_FLOAT: {
      double parsed = strtod(expr->as.floating.data, NULL);
      emit_constant(compiler, value_float(parsed), (int)expr->line);
      return true;
    }
    case AST_EXPR_BOOL: {
      emit_constant(compiler, value_bool(expr->as.boolean), (int)expr->line);
      return true;
    }
    case AST_EXPR_NONE: {
      emit_constant(compiler, value_none(), (int)expr->line);
      return true;
    }
    case AST_EXPR_CALL:
      return compile_call(compiler, expr);
    case AST_EXPR_MEMBER: {
      if (expr->as.member.object->kind != AST_EXPR_IDENT) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "VM currently only supports member access on identifiers");
        compiler->had_error = true;
        return false;
      }
      Str obj_name = expr->as.member.object->as.ident;
      Str type_name = get_local_type_name(compiler, obj_name);
      if (type_name.len == 0) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "could not determine type of object for member access");
        compiler->had_error = true;
        return false;
      }
      TypeEntry* type = type_table_find(&compiler->types, type_name);
      if (!type) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "unknown type '%.*s'", (int)type_name.len, type_name.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }
      int field_index = type_entry_find_field(type, expr->as.member.member);
      if (field_index < 0) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "type '%.*s' has no field '%.*s'", 
                 (int)type_name.len, type_name.data,
                 (int)expr->as.member.member.len, expr->as.member.member.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }
      
      if (!compile_expr(compiler, expr->as.member.object)) return false;
      emit_op(compiler, OP_GET_FIELD, (int)expr->line);
      emit_short(compiler, (uint16_t)field_index, (int)expr->line);
      return true;
    }
    case AST_EXPR_BINARY: {
      switch (expr->as.binary.op) {
        case AST_BIN_AND: {
          if (!compile_expr(compiler, expr->as.binary.lhs)) {
            return false;
          }
          uint16_t end_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (!compile_expr(compiler, expr->as.binary.rhs)) {
            return false;
          }
          patch_jump(compiler, end_jump);
          return true;
        }
        case AST_BIN_OR: {
          if (!compile_expr(compiler, expr->as.binary.lhs)) {
            return false;
          }
          uint16_t false_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)expr->line);
          uint16_t end_jump = emit_jump(compiler, OP_JUMP, (int)expr->line);
          patch_jump(compiler, false_jump);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (!compile_expr(compiler, expr->as.binary.rhs)) {
            return false;
          }
          patch_jump(compiler, end_jump);
          return true;
        }
        default:
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
            case AST_BIN_MOD:
              emit_op(compiler, OP_MOD, (int)expr->line);
              return true;
            case AST_BIN_LT:
              emit_op(compiler, OP_LT, (int)expr->line);
              return true;
            case AST_BIN_GT:
              emit_op(compiler, OP_GT, (int)expr->line);
              return true;
            case AST_BIN_LE:
              emit_op(compiler, OP_LE, (int)expr->line);
              return true;
            case AST_BIN_GE:
              emit_op(compiler, OP_GE, (int)expr->line);
              return true;
            case AST_BIN_IS:
              emit_op(compiler, OP_EQ, (int)expr->line);
              return true;
            default:
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                         "binary operator not supported in VM yet");
              compiler->had_error = true;
              return false;
          }
      }
    }
    case AST_EXPR_UNARY: {
      if (!compile_expr(compiler, expr->as.unary.operand)) {
        return false;
      }
      if (expr->as.unary.op == AST_UNARY_NEG) {
        emit_op(compiler, OP_NEG, (int)expr->line);
        return true;
      } else if (expr->as.unary.op == AST_UNARY_NOT) {
        emit_op(compiler, OP_NOT, (int)expr->line);
        return true;
      } else if (expr->as.unary.op == AST_UNARY_VIEW || expr->as.unary.op == AST_UNARY_MOD) {
        bool is_mod = (expr->as.unary.op == AST_UNARY_MOD);
        const AstExpr* operand = expr->as.unary.operand;
        if (operand->kind == AST_EXPR_IDENT) {
          int slot = compiler_find_local(compiler, operand->as.ident);
          if (slot < 0) {
            diag_error(compiler->file_path, (int)operand->line, (int)operand->column, "unknown identifier");
            compiler->had_error = true; return false;
          }
          emit_op(compiler, is_mod ? OP_MOD_LOCAL : OP_VIEW_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)slot, (int)expr->line);
          return true;
        } else if (operand->kind == AST_EXPR_MEMBER) {
          if (!compile_expr(compiler, operand->as.member.object)) return false;
          Str type_name = get_local_type_name(compiler, operand->as.member.object->as.ident);
          TypeEntry* type = type_table_find(&compiler->types, type_name);
          int field_index = type_entry_find_field(type, operand->as.member.member);
          
          emit_op(compiler, is_mod ? OP_MOD_FIELD : OP_VIEW_FIELD, (int)expr->line);
          emit_short(compiler, (uint16_t)field_index, (int)expr->line);
          return true;
        } else {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "view/mod can only be applied to lvalues (identifiers or members)");
          compiler->had_error = true;
          return false;
        }
      } else if (expr->as.unary.op == AST_UNARY_PRE_INC || expr->as.unary.op == AST_UNARY_PRE_DEC ||
                 expr->as.unary.op == AST_UNARY_POST_INC || expr->as.unary.op == AST_UNARY_POST_DEC) {
        bool is_post = (expr->as.unary.op == AST_UNARY_POST_INC || expr->as.unary.op == AST_UNARY_POST_DEC);
        bool is_inc = (expr->as.unary.op == AST_UNARY_PRE_INC || expr->as.unary.op == AST_UNARY_POST_INC);
        const AstExpr* operand = expr->as.unary.operand;

        if (operand->kind == AST_EXPR_IDENT) {
            int slot = compiler_find_local(compiler, operand->as.ident);
            if (slot < 0) {
               diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                          "unknown identifier in increment/decrement");
               compiler->had_error = true;
               return false;
            }
            
            emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
            emit_short(compiler, (uint16_t)slot, (int)expr->line);
            
            if (is_post) {
                emit_op(compiler, OP_DUP, (int)expr->line);
            }
            
            emit_constant(compiler, value_int(1), (int)expr->line);
            emit_op(compiler, is_inc ? OP_ADD : OP_SUB, (int)expr->line);
            
            emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
            emit_short(compiler, (uint16_t)slot, (int)expr->line);
            
            if (is_post) {
                emit_op(compiler, OP_POP, (int)expr->line); // remove new value, leave original
            }
            return true;
        } else if (operand->kind == AST_EXPR_MEMBER) {
            const AstExpr* obj_expr = operand->as.member.object;
            Str type_name = {0};
            if (obj_expr->kind == AST_EXPR_IDENT) {
                type_name = get_local_type_name(compiler, obj_expr->as.ident);
            }
            if (type_name.len == 0) {
                diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                           "could not determine type for member increment/decrement");
                return false;
            }
            TypeEntry* type = type_table_find(&compiler->types, type_name);
            int field_index = type_entry_find_field(type, operand->as.member.member);
            if (field_index < 0) {
                diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "unknown field");
                return false;
            }

            if (obj_expr->kind == AST_EXPR_IDENT) {
                int obj_slot = compiler_find_local(compiler, obj_expr->as.ident);
                emit_op(compiler, OP_GET_LOCAL, (int)obj_expr->line);
                emit_short(compiler, (uint16_t)obj_slot, (int)obj_expr->line);
                
                emit_op(compiler, OP_GET_FIELD, (int)expr->line);
                emit_short(compiler, (uint16_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_DUP, (int)expr->line);
                
                emit_constant(compiler, value_int(1), (int)expr->line);
                emit_op(compiler, is_inc ? OP_ADD : OP_SUB, (int)expr->line);
                
                emit_op(compiler, OP_SET_LOCAL_FIELD, (int)expr->line);
                emit_short(compiler, (uint16_t)obj_slot, (int)expr->line);
                emit_short(compiler, (uint16_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_POP, (int)expr->line);
            } else {
                if (!compile_expr(compiler, obj_expr)) return false;
                emit_op(compiler, OP_DUP, (int)expr->line);
                
                emit_op(compiler, OP_GET_FIELD, (int)expr->line);
                emit_short(compiler, (uint16_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_DUP, (int)expr->line);
                
                emit_constant(compiler, value_int(1), (int)expr->line);
                emit_op(compiler, is_inc ? OP_ADD : OP_SUB, (int)expr->line);
                
                emit_op(compiler, OP_SET_FIELD, (int)expr->line);
                emit_short(compiler, (uint16_t)field_index, (int)expr->line);
                
                if (is_post) emit_op(compiler, OP_POP, (int)expr->line);
            }
            return true;
        }
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "increment/decrement operand must be an identifier or member in VM");
        compiler->had_error = true;
        return false;
      }
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "unary operator not supported in VM yet");
      compiler->had_error = true;
      return false;
    }
    case AST_EXPR_OBJECT: {
      uint16_t count = 0;
      const AstObjectField* f = expr->as.object_literal.fields;
      while (f) {
        if (!compile_expr(compiler, f->value)) return false;
        count++;
        f = f->next;
      }
      emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
      emit_short(compiler, count, (int)expr->line);
      return true;
    }
    case AST_EXPR_MATCH: {
      int subject_slot = compiler_add_local(compiler, str_from_cstr("$match_subject"), (Str){0});
      if (subject_slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) {
        return false;
      }
      if (!compile_expr(compiler, expr->as.match_expr.subject)) {
        return false;
      }
      emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
      emit_short(compiler, (uint16_t)subject_slot, (int)expr->line);
      emit_op(compiler, OP_POP, (int)expr->line);

      int result_slot = compiler_add_local(compiler, str_from_cstr("$match_value"), (Str){0});
      if (result_slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) {
        return false;
      }

      uint16_t end_jumps[256];
      size_t end_count = 0;
      bool has_default = false;
      AstMatchArm* arm = expr->as.match_expr.arms;
      while (arm) {
        bool is_default = arm->pattern == NULL;
        if (is_default) {
          has_default = true;
          if (!compile_expr(compiler, arm->value)) {
            return false;
          }
          emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)result_slot, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)expr->line);
          }
        } else {
          emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)subject_slot, (int)expr->line);
          if (!compile_expr(compiler, arm->pattern)) {
            return false;
          }
          emit_op(compiler, OP_EQ, (int)expr->line);
          uint16_t skip = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (!compile_expr(compiler, arm->value)) {
            return false;
          }
          emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)result_slot, (int)expr->line);
          emit_op(compiler, OP_POP, (int)expr->line);
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)expr->line);
          }
          patch_jump(compiler, skip);
          emit_op(compiler, OP_POP, (int)expr->line);
        }
        arm = arm->next;
      }
      if (!has_default) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                   "match expression requires a 'default' arm");
        compiler->had_error = true;
        return false;
      }
      for (size_t i = 0; i < end_count; ++i) {
        patch_jump(compiler, end_jumps[i]);
      }
      emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
      emit_short(compiler, (uint16_t)result_slot, (int)expr->line);
      return true;
    }
    case AST_EXPR_METHOD_CALL: {
      Str method_name = expr->as.method_call.method_name;
      
      // Handle built-in toString() for all types
      if (str_eq_cstr(method_name, "toString")) {
          if (!compile_expr(compiler, expr->as.method_call.object)) return false;
          return emit_native_call(compiler, str_from_cstr("rae_str"), 1, (int)expr->line, (int)expr->column);
      }

      FunctionEntry* entry = NULL;
      uint16_t explicit_args_count = 0;

      // Desugar: p.method(args) -> method(p, args)
      
      // Compile the receiver ('this')
      const AstExpr* receiver = expr->as.method_call.object;
      
      // We need to decide whether to pass the receiver as a reference or a value.
      // For now, in the VM, we often prefer mod reference for potential mutation
      // unless it's a primitive.
      if (receiver->kind == AST_EXPR_IDENT) {
          int slot = compiler_find_local(compiler, receiver->as.ident);
          if (slot >= 0) {
              // Try to find if the function expects a reference or value.
              // For sugar, we'll try searching for "method" first.
              entry = function_table_find(&compiler->functions, method_name);
              
              // If not found, try common built-ins
              if (!entry) {
                if (str_eq_cstr(method_name, "add")) entry = function_table_find(&compiler->functions, str_from_cstr("listAdd"));
                else if (str_eq_cstr(method_name, "length")) entry = function_table_find(&compiler->functions, str_from_cstr("listLength"));
                else if (str_eq_cstr(method_name, "get")) entry = function_table_find(&compiler->functions, str_from_cstr("listGet"));
              }

              // Decide how to push receiver
              emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
              emit_short(compiler, (uint16_t)slot, (int)expr->line);
          } else {
              if (!compile_expr(compiler, receiver)) return false;
          }
      } else {
          if (!compile_expr(compiler, receiver)) return false;
          // If receiver is an rvalue, we might still need to wrap it if the func expects a ref
          // but for now let's just push it.
      }

      // Compile remaining arguments
      const AstCallArg* current_arg = expr->as.method_call.args;
      while (current_arg) {
        if (!compile_expr(compiler, current_arg->value)) return false;
        explicit_args_count++;
        current_arg = current_arg->next;
      }
      
      uint16_t total_arg_count = 1 + explicit_args_count;

      if (!entry) {
        char buffer[128];
        snprintf(buffer, sizeof(buffer), "unknown method '%.*s'", (int)method_name.len, method_name.data);
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
        compiler->had_error = true;
        return false;
      }

      return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column, total_arg_count);
    }
    case AST_EXPR_INDEX: {
      if (!compile_expr(compiler, expr->as.index.target)) return false;
      if (!compile_expr(compiler, expr->as.index.index)) return false;
      // For index, we fallback to 'listGet' for now
      FunctionEntry* entry = function_table_find(&compiler->functions, str_from_cstr("listGet"));
      if (!entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'listGet' method not found for indexing");
          compiler->had_error = true;
          return false;
      }
      return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column, 2);
    }
    case AST_EXPR_COLLECTION_LITERAL: {
      uint16_t element_count = 0;
      AstCollectionElement* current = expr->as.collection.elements;
      bool is_map = false;
      if (current && current->key) { // Check if the first element has a key to infer map
        is_map = true;
      }

      if (is_map) {
          while (current) {
            if (!compile_expr(compiler, current->value)) return false;
            element_count++;
            current = current->next;
          }
          emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
          emit_short(compiler, element_count, (int)expr->line);
          return true;
      } else {
          // Rae-native List construction
          while (current) { element_count++; current = current->next; }

          FunctionEntry* create_entry = function_table_find(&compiler->functions, str_from_cstr("createList"));
          if (!create_entry) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'createList' not found in core.rae");
              return false;
          }

          // Push initialCap
          emit_constant(compiler, value_int(element_count), (int)expr->line);
          if (!emit_function_call(compiler, create_entry, (int)expr->line, (int)expr->column, 1)) return false;

          // Store list in a temporary local to allow calling methods by reference
          char temp_name[64];
          snprintf(temp_name, sizeof(temp_name), "__list_lit_%zu_%zu", expr->line, expr->column);
          int slot = compiler_add_local(compiler, str_from_cstr(temp_name), str_from_cstr("List"));
          if (slot < 0) return false;
          if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) return false;

          emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)slot, (int)expr->line);
          // Stack still has the list value (OP_SET_LOCAL leaves it there)
          // We pop it because we'll build it via the local ref and then push it back at the end
          emit_op(compiler, OP_POP, (int)expr->line);

          FunctionEntry* add_entry = function_table_find(&compiler->functions, str_from_cstr("listAdd"));
          if (!add_entry) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'listAdd' not found in core.rae");
              return false;
          }

          current = expr->as.collection.elements;
          while (current) {
              // Push mod ref to list
              emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
              emit_short(compiler, (uint16_t)slot, (int)expr->line);

              // Compile element value
              if (!compile_expr(compiler, current->value)) return false;

              // Call listAdd(list, value)
              if (!emit_function_call(compiler, add_entry, (int)expr->line, (int)expr->column, 2)) return false;

              // Pop 'none' result of add
              emit_op(compiler, OP_POP, (int)expr->line);

              current = current->next;
          }

          // Push the final list back onto the stack
          emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)slot, (int)expr->line);
          return true;
      }
    }
    case AST_EXPR_LIST: {
      uint16_t element_count = 0;
      AstExprList* current = expr->as.list;
      while (current) { element_count++; current = current->next; }

      FunctionEntry* create_entry = function_table_find(&compiler->functions, str_from_cstr("createList"));
      if (!create_entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'createList' not found in core.rae");
          return false;
      }

      emit_constant(compiler, value_int(element_count), (int)expr->line);
      if (!emit_function_call(compiler, create_entry, (int)expr->line, (int)expr->column, 1)) return false;

      char temp_name[64];
      snprintf(temp_name, sizeof(temp_name), "__list_lit_%zu_%zu", expr->line, expr->column);
      int slot = compiler_add_local(compiler, str_from_cstr(temp_name), str_from_cstr("List"));
      if (slot < 0) return false;
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)expr->line)) return false;

      emit_op(compiler, OP_SET_LOCAL, (int)expr->line);
      emit_short(compiler, (uint16_t)slot, (int)expr->line);
      emit_op(compiler, OP_POP, (int)expr->line);

      FunctionEntry* add_entry = function_table_find(&compiler->functions, str_from_cstr("listAdd"));
      if (!add_entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'listAdd' not found in core.rae");
          return false;
      }

      current = expr->as.list;
      while (current) {
          emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
          emit_short(compiler, (uint16_t)slot, (int)expr->line);
          if (!compile_expr(compiler, current->value)) return false;
          if (!emit_function_call(compiler, add_entry, (int)expr->line, (int)expr->column, 2)) return false;
          emit_op(compiler, OP_POP, (int)expr->line);
          current = current->next;
      }

      emit_op(compiler, OP_GET_LOCAL, (int)expr->line);
      emit_short(compiler, (uint16_t)slot, (int)expr->line);
      return true;
    }
    default:
      diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
                 "expression not supported in VM yet");
      compiler->had_error = true;
      return false;
  }
}

static const char* stmt_kind_name(AstStmtKind kind) {
  switch (kind) {
    case AST_STMT_DEF: return "def";
    case AST_STMT_DESTRUCT: return "destructure";
    case AST_STMT_EXPR: return "expression";
    case AST_STMT_RET: return "ret";
    case AST_STMT_IF: return "if";
    case AST_STMT_LOOP: return "loop";
    case AST_STMT_MATCH: return "match";
    case AST_STMT_ASSIGN: return "assignment";
  }
  return "unknown";
}

static bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line);

static bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt) {
  if (!stmt) return true;
  switch (stmt->kind) {
    case AST_STMT_DEF: {
      Str type_name = get_base_type_name(stmt->as.def_stmt.type);

      int slot = compiler_add_local(compiler, stmt->as.def_stmt.name, type_name);
      if (slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)stmt->line)) {
        return false;
      }

      if (!stmt->as.def_stmt.value) {
        // Automatically initialize to default value
        if (!emit_default_value(compiler, stmt->as.def_stmt.type, (int)stmt->line)) {
          return false;
        }
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
      } else if (stmt->as.def_stmt.is_bind) {
          if (!stmt->as.def_stmt.type || 
              (!stmt->as.def_stmt.type->is_view && 
               !stmt->as.def_stmt.type->is_mod && 
               !stmt->as.def_stmt.type->is_opt)) {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "=> not allowed for plain value types");
              return false;
          }
          // If the RHS is an identifier or member, we can emit a specific VIEW/MOD instruction
          // to get its address.
          if (stmt->as.def_stmt.value->kind == AST_EXPR_IDENT) {
              int src_slot = compiler_find_local(compiler, stmt->as.def_stmt.value->as.ident);
              emit_op(compiler, stmt->as.def_stmt.type && stmt->as.def_stmt.type->is_view ? OP_VIEW_LOCAL : OP_MOD_LOCAL, (int)stmt->line);
              emit_short(compiler, (uint16_t)src_slot, (int)stmt->line);
          } else if (stmt->as.def_stmt.value->kind == AST_EXPR_MEMBER) {
              const AstExpr* member_expr = stmt->as.def_stmt.value;
              if (member_expr->as.member.object->kind == AST_EXPR_IDENT) {
                  Str obj_name = member_expr->as.member.object->as.ident;
                  Str type_name = get_local_type_name(compiler, obj_name);
                  TypeEntry* type = type_table_find(&compiler->types, type_name);
                  if (type) {
                      int field_index = type_entry_find_field(type, member_expr->as.member.member);
                      if (field_index >= 0) {
                          int obj_slot = compiler_find_local(compiler, obj_name);
                          emit_op(compiler, stmt->as.def_stmt.type && stmt->as.def_stmt.type->is_view ? OP_VIEW_LOCAL : OP_MOD_LOCAL, (int)stmt->line);
                          emit_short(compiler, (uint16_t)obj_slot, (int)stmt->line);
                          emit_op(compiler, stmt->as.def_stmt.type && stmt->as.def_stmt.type->is_view ? OP_VIEW_FIELD : OP_MOD_FIELD, (int)stmt->line);
                          emit_short(compiler, (uint16_t)field_index, (int)stmt->line);
                      } else {
                          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                          return false;
                      }
                  } else {
                      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                      return false;
                  }
              } else {
                  diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                  return false;
              }
          } else {
              // Fallback: compile as value
              if (!compile_expr(compiler, stmt->as.def_stmt.value)) return false;
              
              bool already_ref = false;
              if (stmt->as.def_stmt.value->kind == AST_EXPR_CALL) {
                  const AstExpr* callee = stmt->as.def_stmt.value->as.call.callee;
                  if (callee->kind == AST_EXPR_IDENT) {
                      Str name = callee->as.ident;
                      FunctionEntry* entry = function_table_find(&compiler->functions, name);
                      if (entry && entry->returns_ref) {
                          already_ref = true;
                      }
                  }
              } else if (stmt->as.def_stmt.value->kind == AST_EXPR_METHOD_CALL) {
                  Str method_name = stmt->as.def_stmt.value->as.method_call.method_name;
                  FunctionEntry* entry = function_table_find(&compiler->functions, method_name);
                  if (!entry) {
                      // Try common list methods
                      if (str_eq_cstr(method_name, "add")) entry = function_table_find(&compiler->functions, str_from_cstr("rae_list_add"));
                      else if (str_eq_cstr(method_name, "get")) entry = function_table_find(&compiler->functions, str_from_cstr("rae_list_get"));
                  }
                  if (entry && entry->returns_ref) {
                      already_ref = true;
                  }
              } else if (stmt->as.def_stmt.value->kind == AST_EXPR_NONE) {
                  already_ref = true;
              }
              
              if (!already_ref) {
                  diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value; RHS must be a reference or a function returning one");
                  return false;
              }
          }
          emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
      } else {
          if (!compile_expr(compiler, stmt->as.def_stmt.value)) {
            return false;
          }
          emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
      }
      
      emit_short(compiler, (uint16_t)slot, (int)stmt->line);
      emit_op(compiler, OP_POP, (int)stmt->line);
      return true;
    }
    case AST_STMT_EXPR:
      if (!compile_expr(compiler, stmt->as.expr_stmt)) {
        return false;
      }
      emit_op(compiler, OP_POP, (int)stmt->line);
      return true;
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

      // Lifetime check: can only return reference derived from params
      if (arg->value->kind == AST_EXPR_UNARY && 
          (arg->value->as.unary.op == AST_UNARY_VIEW || arg->value->as.unary.op == AST_UNARY_MOD)) {
          const AstExpr* operand = arg->value->as.unary.operand;
          const AstExpr* base_obj = operand;
          while (base_obj->kind == AST_EXPR_MEMBER) {
              base_obj = base_obj->as.member.object;
          }
          if (base_obj->kind == AST_EXPR_IDENT) {
              int slot = compiler_find_local(compiler, base_obj->as.ident);
              if (slot >= 0) {
                  // In our simple VM compiler, parameters are the first N locals.
                  // We need to check if 'slot' corresponds to a parameter.
                  uint16_t param_count = 0;
                  if (compiler->current_function) {
                      const AstParam* p = compiler->current_function->params;
                      while (p) { param_count++; p = p->next; }
                  }
                  if (slot >= param_count) {
                      diag_report(compiler->file_path, (int)arg->value->line, (int)arg->value->column, "reference escapes local storage");
                      compiler->had_error = true;
                      // continue to compile to find more errors
                  }
              }
          }
      }

      if (!compile_expr(compiler, arg->value)) {
        return false;
      }
      return emit_return(compiler, true, (int)stmt->line);
    }
    case AST_STMT_IF: {
      if (!stmt->as.if_stmt.condition || !stmt->as.if_stmt.then_block) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "if statement missing condition or then block");
        compiler->had_error = true;
        return false;
      }
      if (!compile_expr(compiler, stmt->as.if_stmt.condition)) {
        return false;
      }
      uint16_t else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
      emit_op(compiler, OP_POP, (int)stmt->line);
      if (!compile_block(compiler, stmt->as.if_stmt.then_block)) {
        return false;
      }
      
      uint16_t end_jump = emit_jump(compiler, OP_JUMP, (int)stmt->line);
      
      patch_jump(compiler, else_jump);
      emit_op(compiler, OP_POP, (int)stmt->line);
      if (stmt->as.if_stmt.else_block) {
        if (!compile_block(compiler, stmt->as.if_stmt.else_block)) {
          return false;
        }
      }
      patch_jump(compiler, end_jump);
      return true;
    }
    case AST_STMT_LOOP: {
      if (stmt->as.loop_stmt.is_range) {
        // Range loop: loop x: Type in collection { ... }
        // stmt->as.loop_stmt.init is a DEF stmt for the loop variable 'x'
        // stmt->as.loop_stmt.condition is the collection expression
        
        uint16_t scope_start_locals = compiler->local_count;
        
        // 1. Evaluate collection and store in a hidden local
        if (!compile_expr(compiler, stmt->as.loop_stmt.condition)) return false;
        
        Str col_name = str_from_cstr("__collection");
        Str col_type_name = str_from_cstr("List"); // Default
        
        // Simple type inference for identifiers
        if (stmt->as.loop_stmt.condition->kind == AST_EXPR_IDENT) {
            Str inferred = get_local_type_name(compiler, stmt->as.loop_stmt.condition->as.ident);
            if (inferred.len > 0) col_type_name = inferred;
        } else if (stmt->as.loop_stmt.condition->kind == AST_EXPR_MEMBER) {
            // HACK: for List2Int prototype, we know 'data' is Buffer
            if (str_eq_cstr(stmt->as.loop_stmt.condition->as.member.member, "data")) {
                col_type_name = str_from_cstr("Buffer");
            }
        }
        
        int col_slot = compiler_add_local(compiler, col_name, col_type_name);
        if (col_slot < 0) return false;
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)col_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line); // Clean stack

        // 2. Initialize index = 0 in a hidden local
        emit_constant(compiler, value_int(0), (int)stmt->line);
        Str idx_name = str_from_cstr("__index");
        int idx_slot = compiler_add_local(compiler, idx_name, str_from_cstr("Int"));
        if (idx_slot < 0) return false;
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)idx_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        // 3. Loop Start
        uint16_t loop_start = (uint16_t)compiler->chunk->code_count;

        // 4. Condition: index < collection.length()
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)idx_slot, (int)stmt->line);
        
        // Determine collection type for length call
        bool is_buf = str_eq_cstr(col_type_name, "Buffer");

        // Call length() on collection
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)col_slot, (int)stmt->line);
        if (is_buf) {
            emit_op(compiler, OP_BUF_LEN, (int)stmt->line);
        } else {
            if (!emit_native_call(compiler, str_from_cstr("rae_list_length"), 1, (int)stmt->line, 0)) return false;
        }
        
        emit_op(compiler, OP_LT, (int)stmt->line);
        uint16_t exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        // 5. Body Start: Bind x = collection.get(index)
        Str var_name = stmt->as.loop_stmt.init->as.def_stmt.name;
        Str var_type = get_base_type_name(stmt->as.loop_stmt.init->as.def_stmt.type);
        int var_slot = compiler_add_local(compiler, var_name, var_type);
        if (var_slot < 0) return false;
        
        // collection.get(index)
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)col_slot, (int)stmt->line);
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)idx_slot, (int)stmt->line);
        if (is_buf) {
            emit_op(compiler, OP_BUF_GET, (int)stmt->line);
        } else {
            if (!emit_native_call(compiler, str_from_cstr("rae_list_get"), 2, (int)stmt->line, 0)) return false;
        }
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)var_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        if (!compile_block(compiler, stmt->as.loop_stmt.body)) return false;

        // 6. Increment: index = index + 1
        emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)idx_slot, (int)stmt->line);
        emit_constant(compiler, value_int(1), (int)stmt->line);
        emit_op(compiler, OP_ADD, (int)stmt->line);
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)idx_slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);

        // 7. Jump back
        emit_op(compiler, OP_JUMP, (int)stmt->line);
        emit_short(compiler, loop_start, (int)stmt->line);

        // 8. Exit
        patch_jump(compiler, exit_jump);
        emit_op(compiler, OP_POP, (int)stmt->line);
        
        compiler->local_count = scope_start_locals;
        return true;
      }
      
      // Enter scope for loop variables
      // (VM currently manages locals by simple count, so just tracking count is enough for "scope")
      uint16_t scope_start_locals = compiler->local_count;
      
      if (stmt->as.loop_stmt.init) {
        if (!compile_stmt(compiler, stmt->as.loop_stmt.init)) {
            return false;
        }
        // If init was an expression statement, it left nothing.
        // If init was a def, it added a local.
      }

      uint16_t loop_start = (uint16_t)compiler->chunk->code_count;
      
      uint16_t exit_jump = 0;
      bool has_condition = stmt->as.loop_stmt.condition != NULL;
      
      if (has_condition) {
        if (!compile_expr(compiler, stmt->as.loop_stmt.condition)) {
          return false;
        }
        exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);
      }
      
      if (!compile_block(compiler, stmt->as.loop_stmt.body)) {
        return false;
      }
      
      if (stmt->as.loop_stmt.increment) {
        if (!compile_expr(compiler, stmt->as.loop_stmt.increment)) {
          return false;
        }
        emit_op(compiler, OP_POP, (int)stmt->line); // Discard increment result
      }
      
      emit_op(compiler, OP_JUMP, (int)stmt->line);
      emit_short(compiler, loop_start, (int)stmt->line);
      
      if (has_condition) {
        patch_jump(compiler, exit_jump);
        emit_op(compiler, OP_POP, (int)stmt->line);
      }
      
      // Exit scope (pop locals)
      while (compiler->local_count > scope_start_locals) {
        // In a real VM we'd emit OP_POP_LOCAL or similar, but here we just decrement compiler counter?
        // Wait, the runtime stack needs to be popped if we declared locals.
        // We need to emit OP_POP for each local we leave?
        // Rae VM locals are on stack.
        // Does VM have OP_POP_N? Or just OP_POP?
        // Existing `compile_block` doesn't seem to emit POPs for locals?
        // Let's check `compile_block`. It just loops.
        // Ah, `compile_function` resets locals.
        // It seems the current VM implementation might be "leaking" locals on stack or I missed where they are popped.
        // Re-reading `vm_compiler.c`: `compile_block` does NOT pop locals.
        // `compile_function` resets `local_count` at start.
        // This implies locals persist until function return in current trivial VM?
        // `compiler->local_count` is just a compile-time tracker.
        // If I declare `def x = 1` in a loop, and loop runs 10 times, do I get 10 x's on stack?
        // Yes, if I don't pop them.
        // But `compile_stmt` for `AST_STMT_DEF` emits `SET_LOCAL`.
        // `compiler_add_local` assigns a slot.
        // If the loop re-executes, does it reuse the slot?
        // The slot is fixed at compile time.
        // `def x = 1` -> `SET_LOCAL 5`.
        // Next iteration -> `SET_LOCAL 5`.
        // So the stack depth doesn't grow per iteration.
        // BUT, `init` is outside the loop body.
        // If `init` defines `i`, it takes slot 0.
        // `body` defines `x`, it takes slot 1.
        // Next iteration, `x` uses slot 1 again.
        // So "stack" growth is bounded by unique variables in source.
        // So I don't need to emit POPs for scoping in this simple VM?
        // However, `compiler->local_count` should be reset to `scope_start_locals` after the loop
        // so that subsequent statements reuse those slots.
        // Yes.
        compiler->local_count = scope_start_locals;
      }
      return true;
    }
    case AST_STMT_MATCH: {
      if (!stmt->as.match_stmt.subject || !stmt->as.match_stmt.cases) {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "match statement requires a subject and at least one case");
        compiler->had_error = true;
        return false;
      }
      int subject_slot = compiler_add_local(compiler, str_from_cstr("$match"), (Str){0});
      if (subject_slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)stmt->line)) {
        return false;
      }
      if (!compile_expr(compiler, stmt->as.match_stmt.subject)) {
        return false;
      }
      emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
      emit_short(compiler, (uint16_t)subject_slot, (int)stmt->line);
      emit_op(compiler, OP_POP, (int)stmt->line);

      uint16_t end_jumps[256];
      size_t end_count = 0;
      bool had_default = false;

      const AstMatchCase* match_case = stmt->as.match_stmt.cases;
      while (match_case) {
        bool is_default = match_case->pattern == NULL ||
                          (match_case->pattern && match_case->pattern->kind == AST_EXPR_IDENT &&
                           str_eq_cstr(match_case->pattern->as.ident, "_"));
      if (is_default) {
        if (had_default) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                     "multiple 'default' cases in match");
          compiler->had_error = true;
          return false;
        }
        had_default = true;
          if (!compile_block(compiler, match_case->block)) {
            return false;
          }
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)stmt->line);
          }
        } else {
          emit_op(compiler, OP_GET_LOCAL, (int)stmt->line);
          emit_short(compiler, (uint16_t)subject_slot, (int)stmt->line);
          if (!compile_expr(compiler, match_case->pattern)) {
            return false;
          }
          emit_op(compiler, OP_EQ, (int)stmt->line);
          uint16_t skip = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
          emit_op(compiler, OP_POP, (int)stmt->line);
          if (!compile_block(compiler, match_case->block)) {
            return false;
          }
          if (end_count < sizeof(end_jumps) / sizeof(end_jumps[0])) {
            end_jumps[end_count++] = emit_jump(compiler, OP_JUMP, (int)stmt->line);
          }
          patch_jump(compiler, skip);
          emit_op(compiler, OP_POP, (int)stmt->line);
        }
        match_case = match_case->next;
      }

      for (size_t i = 0; i < end_count; ++i) {
        patch_jump(compiler, end_jumps[i]);
      }
      return true;
    }
    case AST_STMT_ASSIGN: {
      const AstExpr* target = stmt->as.assign_stmt.target;
      if (target->kind == AST_EXPR_IDENT) {
        int slot = compiler_find_local(compiler, target->as.ident);
        if (slot < 0) {
          char buffer[128];
          snprintf(buffer, sizeof(buffer), "unknown identifier '%.*s' in assignment",
                   (int)target->as.ident.len, target->as.ident.data);
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
          compiler->had_error = true;
          return false;
        }

        if (stmt->as.assign_stmt.is_bind) {
            // ... rebinding logic ...
            if (stmt->as.assign_stmt.value->kind == AST_EXPR_IDENT) {
                int src_slot = compiler_find_local(compiler, stmt->as.assign_stmt.value->as.ident);
                emit_op(compiler, OP_MOD_LOCAL, (int)stmt->line);
                emit_short(compiler, (uint16_t)src_slot, (int)stmt->line);
            } else if (stmt->as.assign_stmt.value->kind == AST_EXPR_MEMBER) {
                const AstExpr* member_expr = stmt->as.assign_stmt.value;
                if (member_expr->as.member.object->kind == AST_EXPR_IDENT) {
                    Str obj_name = member_expr->as.member.object->as.ident;
                    Str type_name = get_local_type_name(compiler, obj_name);
                    TypeEntry* type = type_table_find(&compiler->types, type_name);
                    if (type) {
                        int field_index = type_entry_find_field(type, member_expr->as.member.member);
                        if (field_index >= 0) {
                            int obj_slot = compiler_find_local(compiler, obj_name);
                            emit_op(compiler, OP_MOD_LOCAL, (int)stmt->line);
                            emit_short(compiler, (uint16_t)obj_slot, (int)stmt->line);
                            emit_op(compiler, OP_MOD_FIELD, (int)stmt->line);
                            emit_short(compiler, (uint16_t)field_index, (int)stmt->line);
                        } else {
                            diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                            return false;
                        }
                    } else {
                        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                        return false;
                    }
                } else {
                    diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value");
                    return false;
                }
            } else {
                if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
                
                bool already_ref = false;
                if (stmt->as.assign_stmt.value->kind == AST_EXPR_CALL) {
                    const AstExpr* callee = stmt->as.assign_stmt.value->as.call.callee;
                    if (callee->kind == AST_EXPR_IDENT) {
                        Str name = callee->as.ident;
                        FunctionEntry* entry = function_table_find(&compiler->functions, name);
                        if (entry && entry->returns_ref) {
                            already_ref = true;
                        }
                    }
                } else if (stmt->as.assign_stmt.value->kind == AST_EXPR_METHOD_CALL) {
                    Str method_name = stmt->as.assign_stmt.value->as.method_call.method_name;
                    FunctionEntry* entry = function_table_find(&compiler->functions, method_name);
                    if (!entry) {
                        if (str_eq_cstr(method_name, "get")) entry = function_table_find(&compiler->functions, str_from_cstr("rae_list_get"));
                    }
                    if (entry && entry->returns_ref) {
                        already_ref = true;
                    }
                } else if (stmt->as.assign_stmt.value->kind == AST_EXPR_NONE) {
                    already_ref = true;
                }
                
                if (!already_ref) {
                    diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value; RHS must be a reference or a function returning one");
                    return false;
                }
            }
            emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
        } else {
            // Normal assignment: LHS = RHS
            if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
            emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        }
        emit_short(compiler, (uint16_t)slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line); // assigned value
        return true;
      } else if (target->kind == AST_EXPR_MEMBER) {
        if (target->as.member.object->kind != AST_EXPR_IDENT) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                     "VM currently only supports member assignment on identifiers");
          compiler->had_error = true;
          return false;
        }
        Str obj_name = target->as.member.object->as.ident;
        Str type_name = get_local_type_name(compiler, obj_name);
        if (type_name.len == 0) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                     "could not determine type of object for member assignment");
          compiler->had_error = true;
          return false;
        }
        TypeEntry* type = type_table_find(&compiler->types, type_name);
        if (!type) {
          char buffer[128];
          snprintf(buffer, sizeof(buffer), "unknown type '%.*s' for member assignment", (int)type_name.len, type_name.data);
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
          compiler->had_error = true;
          return false;
        }
        int field_index = type_entry_find_field(type, target->as.member.member);
        if (field_index < 0) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "unknown field");
          compiler->had_error = true;
          return false;
        }

        if (stmt->as.assign_stmt.is_bind) {
            if (!compile_expr(compiler, target->as.member.object)) return false;
            if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
            emit_op(compiler, OP_BIND_FIELD, (int)stmt->line);
            emit_short(compiler, (uint16_t)field_index, (int)stmt->line);
        } else {
            int slot = compiler_find_local(compiler, obj_name);
            if (slot >= 0) {
                if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
                emit_op(compiler, OP_SET_LOCAL_FIELD, (int)stmt->line);
                emit_short(compiler, (uint16_t)slot, (int)stmt->line);
                emit_short(compiler, (uint16_t)field_index, (int)stmt->line);
            } else {
                if (!compile_expr(compiler, target->as.member.object)) return false;
                if (!compile_expr(compiler, stmt->as.assign_stmt.value)) return false;
                emit_op(compiler, OP_SET_FIELD, (int)stmt->line);
                emit_short(compiler, (uint16_t)field_index, (int)stmt->line);
            }
        }
        emit_op(compiler, OP_POP, (int)stmt->line); // assigned value
        return true;
      } else {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "VM currently only supports assignment to identifiers or members");
        compiler->had_error = true;
        return false;
      }
    }
    default: {
      char buffer[128];
      snprintf(buffer, sizeof(buffer), "%s statement not supported in VM yet", stmt_kind_name(stmt->kind));
      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
      compiler->had_error = true;
      return false;
    }
  }
}

static bool compile_block(BytecodeCompiler* compiler, const AstBlock* block) {
  const AstStmt* stmt = block ? block->first : NULL;
  bool success = true;
  while (stmt) {
    if (!compile_stmt(compiler, stmt)) {
      success = false;
    }
    stmt = stmt->next;
  }
  return success;
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
  if (func->is_extern) {
    return true;
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
  
  char* func_name = str_to_cstr(func->name);
  if (func_name) {
      chunk_add_function_info(compiler->chunk, func_name, (size_t)entry->offset);
      free(func_name);
  }

  compiler->current_function = func;
  compiler_reset_locals(compiler);
  const AstParam* param = func->params;
  while (param) {
    Str type_name = get_base_type_name(param->type);
    if (compiler_add_local(compiler, param->name, type_name) < 0) {
      compiler->current_function = NULL;
      return false;
    }
    param = param->next;
  }
  compiler->allocated_locals = compiler->local_count;

  const AstStmt* stmt = func->body->first;
  bool success = true;
  while (stmt) {
    if (!compile_stmt(compiler, stmt)) {
      success = false;
    }
    stmt = stmt->next;
  }
  emit_return(compiler, false, (int)decl->line);
  compiler->current_function = NULL;
  return success;
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
      .allocated_locals = 0,
  };
  memset(&compiler.functions, 0, sizeof(FunctionTable));
  memset(&compiler.types, 0, sizeof(TypeTable));
  memset(&compiler.methods, 0, sizeof(MethodTable)); // GEMINI: MethodTable initialization added to fix build.

  if (!collect_metadata(file_path, module, &compiler.functions, &compiler.types /* GEMINI: methods param removed to fix build */)) {
    diag_error(file_path, 0, 0, "failed to prepare VM metadata");
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
      emit_op(&compiler, OP_POP, 0);
      emit_return(&compiler, false, 0);
    }
  }

  const AstDecl* decl = module->decls;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC) {
      if (!compile_function(&compiler, decl)) {
        compiler.had_error = true;
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
  
  bool success = !compiler.had_error;
  free_function_table(&compiler.functions);
  free_type_table(&compiler.types);
  // Method table free not yet implemented but should be added when used.

  return success;
}

static bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line) {
  if (!type || !type->parts) {
    emit_constant(compiler, value_int(0), line);
    return true;
  }

  Str type_name = type->parts->text;
  if (type->is_view || type->is_mod) {
    diag_error(compiler->file_path, line, 0, "references must be explicitly initialized");
    return false;
  }

  if (str_eq_cstr(type_name, "Int")) {
    emit_constant(compiler, value_int(0), line);
  } else if (str_eq_cstr(type_name, "Float")) {
    emit_constant(compiler, value_float(0.0), line);
  } else if (str_eq_cstr(type_name, "Bool")) {
    emit_constant(compiler, value_bool(false), line);
  } else if (str_eq_cstr(type_name, "String")) {
    emit_constant(compiler, value_string_copy("", 0), line);
  } else if (str_eq_cstr(type_name, "Char")) {
    emit_constant(compiler, value_int(0), line);
  } else if (str_eq_cstr(type_name, "List") || str_eq_cstr(type_name, "Array")) {
    emit_op(compiler, OP_NATIVE_CALL, line);
    emit_short(compiler, chunk_add_constant(compiler->chunk, value_string_copy("rae_list_create", 15)), line);
    emit_constant(compiler, value_int(0), line);
    emit_byte(compiler, 1, line);
  } else {
    TypeEntry* entry = type_table_find(&compiler->types, type_name);
    if (!entry) {
      char buf[128];
      snprintf(buf, sizeof(buf), "unknown type '%.*s' for default initialization", (int)type_name.len, type_name.data);
      diag_error(compiler->file_path, line, 0, buf);
      return false;
    }

    for (size_t i = 0; i < entry->field_count; i++) {
      if (!emit_default_value(compiler, entry->field_types[i], line)) {
        return false;
      }
    }
    emit_op(compiler, OP_CONSTRUCT, line);
    emit_short(compiler, (uint16_t)entry->field_count, line);
  }

  return true;
}
