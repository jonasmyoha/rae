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
static bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line);

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

static Str strip_generics(Str type_name) {
  for (size_t i = 0; i < type_name.len; ++i) {
    if (type_name.data[i] == '(') {
      return (Str){.data = type_name.data, .len = i};
    }
  }
  return type_name;
}

static Str get_base_type_name_str(Str type_name) {
  Str res = type_name;
  if (str_starts_with_cstr(res, "mod ")) {
    res.data += 4;
    res.len -= 4;
  } else if (str_starts_with_cstr(res, "view ")) {
    res.data += 5;
    res.len -= 5;
  } else if (str_starts_with_cstr(res, "opt ")) {
    res.data += 4;
    res.len -= 4;
  }
  return res;
}

static bool types_match(Str entry_type_raw, Str call_type_raw) {
  Str entry_type = strip_generics(get_base_type_name_str(entry_type_raw));
  Str call_type = strip_generics(get_base_type_name_str(call_type_raw));

  // 1. Exact match
  if (str_matches(entry_type, call_type)) return true;
  
  // 2. Generic placeholder (single uppercase letter like 'T')
  // If entry type is a placeholder, it matches anything
  if (entry_type.len == 1 && entry_type.data[0] >= 'A' && entry_type.data[0] <= 'Z') {
    return true;
  }
  
  // 'Any' in call matches anything in entry
  if (str_eq_cstr(call_type, "Any")) {
      return true;
  }
  
  // 'Any' in entry matches anything in call
  if (str_eq_cstr(entry_type, "Any")) {
      return true;
  }

  // 3. Complex generic match: List(T) vs List(Int)
  Str entry_base = strip_generics(entry_type);
  Str call_base = strip_generics(call_type);
  
  if (str_matches(entry_base, call_base) && entry_type.len > entry_base.len && call_type.len > call_base.len) {
    // Both have generics, match the inner part
    // entry: List(T), call: List(Int)
    // inner entry: T, inner call: Int
    Str entry_inner = { .data = entry_type.data + entry_base.len + 1, .len = entry_type.len - entry_base.len - 2 };
    Str call_inner = { .data = call_type.data + call_base.len + 1, .len = call_type.len - call_base.len - 2 };
    return types_match(entry_inner, call_inner);
  }
  
  return false;
}

FunctionEntry* function_table_find_overload(FunctionTable* table, Str name, const Str* param_types, uint16_t param_count) {
  if (!table) return NULL;
  
  FunctionEntry* name_match = NULL;

  for (size_t i = 0; i < table->count; ++i) {
    FunctionEntry* entry = &table->entries[i];
    if (str_matches(entry->name, name)) {
      if (entry->param_count == param_count) {
        if (param_types == NULL) return entry;
        
        bool mismatch = false;
        for (uint16_t j = 0; j < param_count; ++j) {
          if (!types_match(entry->param_types[j], param_types[j])) {
            mismatch = true;
            break;
          }
        }
        if (!mismatch) return entry;
      }
      
      // Fallback: remember the first name match with same arity
      if (!name_match && entry->param_count == param_count) {
          name_match = entry;
      }
    }
  }
  return name_match;
}

static FunctionEntry* function_table_find_exact(FunctionTable* table, Str name, const Str* param_types, uint16_t param_count) {
  if (!table) return NULL;
  for (size_t i = 0; i < table->count; ++i) {
    FunctionEntry* entry = &table->entries[i];
    if (str_matches(entry->name, name) && entry->param_count == param_count) {
      bool mismatch = false;
      for (uint16_t j = 0; j < param_count; ++j) {
        if (!str_matches(entry->param_types[j], param_types[j])) {
          mismatch = true;
          break;
        }
      }
      if (!mismatch) return entry;
    }
  }
  return NULL;
}

static bool function_table_add(FunctionTable* table, Str name, const Str* param_types, uint16_t param_count, bool is_extern, bool returns_ref, Str return_type) {
  FunctionEntry* existing = function_table_find_exact(table, name, param_types, param_count);
  if (existing) {
    existing->is_extern = is_extern;
    existing->return_type = return_type;
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
  entry->param_types = malloc(param_count * sizeof(Str));
  for (uint16_t i = 0; i < param_count; ++i) {
    entry->param_types[i] = param_types[i];
  }
  entry->offset = INVALID_OFFSET;
  entry->param_count = param_count;
  entry->patches = NULL;
  entry->patch_count = 0;
  entry->patch_capacity = 0;
  entry->is_extern = is_extern;
  entry->returns_ref = returns_ref;
  entry->return_type = return_type;
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

void free_enum_table(EnumTable* table) {
  if (!table) return;
  for (size_t i = 0; i < table->count; i++) {
    free(table->entries[i].members);
  }
  free(table->entries);
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
}

EnumEntry* enum_table_find(EnumTable* table, Str name) {
  if (!table) return NULL;
  for (size_t i = 0; i < table->count; i++) {
    if (str_matches(table->entries[i].name, name)) return &table->entries[i];
  }
  return NULL;
}

bool enum_table_add(EnumTable* table, Str name, Str* members, size_t member_count) {
  if (enum_table_find(table, name)) return true;
  if (table->count + 1 > table->capacity) {
    size_t new_cap = table->capacity < 4 ? 4 : table->capacity * 2;
    EnumEntry* resized = realloc(table->entries, new_cap * sizeof(EnumEntry));
    if (!resized) return false;
    table->entries = resized;
    table->capacity = new_cap;
  }
  EnumEntry* entry = &table->entries[table->count++];
  entry->name = name;
  entry->members = members;
  entry->member_count = member_count;
  return true;
}

int enum_entry_find_member(const EnumEntry* entry, Str name) {
  for (size_t i = 0; i < entry->member_count; i++) {
    if (str_matches(entry->members[i], name)) return (int)i;
  }
  return -1;
}

static bool is_primitive_type(Str type_name) {
  return str_eq_cstr(type_name, "Int") || 
         str_eq_cstr(type_name, "Float") || 
         str_eq_cstr(type_name, "Bool") || 
         str_eq_cstr(type_name, "String");
}

bool collect_metadata(const char* file_path, const AstModule* module, FunctionTable* funcs, TypeTable* types, EnumTable* enums) {
  if (!module) return true;

  // Process imports first
  for (const AstImport* imp = module->imports; imp; imp = imp->next) {
      if (!collect_metadata(file_path, imp->module, funcs, types, enums)) {
          return false;
      }
  }

  const AstDecl* decl = module->decls;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC) {
      // ... (func handling remains same) ...
      uint16_t param_count = 0;
      const AstParam* p = decl->as.func_decl.params;
      while (p) {
        param_count++;
        p = p->next;
      }

      Str* param_types = NULL;
      if (param_count > 0) {
        param_types = malloc(param_count * sizeof(Str));
        p = decl->as.func_decl.params;
        for (uint16_t i = 0; i < param_count; ++i) {
          if (p->type->is_opt && (p->type->is_view || p->type->is_mod)) {
            diag_error(file_path, (int)p->type->line, (int)p->type->column, "opt view/mod not allowed");
            free(param_types);
            return false;
          }
          param_types[i] = get_base_type_name(p->type);
          p = p->next;
        }
      }

      bool returns_ref = decl->as.func_decl.returns && (decl->as.func_decl.returns->type->is_view || decl->as.func_decl.returns->type->is_mod);
      Str return_type = (Str){0};
      if (decl->as.func_decl.returns) {
          if (decl->as.func_decl.returns->type->is_opt && (decl->as.func_decl.returns->type->is_view || decl->as.func_decl.returns->type->is_mod)) {
            diag_error(file_path, (int)decl->as.func_decl.returns->type->line, (int)decl->as.func_decl.returns->type->column, "opt view/mod not allowed");
            if (param_types) free(param_types);
            return false;
          }
          return_type = get_base_type_name(decl->as.func_decl.returns->type);
      }
      bool ok = function_table_add(funcs, decl->as.func_decl.name, param_types, param_count, decl->as.func_decl.is_extern, returns_ref, return_type);
      free(param_types);
      if (!ok) return false;
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
        if (f->type->is_opt && (f->type->is_view || f->type->is_mod)) {
          diag_error(file_path, (int)f->type->line, (int)f->type->column, "opt view/mod not allowed");
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
    } else if (decl->kind == AST_DECL_ENUM) {
      size_t member_count = 0;
      const AstEnumMember* m = decl->as.enum_decl.members;
      while (m) { member_count++; m = m->next; }
      
      Str* members = malloc(member_count * sizeof(Str));
      m = decl->as.enum_decl.members;
      for (size_t i = 0; i < member_count; i++) {
        members[i] = m->name;
        m = m->next;
      }
      if (!enum_table_add(enums, decl->as.enum_decl.name, members, member_count)) {
        free(members);
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
    free(table->entries[i].param_types);
    table->entries[i].patches = NULL;
    table->entries[i].param_types = NULL;
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

static const AstDecl* find_type_decl(const AstModule* module, Str name) {
  for (const AstDecl* decl = module->decls; decl; decl = decl->next) {
    if (decl->kind == AST_DECL_TYPE && str_matches(decl->as.type_decl.name, name)) {
      return decl;
    }
  }
  return NULL;
}

static bool has_property(const AstProperty* props, const char* name) {
  while (props) {
    if (str_eq_cstr(props->name, name)) return true;
    props = props->next;
  }
  return false;
}

static Str get_local_type_name(BytecodeCompiler* compiler, Str name) {
  for (int i = compiler->local_count - 1; i >= 0; i--) {
    if (str_matches(compiler->locals[i].name, name)) {
      return compiler->locals[i].type_name;
    }
  }
  return (Str){0};
}

static Str get_generic_arg(Str type_name) {
  for (size_t i = 0; i < type_name.len; ++i) {
    if (type_name.data[i] == '(') {
      Str res = { .data = type_name.data + i + 1, .len = type_name.len - i - 2 };
      return res;
    }
  }
  return (Str){0};
}

static Str infer_expr_type(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (!expr) return (Str){0};
  switch (expr->kind) {
    case AST_EXPR_IDENT:
      return get_local_type_name(compiler, expr->as.ident);
    case AST_EXPR_INTEGER: return str_from_cstr("Int");
    case AST_EXPR_FLOAT: return str_from_cstr("Float");
    case AST_EXPR_BOOL: return str_from_cstr("Bool");
    case AST_EXPR_STRING: return str_from_cstr("String");
    case AST_EXPR_CHAR: return str_from_cstr("Char");
    case AST_EXPR_LIST:
    case AST_EXPR_COLLECTION_LITERAL: {
        // If it's a collection literal, we often know if it's a List
        // (Simplified for now)
        return str_from_cstr("List");
    }
    case AST_EXPR_OBJECT: {
        if (expr->as.object_literal.type && expr->as.object_literal.type->parts) {
            return expr->as.object_literal.type->parts->text;
        }
        break;
    }
    case AST_EXPR_CALL: {
        if (expr->as.call.callee->kind == AST_EXPR_IDENT) {
            Str name = expr->as.call.callee->as.ident;
            if (str_eq_cstr(name, "rae_str") || str_eq_cstr(name, "rae_str_concat") || str_eq_cstr(name, "rae_str_sub")) {
                return str_from_cstr("String");
            }
            if (str_eq_cstr(name, "createList")) return str_from_cstr("List");
            
            // Calculate args for overload matching
            uint16_t arg_count = 0;
            for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) arg_count++;
            
            Str* arg_types = NULL;
            if (arg_count > 0) {
                arg_types = malloc(arg_count * sizeof(Str));
                const AstCallArg* arg = expr->as.call.args;
                for (uint16_t i = 0; i < arg_count; i++) {
                    arg_types[i] = infer_expr_type(compiler, arg->value);
                    arg = arg->next;
                }
            }
            
            FunctionEntry* entry = function_table_find_overload(&compiler->functions, name, arg_types, arg_count);
            free(arg_types);
            
            if (entry) {
                // If it's a generic List function returning T, we should return generic type
                if (str_eq_cstr(entry->return_type, "T")) {
                    // Try to infer T from first arg (receiver) if it's a List(T)
                    if (arg_count > 0) {
                        Str first_arg_type = infer_expr_type(compiler, expr->as.call.args->value);
                        return get_generic_arg(first_arg_type);
                    }
                }
                return entry->return_type;
            }
        }
        break;
    }
    case AST_EXPR_METHOD_CALL: {
        Str method = expr->as.method_call.method_name;
        if (str_eq_cstr(method, "length")) return str_from_cstr("Int");
        if (str_eq_cstr(method, "nowMs")) return str_from_cstr("Int");
        if (str_eq_cstr(method, "get") || str_eq_cstr(method, "pop")) {
            Str receiver_type = infer_expr_type(compiler, expr->as.method_call.object);
            return get_generic_arg(receiver_type);
        }
        break;
    }
    case AST_EXPR_MEMBER: {
        Str obj_type = infer_expr_type(compiler, expr->as.member.object);
        if (obj_type.len == 0) return (Str){0};
        
        Str base_type = get_base_type_name_str(obj_type);
        TypeEntry* type = type_table_find(&compiler->types, base_type);
        if (type) {
            int field_idx = type_entry_find_field(type, expr->as.member.member);
            if (field_idx >= 0) {
                return get_base_type_name(type->field_types[field_idx]);
            }
        }
        
        // Fallback for some common member types if possible
        if (str_eq_cstr(expr->as.member.member, "length")) return str_from_cstr("Int");
        break;
    }
    case AST_EXPR_INDEX: {
        Str target_type = infer_expr_type(compiler, expr->as.index.target);
        return get_generic_arg(target_type);
    }
    case AST_EXPR_INTERP: return str_from_cstr("String");
    default: break;
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

static bool emit_flattened_struct_args(BytecodeCompiler* compiler, const AstExpr* root_expr, Str type_name, uint16_t* total_args, int line) {
    const AstDecl* type_decl = find_type_decl(compiler->module, type_name);
    if (!type_decl || type_decl->kind != AST_DECL_TYPE || !has_property(type_decl->as.type_decl.properties, "c_struct")) {
        // Not a c_struct or not found, just push as a single value
        if (!compile_expr(compiler, root_expr)) return false;
        (*total_args)++;
        return true;
    }

    const AstTypeField* field = type_decl->as.type_decl.fields;
    int field_idx = 0;
    while (field) {
        Str field_type = get_base_type_name(field->type);
        const AstDecl* field_type_decl = find_type_decl(compiler->module, field_type);
        
        bool is_nested_struct = (field_type_decl && field_type_decl->kind == AST_DECL_TYPE && has_property(field_type_decl->as.type_decl.properties, "c_struct"));

        if (is_nested_struct) {
            // Nested struct!
            if (!compile_expr(compiler, root_expr)) return false;
            emit_op(compiler, OP_GET_FIELD, line);
            emit_short(compiler, (uint16_t)field_idx, line);
            
            char temp_name[64];
            snprintf(temp_name, sizeof(temp_name), "__flatten_tmp_%d_%d", line, field_idx);
            int slot = compiler_add_local(compiler, str_from_cstr(temp_name), field_type);
            if (slot < 0) return false;
            if (!compiler_ensure_local_capacity(compiler, compiler->local_count, line)) return false;
            
            emit_op(compiler, OP_SET_LOCAL, line);
            emit_short(compiler, (uint16_t)slot, line);
            emit_op(compiler, OP_POP, line); // Return from SET_LOCAL
            
            AstExpr tmp_ident = { .kind = AST_EXPR_IDENT, .line = (size_t)line, .as = { .ident = str_from_cstr(temp_name) } };
            if (!emit_flattened_struct_args(compiler, &tmp_ident, field_type, total_args, line)) return false;
        } else {
            // Primitive or regular field
            if (!compile_expr(compiler, root_expr)) return false;
            emit_op(compiler, OP_GET_FIELD, line);
            emit_short(compiler, (uint16_t)field_idx, line);
            (*total_args)++;
        }
        
        field = field->next;
        field_idx++;
    }
    return true;
}

static bool compile_call(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (expr->as.call.callee->kind != AST_EXPR_IDENT) {
  
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
               "VM currently only supports direct function calls");
    compiler->had_error = true;
    return false;
  }

  uint16_t arg_count = 0;
  {
    const AstCallArg* arg = expr->as.call.args;
    while (arg) {
      arg_count++;
      arg = arg->next;
    }
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
    const AstCallArg* arg = expr->as.call.args;
    while (arg) {
      if (!compile_expr(compiler, arg->value)) return false;
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

  FunctionEntry* entry = NULL;
  
  Str* arg_types = NULL;
  if (arg_count > 0) {
    arg_types = malloc(arg_count * sizeof(Str));
    const AstCallArg* arg = expr->as.call.args;
    for (uint16_t i = 0; i < arg_count; ++i) {
      arg_types[i] = infer_expr_type(compiler, arg->value);
      arg = arg->next;
    }
  }

  entry = function_table_find_overload(&compiler->functions, name, arg_types, arg_count);
  free(arg_types);

  if (!entry) {
    // Raylib fallback for build mode where imports might not be fully populated
    const char* raylib_funcs[] = {
        "initWindow", "windowShouldClose", "closeWindow", "beginDrawing", "endDrawing",
        "setTargetFPS", "getScreenWidth", "getScreenHeight", "isKeyDown", "isKeyPressed", "clearBackground", 
        "loadTexture", "unloadTexture", "drawTexture", "drawTextureEx",
        "drawRectangle", "drawRectangleLines", "drawCircle", "drawText", "drawCube", "drawCubeWires",
        "drawSphere", "drawCylinder", "drawGrid", "beginMode3D", "endMode3D",
        "getTime", "colorFromHSV", "random", "random_int", "seed", NULL
    };
    bool found_raylib = false;
    for (int i = 0; raylib_funcs[i]; i++) {
        if (str_eq_cstr(name, raylib_funcs[i])) { found_raylib = true; break; }
    }
    
    if (found_raylib) {
        // Create a temporary entry for the native call
        emit_op(compiler, OP_NATIVE_CALL, (int)expr->line);
        uint16_t name_idx = chunk_add_constant(compiler->chunk, value_string_copy(name.data, name.len));
        emit_short(compiler, name_idx, (int)expr->line);
        emit_byte(compiler, (uint8_t)arg_count, (int)expr->line);
        return true;
    }

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "unknown function '%.*s' for VM call", (int)name.len,
             name.data);
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
    compiler->had_error = true;
    return false;
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

  const AstCallArg* arg = expr->as.call.args;
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
        // STRUCT-TO-STRUCT FFI (VM Flattening)
        // If it's a native call, check if the argument type is a c_struct
        bool handled = false;
        if (entry->is_extern) {
            Str type_name = infer_expr_type(compiler, arg->value);
            if (type_name.len == 0) {
                // Try fallback to expected parameter type
                type_name = entry->param_types[current_arg_idx];
            }
            
            const AstDecl* type_decl = find_type_decl(compiler->module, type_name);
            if (type_decl && type_decl->kind == AST_DECL_TYPE && has_property(type_decl->as.type_decl.properties, "c_struct")) {
                // Flatten! Push each field (recursively)
                uint16_t flattened_count = 0;
                if (!emit_flattened_struct_args(compiler, arg->value, type_name, &flattened_count, (int)expr->line)) return false;
                arg_count = (uint16_t)(arg_count + flattened_count - 1);
                handled = true;
            }
        }

        if (!handled) {
            if (!compile_expr(compiler, arg->value)) {
              return false;
            }
        }
    }
    arg = arg->next;
    current_arg_idx++;
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
          // Current stack has LHS. Decide if LHS needs wrapping.
          // Note: In the first iteration, LHS is the initial string part.
          // In subsequent iterations, it's the result of the previous rae_str_concat (String).
          
          // Push RHS
          if (!compile_expr(compiler, part->value)) return false;
          
          Str rhs_type = infer_expr_type(compiler, part->value);
          
          // If the next value isn't a string (it's the {expression} result), wrap it in rae_str
          if (!str_eq_cstr(rhs_type, "String")) {
              emit_op(compiler, OP_NATIVE_CALL, (int)expr->line);
              const char* native_name = "rae_str";
              Str name_str = str_from_cstr(native_name);
              uint16_t name_idx = chunk_add_constant(compiler->chunk, value_string_copy(name_str.data, name_str.len));
              emit_short(compiler, name_idx, (int)expr->line);
              emit_byte(compiler, 1, (int)expr->line); // 1 arg
          }
          
          // Now stack has [LHS, RHS_str]. Call rae_str_concat
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
      if (expr->as.member.object->kind == AST_EXPR_IDENT) {
          Str obj_name = expr->as.member.object->as.ident;
          // Check if it's an enum member access (e.g. Color.RED)
          EnumEntry* en = enum_table_find(&compiler->enums, obj_name);
          if (en) {
              int member_idx = enum_entry_find_member(en, expr->as.member.member);
              if (member_idx < 0) {
                  char buffer[128];
                  snprintf(buffer, sizeof(buffer), "enum '%.*s' has no member '%.*s'", 
                           (int)obj_name.len, obj_name.data,
                           (int)expr->as.member.member.len, expr->as.member.member.data);
                  diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
                  compiler->had_error = true;
                  return false;
              }
              emit_constant(compiler, value_int(member_idx), (int)expr->line);
              return true;
          }
      }

      Str obj_type_raw = infer_expr_type(compiler, expr->as.member.object);
      Str type_name = get_base_type_name_str(obj_type_raw);
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
      Str type_name = {0};
      if (expr->as.object_literal.type && expr->as.object_literal.type->parts) {
          type_name = expr->as.object_literal.type->parts->text;
      } else {
          type_name = compiler->expected_type;
      }
      
      TypeEntry* type = type_name.len > 0 ? type_table_find(&compiler->types, type_name) : NULL;
      
      if (type) {
          // Reorder fields according to type definition
          Str saved_expected = compiler->expected_type;
          for (size_t i = 0; i < type->field_count; i++) {
              Str expected_name = type->field_names[i];
              compiler->expected_type = get_base_type_name(type->field_types[i]);
              
              const AstObjectField* f = expr->as.object_literal.fields;
              bool found = false;
              while (f) {
                  if (str_matches(f->name, expected_name)) {
                      if (!compile_expr(compiler, f->value)) return false;
                      found = true;
                      break;
                  }
                  f = f->next;
              }
              if (!found) {
                  // Push default value if field is missing in literal
                  if (!emit_default_value(compiler, type->field_types[i], (int)expr->line)) {
                      return false;
                  }
              }
          }
          compiler->expected_type = saved_expected;
          emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
          emit_short(compiler, (uint16_t)type->field_count, (int)expr->line);
      } else {
          // Untyped or unknown type: push in order of appearance
          uint16_t count = 0;
          const AstObjectField* f = expr->as.object_literal.fields;
          while (f) {
            if (!compile_expr(compiler, f->value)) return false;
            count++;
            f = f->next;
          }
          emit_op(compiler, OP_CONSTRUCT, (int)expr->line);
          emit_short(compiler, count, (int)expr->line);
      }
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
      
      // Calculate total arguments (1 receiver + explicit args)
      uint16_t explicit_args_count = 0;
      for (const AstCallArg* arg = expr->as.method_call.args; arg; arg = arg->next) explicit_args_count++;
      uint16_t total_arg_count = 1 + explicit_args_count;

      // Infer all types for dispatch
      Str* arg_types = malloc(total_arg_count * sizeof(Str));
      arg_types[0] = get_base_type_name_str(infer_expr_type(compiler, expr->as.method_call.object));
      {
          const AstCallArg* arg = expr->as.method_call.args;
          for (uint16_t i = 0; i < explicit_args_count; ++i) {
              arg_types[i + 1] = infer_expr_type(compiler, arg->value);
              arg = arg->next;
          }
      }

      entry = function_table_find_overload(&compiler->functions, method_name, arg_types, total_arg_count);
      free(arg_types);

      // Compile the receiver ('this')
      const AstExpr* receiver = expr->as.method_call.object;
      
      // We need to decide whether to pass the receiver as a reference or a value.
      // For now, in the VM, we often prefer mod reference for potential mutation
      // unless it's a primitive.
      if (receiver->kind == AST_EXPR_IDENT) {
          int slot = compiler_find_local(compiler, receiver->as.ident);
          if (slot >= 0) {
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
        current_arg = current_arg->next;
      }

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
      Str target_type = infer_expr_type(compiler, expr->as.index.target);
      if (!compile_expr(compiler, expr->as.index.target)) return false;
      if (!compile_expr(compiler, expr->as.index.index)) return false;
      // For index, we fallback to 'get' for the specific type
      Str param_types[] = { target_type, str_from_cstr("Int") };
      FunctionEntry* entry = function_table_find_overload(&compiler->functions, str_from_cstr("get"), param_types, 2);
      if (!entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'get' method not found for indexing this type");
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

          Str int_type = str_from_cstr("Int");
    FunctionEntry* create_entry = function_table_find_overload(&compiler->functions, str_from_cstr("createList"), &int_type, 1);
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

          Str add_types[] = { str_from_cstr("List"), str_from_cstr("Any") };
          FunctionEntry* add_entry = function_table_find_overload(&compiler->functions, str_from_cstr("add"), add_types, 2);
          if (!add_entry) {
              diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'add' not found in core.rae");
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

      Str int_type = str_from_cstr("Int");
    FunctionEntry* create_entry = function_table_find_overload(&compiler->functions, str_from_cstr("createList"), &int_type, 1);
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

      Str add_types[] = { str_from_cstr("List"), str_from_cstr("Any") };
      FunctionEntry* add_entry = function_table_find_overload(&compiler->functions, str_from_cstr("add"), add_types, 2);
      if (!add_entry) {
          diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "built-in 'add' not found in core.rae");
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
    case AST_STMT_LET: return "let";
    case AST_STMT_DESTRUCT: return "destructure";
    case AST_STMT_EXPR: return "expression";
    case AST_STMT_RET: return "ret";
    case AST_STMT_IF: return "if";
    case AST_STMT_LOOP: return "loop";
    case AST_STMT_MATCH: return "match";
    case AST_STMT_ASSIGN: return "assignment";
    case AST_STMT_DEFER: return "defer";
  }
  return "unknown";
}

static bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line);

static bool emit_lvalue_ref(BytecodeCompiler* compiler, const AstExpr* expr) {
    if (expr->kind == AST_EXPR_IDENT) {
        int slot = compiler_find_local(compiler, expr->as.ident);
        if (slot < 0) {
            diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "unknown identifier for reference");
            return false;
        }
        emit_op(compiler, OP_MOD_LOCAL, (int)expr->line);
        emit_short(compiler, (uint16_t)slot, (int)expr->line);
        return true;
    } else if (expr->kind == AST_EXPR_MEMBER) {
        if (!emit_lvalue_ref(compiler, expr->as.member.object)) return false;
        
        Str obj_type_raw = infer_expr_type(compiler, expr->as.member.object);
        Str obj_type = get_base_type_name_str(obj_type_raw);
        TypeEntry* type = type_table_find(&compiler->types, obj_type);
        if (!type) {
            char buffer[128];
            snprintf(buffer, sizeof(buffer), "unknown type '%.*s' for member reference", (int)obj_type.len, obj_type.data);
            diag_error(compiler->file_path, (int)expr->line, (int)expr->column, buffer);
            return false;
        }
        
        int field_index = type_entry_find_field(type, expr->as.member.member);
        if (field_index < 0) {
            diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "unknown field for reference");
            return false;
        }
        
        emit_op(compiler, OP_MOD_FIELD, (int)expr->line);
        emit_short(compiler, (uint16_t)field_index, (int)expr->line);
        return true;
    }
    return false;
}

static bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt) {
  if (!stmt) return true;
  switch (stmt->kind) {
    case AST_STMT_LET: {
      Str type_name = get_base_type_name(stmt->as.let_stmt.type);

      int slot = compiler_add_local(compiler, stmt->as.let_stmt.name, type_name);
      if (slot < 0) {
        return false;
      }
      if (!compiler_ensure_local_capacity(compiler, compiler->local_count, (int)stmt->line)) {
        return false;
      }

      if (!stmt->as.let_stmt.value) {
        // Automatically initialize to default value
        if (!emit_default_value(compiler, stmt->as.let_stmt.type, (int)stmt->line)) {
          return false;
        }
        emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
        emit_short(compiler, (uint16_t)slot, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line);
      } else if (stmt->as.let_stmt.is_bind) {
          if (!stmt->as.let_stmt.type || 
              (!stmt->as.let_stmt.type->is_view && 
               !stmt->as.let_stmt.type->is_mod && 
               !stmt->as.let_stmt.type->is_opt)) {
              diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "=> not allowed for plain value types");
              return false;
          }
          // If the RHS is an identifier or member, we can emit a specific VIEW/MOD instruction
          // to get its address.
          if (stmt->as.let_stmt.value->kind == AST_EXPR_IDENT) {
              int src_slot = compiler_find_local(compiler, stmt->as.let_stmt.value->as.ident);
              emit_op(compiler, stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_view ? OP_VIEW_LOCAL : OP_MOD_LOCAL, (int)stmt->line);
              emit_short(compiler, (uint16_t)src_slot, (int)stmt->line);
          } else if (stmt->as.let_stmt.value->kind == AST_EXPR_MEMBER) {
              const AstExpr* member_expr = stmt->as.let_stmt.value;
              if (member_expr->as.member.object->kind == AST_EXPR_IDENT) {
                  Str obj_name = member_expr->as.member.object->as.ident;
                  Str type_name = get_local_type_name(compiler, obj_name);
                  TypeEntry* type = type_table_find(&compiler->types, type_name);
                  if (type) {
                      int field_index = type_entry_find_field(type, member_expr->as.member.member);
                      if (field_index >= 0) {
                          int obj_slot = compiler_find_local(compiler, obj_name);
                          emit_op(compiler, stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_view ? OP_VIEW_LOCAL : OP_MOD_LOCAL, (int)stmt->line);
                          emit_short(compiler, (uint16_t)obj_slot, (int)stmt->line);
                          emit_op(compiler, stmt->as.let_stmt.type && stmt->as.let_stmt.type->is_view ? OP_VIEW_FIELD : OP_MOD_FIELD, (int)stmt->line);
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
              if (!compile_expr(compiler, stmt->as.let_stmt.value)) return false;
              
              bool already_ref = false;
              if (stmt->as.let_stmt.value->kind == AST_EXPR_CALL) {
                  const AstExpr* callee = stmt->as.let_stmt.value->as.call.callee;
                  if (callee->kind == AST_EXPR_IDENT) {
                      Str name = callee->as.ident;
                      FunctionEntry* entry = function_table_find(&compiler->functions, name);
                      if (entry && entry->returns_ref) {
                          already_ref = true;
                      }
                  }
              } else if (stmt->as.let_stmt.value->kind == AST_EXPR_METHOD_CALL) {
                  Str method_name = stmt->as.let_stmt.value->as.method_call.method_name;
                  FunctionEntry* entry = function_table_find(&compiler->functions, method_name);
                  if (!entry) {
                      // Try common list methods
                      if (str_eq_cstr(method_name, "add")) entry = function_table_find(&compiler->functions, str_from_cstr("rae_list_add"));
                      else if (str_eq_cstr(method_name, "get")) entry = function_table_find(&compiler->functions, str_from_cstr("rae_list_get"));
                  }
                  if (entry && entry->returns_ref) {
                      already_ref = true;
                  }
              } else if (stmt->as.let_stmt.value->kind == AST_EXPR_NONE) {
                  already_ref = true;
              }
              
              if (!already_ref) {
                  diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "cannot bind reference (=>) to a value; RHS must be a reference or a function returning one");
                  return false;
              }
          }
          emit_op(compiler, OP_BIND_LOCAL, (int)stmt->line);
          emit_short(compiler, (uint16_t)slot, (int)stmt->line);
      } else {
          Str saved_expected = compiler->expected_type;
          compiler->expected_type = type_name;
          if (!compile_expr(compiler, stmt->as.let_stmt.value)) {
            compiler->expected_type = saved_expected;
            return false;
          }
          compiler->expected_type = saved_expected;
          emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
          emit_short(compiler, (uint16_t)slot, (int)stmt->line);
          emit_op(compiler, OP_POP, (int)stmt->line);
      }
      
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
      
      uint16_t scope_start_locals = compiler->local_count;

      if (!compile_expr(compiler, stmt->as.if_stmt.condition)) {
        return false;
      }
      uint16_t else_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, (int)stmt->line);
      emit_op(compiler, OP_POP, (int)stmt->line);
      
      if (!compile_block(compiler, stmt->as.if_stmt.then_block)) {
        return false;
      }
      
      // Reset local count for scope reuse
      compiler->local_count = scope_start_locals;
      
      uint16_t end_jump = emit_jump(compiler, OP_JUMP, (int)stmt->line);
      
      patch_jump(compiler, else_jump);
      emit_op(compiler, OP_POP, (int)stmt->line);
      
      if (stmt->as.if_stmt.else_block) {
        if (!compile_block(compiler, stmt->as.if_stmt.else_block)) {
          return false;
        }
        
        // Reset local count for scope reuse
        compiler->local_count = scope_start_locals;
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
        Str var_name = stmt->as.loop_stmt.init->as.let_stmt.name;
        Str var_type = get_base_type_name(stmt->as.loop_stmt.init->as.let_stmt.type);
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
      
      // Reset local count for scope reuse
      compiler->local_count = scope_start_locals;
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
            diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "rebinding an alias is illegal. '=>' is only for 'let' bindings.");
            return false;
        } else {
            // Normal assignment: LHS = RHS
            Str saved_expected = compiler->expected_type;
            compiler->expected_type = get_local_type_name(compiler, target->as.ident);
            if (!compile_expr(compiler, stmt->as.assign_stmt.value)) {
                compiler->expected_type = saved_expected;
                return false;
            }
            compiler->expected_type = saved_expected;
            emit_op(compiler, OP_SET_LOCAL, (int)stmt->line);
            emit_short(compiler, (uint16_t)slot, (int)stmt->line);
            emit_op(compiler, OP_POP, (int)stmt->line); // assigned value
        }
        return true;
      } else if (target->kind == AST_EXPR_MEMBER) {
        if (stmt->as.assign_stmt.is_bind) {
            diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "rebinding an alias is illegal. '=>' is only for 'let' bindings.");
            return false;
        }

        // 1. Get reference to the parent object
        if (!emit_lvalue_ref(compiler, target->as.member.object)) return false;

        // 2. Resolve the field index and type
        Str obj_type_raw = infer_expr_type(compiler, target->as.member.object);
        Str obj_type = get_base_type_name_str(obj_type_raw);
        TypeEntry* type = type_table_find(&compiler->types, obj_type);
        if (!type) {
          char buffer[128];
          snprintf(buffer, sizeof(buffer), "unknown type '%.*s' for member assignment", (int)obj_type.len, obj_type.data);
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, buffer);
          return false;
        }
        int field_index = type_entry_find_field(type, target->as.member.member);
        if (field_index < 0) {
          diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "unknown field for assignment");
          return false;
        }

        // 3. Compile the value to be assigned
        Str field_type = get_base_type_name(type->field_types[field_index]);
        Str saved_expected = compiler->expected_type;
        compiler->expected_type = field_type;
        if (!compile_expr(compiler, stmt->as.assign_stmt.value)) {
            compiler->expected_type = saved_expected;
            return false;
        }
        compiler->expected_type = saved_expected;

        // 4. Set the field
        emit_op(compiler, OP_SET_FIELD, (int)stmt->line);
        emit_short(compiler, (uint16_t)field_index, (int)stmt->line);
        emit_op(compiler, OP_POP, (int)stmt->line); // assigned value
        return true;
      } else {
        diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column,
                   "VM currently only supports assignment to identifiers or members");
        compiler->had_error = true;
        return false;
      }
    }
    case AST_STMT_DEFER:
      diag_error(compiler->file_path, (int)stmt->line, (int)stmt->column, "defer not supported in VM yet");
      return false;
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
  
  uint16_t param_count = 0;
  for (const AstParam* p = func->params; p; p = p->next) param_count++;
  
  Str* param_types = NULL;
  if (param_count > 0) {
    param_types = malloc(param_count * sizeof(Str));
    const AstParam* p = func->params;
    for (uint16_t i = 0; i < param_count; ++i) {
      param_types[i] = get_base_type_name(p->type);
      p = p->next;
    }
  }

  FunctionEntry* entry = function_table_find_overload(&compiler->functions, func->name, param_types, param_count);
  free(param_types);

  if (!entry) {
    diag_error(compiler->file_path, (int)decl->line, (int)decl->column,
               "function table entry missing during VM compilation");
    compiler->had_error = true;
    return false;
  }
  if (entry->offset != INVALID_OFFSET) {
    return true; // Already compiled
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
  
  if (module->had_error) {
      return false;
  }

  chunk_init(chunk);
  BytecodeCompiler compiler = {
      .chunk = chunk,
      .module = module,
      .file_path = file_path,
      .had_error = false,
      .current_function = NULL,
      .local_count = 0,
      .allocated_locals = 0,
      .expected_type = {0},
  };
  memset(&compiler.functions, 0, sizeof(FunctionTable));
  memset(&compiler.types, 0, sizeof(TypeTable));
  memset(&compiler.methods, 0, sizeof(MethodTable));
  memset(&compiler.enums, 0, sizeof(EnumTable));

  if (!collect_metadata(file_path, module, &compiler.functions, &compiler.types, &compiler.enums)) {
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
  free_enum_table(&compiler.enums);
  // Method table free not yet implemented but should be added when used.

  return success;
}

static bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line) {
  if (!type) {
    emit_constant(compiler, value_int(0), line);
    return true;
  }

  if (type->is_opt) {
    emit_constant(compiler, value_none(), line);
    return true;
  }

  if (!type->parts) {
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
