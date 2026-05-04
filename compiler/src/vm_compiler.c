#include "vm_compiler.h"
#include "vm_compiler_internal.h"

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

bool compiler_ensure_local_capacity(BytecodeCompiler* compiler, uint32_t required,
                                           int line);
uint32_t emit_jump(BytecodeCompiler* compiler, OpCode op, int line);
void patch_jump(BytecodeCompiler* compiler, uint32_t offset);
bool compile_block(BytecodeCompiler* compiler, const AstBlock* block);
bool emit_native_call(BytecodeCompiler* compiler, Str name, uint8_t arg_count, int line,
                             int column);
bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line);
bool vm_emit_defers(BytecodeCompiler* compiler, int min_depth);
void vm_pop_defers(BytecodeCompiler* compiler, int depth);

void emit_byte(BytecodeCompiler* compiler, uint8_t byte, int line) {
  chunk_write(compiler->chunk, byte, line);
}

void emit_op(BytecodeCompiler* compiler, OpCode op, int line) {
  emit_byte(compiler, (uint8_t)op, line);
}

void emit_uint32(BytecodeCompiler* compiler, uint32_t value, int line) {
  emit_byte(compiler, (uint8_t)((value >> 24) & 0xFF), line);
  emit_byte(compiler, (uint8_t)((value >> 16) & 0xFF), line);
  emit_byte(compiler, (uint8_t)((value >> 8) & 0xFF), line);
  emit_byte(compiler, (uint8_t)(value & 0xFF), line);
}

void emit_constant(BytecodeCompiler* compiler, Value value, int line) {
  uint32_t index = chunk_add_constant(compiler->chunk, value);
  emit_op(compiler, OP_CONSTANT, line);
  emit_uint32(compiler, index, line);
}

bool vm_emit_defers(BytecodeCompiler* compiler, int min_depth) {
  // We must execute defers in reverse order of their addition.
  // Note: we don't pop them here because multiple return paths might need them.
  for (int i = compiler->defer_stack.count - 1; i >= 0; i--) {
    if (compiler->defer_stack.entries[i].scope_depth >= min_depth) {
      if (!compile_block(compiler, compiler->defer_stack.entries[i].block)) {
        return false;
      }
    }
  }
  return true;
}

void vm_pop_defers(BytecodeCompiler* compiler, int depth) {
  while (compiler->defer_stack.count > 0 && 
         compiler->defer_stack.entries[compiler->defer_stack.count - 1].scope_depth >= depth) {
    compiler->defer_stack.count--;
  }
}

static void write_uint32_at(Chunk* chunk, uint32_t offset, uint32_t value) {
  if (offset + 3 >= chunk->code_count) return;
  chunk->code[offset] = (uint8_t)((value >> 24) & 0xFF);
  chunk->code[offset + 1] = (uint8_t)((value >> 16) & 0xFF);
  chunk->code[offset + 2] = (uint8_t)((value >> 8) & 0xFF);
  chunk->code[offset + 3] = (uint8_t)(value & 0xFF);
}

uint32_t emit_jump(BytecodeCompiler* compiler, OpCode op, int line) {
  emit_op(compiler, op, line);
  uint32_t offset = (uint32_t)compiler->chunk->code_count;
  emit_uint32(compiler, 0, line);
  return offset;
}

void patch_jump(BytecodeCompiler* compiler, uint32_t offset) {
  uint32_t jump = (uint32_t)compiler->chunk->code_count;
  write_uint32_at(compiler->chunk, offset, jump);
}


bool str_matches(Str a, Str b) {
  return a.len == b.len && strncmp(a.data, b.data, a.len) == 0;
}

bool is_stdlib_module(Str name) {
    return str_eq_cstr(name, "core") || 
           str_eq_cstr(name, "math") || 
           str_eq_cstr(name, "io") || 
           str_eq_cstr(name, "string") || 
           str_eq_cstr(name, "sys") ||
           str_eq_cstr(name, "raylib") ||
           str_eq_cstr(name, "time") ||
           str_eq_cstr(name, "easing") ||
           str_eq_cstr(name, "tinyexpr") ||
           str_eq_cstr(name, "list2");
}

bool is_module_import(BytecodeCompiler* compiler, Str name) {
    if (is_stdlib_module(name)) return true;
    if (!compiler->module) return false;
    for (const AstImport* imp = compiler->module->imports; imp; imp = imp->next) {
        Str path = imp->path;
        const char* last_slash = NULL;
        for (size_t i = 0; i < path.len; i++) {
            if (path.data[i] == '/' || path.data[i] == '\\') {
                last_slash = &path.data[i];
            }
        }
        
        Str mod_name;
        if (last_slash) {
            mod_name.data = last_slash + 1;
            mod_name.len = (path.data + path.len) - (last_slash + 1);
        } else {
            mod_name = path;
        }
        
        if (str_matches(mod_name, name)) return true;
    }
    return false;
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

bool vm_is_primitive_type(Str type_name) {
  return str_eq_cstr(type_name, "Int64") || 
         str_eq_cstr(type_name, "Int") || 
         str_eq_cstr(type_name, "Float64") || 
         str_eq_cstr(type_name, "Float") || 
         str_eq_cstr(type_name, "Bool") || 
         str_eq_cstr(type_name, "Char") || 
         str_eq_cstr(type_name, "Char32") || 
         str_eq_cstr(type_name, "String") || 
         str_eq_cstr(type_name, "Array") || 
         str_eq_cstr(type_name, "Buffer") || 
         str_eq_cstr(type_name, "Any");
}

Str strip_generics(Str type_name) {
  for (size_t i = 0; i < type_name.len; ++i) {
    if (type_name.data[i] == '(') {
      return (Str){.data = type_name.data, .len = i};
    }
  }
  return type_name;
}

Str get_base_type_name_str(Str type_name) {
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
  } else if (str_starts_with_cstr(res, "val ")) {
    res.data += 4;
    res.len -= 4;
  }
  
  if (str_eq_cstr(res, "Int")) return str_from_cstr("Int64");
  if (str_eq_cstr(res, "Float")) return str_from_cstr("Float64");
  if (str_eq_cstr(res, "Char")) return str_from_cstr("Char32");

  // Check if it's a generic type like List(Int)
  for (size_t i = 0; i < res.len; i++) {
      if (res.data[i] == '(') {
          res.len = i;
          break;
      }
  }

  return res;
}

bool vm_types_match(Str entry_type_raw, Str call_type_raw) {
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
    return vm_types_match(entry_inner, call_inner);
  }
  
  return false;
}

FunctionEntry* function_table_find_overload(FunctionTable* table, Str name, const Str* param_types, uint32_t param_count) {
  if (!table) return NULL;
  
  FunctionEntry* name_match = NULL;

  for (size_t i = 0; i < table->count; ++i) {
    FunctionEntry* entry = &table->entries[i];
    if (str_matches(entry->name, name)) {
      if (entry->param_count == param_count) {
        if (param_types == NULL) return entry;
        
        bool mismatch = false;
        for (uint32_t j = 0; j < param_count; ++j) {
          if (!vm_types_match(entry->param_types[j], param_types[j])) {
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

static FunctionEntry* function_table_find_exact(FunctionTable* table, Str name, const Str* param_types, uint32_t param_count) {
  if (!table) return NULL;
  for (size_t i = 0; i < table->count; ++i) {
    FunctionEntry* entry = &table->entries[i];
    if (str_matches(entry->name, name) && entry->param_count == param_count) {
      bool mismatch = false;
      for (uint32_t j = 0; j < param_count; ++j) {
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

bool function_table_add(CompilerContext* ctx, FunctionTable* table, Str name, Str* param_types, uint32_t param_count, bool is_extern, bool returns_ref, Str return_type) {
  FunctionEntry* existing = function_table_find_exact(table, name, param_types, param_count);
  if (existing) {
    existing->is_extern = is_extern;
    existing->return_type = return_type;
    return true;
  }
  if (table->count + 1 > table->capacity) {
    size_t old_cap = table->capacity;
    size_t new_cap = old_cap < 8 ? 8 : old_cap * 2;
    FunctionEntry* new_entries = arena_alloc(ctx->ast_arena, new_cap * sizeof(FunctionEntry));
    if (table->entries) {
        memcpy(new_entries, table->entries, table->count * sizeof(FunctionEntry));
    }
    table->entries = new_entries;
    table->capacity = new_cap;
  }
  FunctionEntry* entry = &table->entries[table->count++];
  entry->name = name;
  entry->param_types = arena_alloc(ctx->ast_arena, param_count * sizeof(Str));
  for (uint32_t i = 0; i < param_count; ++i) {
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

static bool function_entry_add_patch(FunctionEntry* entry, uint32_t patch_offset) {
  if (entry->patch_count + 1 > entry->patch_capacity) {
    size_t old_cap = entry->patch_capacity;
    size_t new_cap = old_cap < 4 ? 4 : old_cap * 2;
    uint32_t* resized = realloc(entry->patches, new_cap * sizeof(uint32_t));
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
      write_uint32_at(chunk, entry->patches[j], entry->offset);
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

bool type_table_add(CompilerContext* ctx, TypeTable* table, Str name, Str* field_names, const AstTypeRef** field_types, const AstExpr** field_defaults, size_t field_count) {
  if (type_table_find(table, name)) return true;
  if (table->count + 1 > table->capacity) {
    size_t old_cap = table->capacity;
    size_t new_cap = old_cap < 4 ? 4 : old_cap * 2;
    TypeEntry* new_entries = arena_alloc(ctx->ast_arena, new_cap * sizeof(TypeEntry));
    if (table->entries) {
        memcpy(new_entries, table->entries, table->count * sizeof(TypeEntry));
    }
    table->entries = new_entries;
    table->capacity = new_cap;
  }
  TypeEntry* entry = &table->entries[table->count++];
  entry->name = name;
  entry->field_names = field_names;
  entry->field_types = field_types;
  entry->field_defaults = field_defaults;
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

bool enum_table_add(CompilerContext* ctx, EnumTable* table, Str name, Str* members, size_t member_count) {
  if (enum_table_find(table, name)) return true;
  if (table->count + 1 > table->capacity) {
    size_t old_cap = table->capacity;
    size_t new_cap = old_cap < 4 ? 4 : old_cap * 2;
    EnumEntry* new_entries = arena_alloc(ctx->ast_arena, new_cap * sizeof(EnumEntry));
    if (table->entries) {
        memcpy(new_entries, table->entries, table->count * sizeof(EnumEntry));
    }
    table->entries = new_entries;
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

static Str get_type_name_with_refs(const AstTypeRef* type) {
    if (!type) return (Str){0};
    
    char buffer[256];
    int offset = 0;
    Str base = get_base_type_name(type);

    if (type->is_view) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "view ");
    } else if (type->is_mod) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "mod ");
    } else if (type->is_val) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "val ");
    } else if (!vm_is_primitive_type(base)) {
        // Semantically view by default for non-primitives
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "view ");
    }
    
    snprintf(buffer + offset, sizeof(buffer) - offset, "%.*s", (int)base.len, base.data);
    
    return str_dup(str_from_cstr(buffer)); // Note: this leaks in compiler
}

bool collect_metadata(CompilerContext* ctx, const char* file_path, const AstModule* module, FunctionTable* funcs, TypeTable* types, EnumTable* enums, VmRegistry* registry) {

  if (!module) return true;



  // Process imports first

  for (const AstImport* imp = module->imports; imp; imp = imp->next) {

      if (!collect_metadata(ctx, file_path, imp->module, funcs, types, enums, registry)) {

          return false;

      }

  }



    const AstDecl* decl = module->decls;



        while (decl) {



          if (decl->kind == AST_DECL_FUNC) {



            uint32_t param_count = 0;



    



  

      const AstParam* p = decl->as.func_decl.params;

      while (p) {

        param_count++;

        p = p->next;

      }



      Str* param_types = NULL;

      if (param_count > 0) {

        param_types = arena_alloc(ctx->ast_arena, param_count * sizeof(Str));

        p = decl->as.func_decl.params;

        for (uint32_t i = 0; i < param_count; ++i) {

          if (p->type->is_opt && (p->type->is_view || p->type->is_mod)) {

            diag_error(file_path, (int)p->type->line, (int)p->type->column, "opt view/mod not allowed");

            return false;

          }

          param_types[i] = get_type_name_with_refs(p->type);

          p = p->next;

        }

      }



      bool returns_ref = decl->as.func_decl.returns && (decl->as.func_decl.returns->type->is_view || decl->as.func_decl.returns->type->is_mod);

      Str return_type = (Str){0};

      if (decl->as.func_decl.returns) {

          if (decl->as.func_decl.returns->type->is_opt && (decl->as.func_decl.returns->type->is_view || decl->as.func_decl.returns->type->is_mod)) {

            diag_error(file_path, (int)decl->as.func_decl.returns->type->line, (int)decl->as.func_decl.returns->type->column, "opt view/mod not allowed");

            return false;

          }

          return_type = get_type_name_with_refs(decl->as.func_decl.returns->type);

      }

      bool ok = function_table_add(ctx, funcs, decl->as.func_decl.name, param_types, param_count, decl->as.func_decl.is_extern, returns_ref, return_type);

      if (!ok) return false;

    } else if (decl->kind == AST_DECL_TYPE) {

      size_t field_count = 0;

      const AstTypeField* f = decl->as.type_decl.fields;

      while (f) { field_count++; f = f->next; }

      

      Str* field_names = arena_alloc(ctx->ast_arena, field_count * sizeof(Str));

      const AstTypeRef** field_types = arena_alloc(ctx->ast_arena, field_count * sizeof(AstTypeRef*));

      const AstExpr** field_defaults = arena_alloc(ctx->ast_arena, field_count * sizeof(AstExpr*));

      f = decl->as.type_decl.fields;

      for (size_t i = 0; i < field_count; i++) {

        if (f->type->is_view || f->type->is_mod) {

          diag_error(file_path, (int)f->type->line, (int)f->type->column, "view/mod not allowed in struct fields");

          return false;

        }

        if (f->type->is_opt && (f->type->is_view || f->type->is_mod)) {

          diag_error(file_path, (int)f->type->line, (int)f->type->column, "opt view/mod not allowed");

          return false;

        }

        field_names[i] = f->name;

        field_types[i] = f->type;

        field_defaults[i] = f->default_value;

        f = f->next;

      }

      if (!type_table_add(ctx, types, decl->as.type_decl.name, field_names, field_types, field_defaults, field_count)) {

        return false;

      }



      if (registry) {

          char** c_field_names = arena_alloc(ctx->ast_arena, field_count * sizeof(char*));

          char** c_field_types = arena_alloc(ctx->ast_arena, field_count * sizeof(char*));

          for (size_t i = 0; i < field_count; i++) {

              c_field_names[i] = str_to_cstr(field_names[i]);

              c_field_types[i] = str_to_cstr(get_base_type_name(field_types[i]));

          }

          char* type_name_cstr = str_to_cstr(decl->as.type_decl.name);

          vm_registry_add_type_metadata(registry, type_name_cstr, c_field_names, c_field_types, field_count);

          free(type_name_cstr);

          // Note: we let these leak for now or they should be on arena if registry supported it

          // Actually str_to_cstr uses malloc, so we should ideally have arena-based versions.

      }

    } else if (decl->kind == AST_DECL_ENUM) {

      size_t member_count = 0;

      const AstEnumMember* m = decl->as.enum_decl.members;

      while (m) { member_count++; m = m->next; }

      

      Str* members = arena_alloc(ctx->ast_arena, member_count * sizeof(Str));

      m = decl->as.enum_decl.members;

      for (size_t i = 0; i < member_count; i++) {

        members[i] = m->name;

        m = m->next;

      }

      if (!enum_table_add(ctx, enums, decl->as.enum_decl.name, members, member_count)) {

        return false;

      }

    } else if (decl->kind == AST_DECL_GLOBAL_LET) {

        if (registry) {

            Str name = decl->as.let_decl.name;

            Str type_name_str = get_base_type_name(decl->as.let_decl.type);

            vm_registry_ensure_global(registry, name, type_name_str);

        }

    }
    decl = decl->next;
  }
  return true;
}

void free_type_table(TypeTable* table) {
  if (!table) return;
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
  table->entries = NULL;
  table->count = 0;
  table->capacity = 0;
}

void compiler_reset_locals(BytecodeCompiler* compiler) {
  compiler->local_count = 0;
  compiler->allocated_locals = 0;
  compiler->scope_depth = 0;
  compiler->defer_stack.count = 0;
}

int compiler_add_local(BytecodeCompiler* compiler, Str name, Str type_name, bool is_ptr, bool is_mod) {
  if (compiler->local_count >= sizeof(compiler->locals) / sizeof(compiler->locals[0])) {
    diag_error(compiler->file_path, 0, 0, "VM compiler local limit exceeded");
    compiler->had_error = true;
    return -1;
  }
  compiler->locals[compiler->local_count].name = name;
  compiler->locals[compiler->local_count].slot = compiler->local_count;
  compiler->locals[compiler->local_count].type_name = type_name;
  compiler->locals[compiler->local_count].is_ptr = is_ptr;
  compiler->locals[compiler->local_count].is_mod = is_mod;
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

Str vm_get_local_type_name(BytecodeCompiler* compiler, Str name) {
  for (int i = (int)compiler->local_count - 1; i >= 0; i--) {
    if (str_matches(compiler->locals[i].name, name)) {
      return compiler->locals[i].type_name;
    }
  }
  
  // Check for global type in registry
  if (compiler->registry) {
      Str type_name = vm_registry_get_global_type(compiler->registry, name);
      if (type_name.data) {
          return type_name;
      }
  }
  
  return (Str){0};
}

Str get_generic_arg(Str type_name) {
  for (size_t i = 0; i < type_name.len; ++i) {
    if (type_name.data[i] == '(') {
      Str res = { .data = type_name.data + i + 1, .len = type_name.len - i - 2 };
      return res;
    }
  }
  return (Str){0};
}

Str vm_infer_expr_type(BytecodeCompiler* compiler, const AstExpr* expr) {
  if (!expr) return (Str){0};
  switch (expr->kind) {
    case AST_EXPR_IDENT:
      return vm_get_local_type_name(compiler, expr->as.ident);
            case AST_EXPR_INTEGER: return str_from_cstr("Int64");
    case AST_EXPR_FLOAT: return str_from_cstr("Float64");
    
    case AST_EXPR_BOOL: return str_from_cstr("Bool");
    case AST_EXPR_STRING: return str_from_cstr("String");
    case AST_EXPR_CHAR: return str_from_cstr("Char32");
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
            uint32_t arg_count = 0;
            for (const AstCallArg* arg = expr->as.call.args; arg; arg = arg->next) arg_count++;
            
            Str* arg_types = NULL;
            if (arg_count > 0) {
                arg_types = malloc(arg_count * sizeof(Str));
                const AstCallArg* arg = expr->as.call.args;
                for (uint32_t i = 0; i < arg_count; i++) {
                    arg_types[i] = vm_infer_expr_type(compiler, arg->value);
                    arg = arg->next;
                }
            }
            
            FunctionEntry* entry = function_table_find_overload(&compiler->compiler_ctx->functions, name, arg_types, arg_count);
            free(arg_types);
            
            if (entry) {
                // If it's a generic List function returning T, we should return generic type
                if (str_eq_cstr(entry->return_type, "T")) {
                    // Try to infer T from first arg (receiver) if it's a List(T)
                    if (arg_count > 0) {
                        Str first_arg_type = vm_infer_expr_type(compiler, expr->as.call.args->value);
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
        if (str_eq_cstr(method, "length")) return str_from_cstr("Int64");
        if (str_eq_cstr(method, "nowMs")) return str_from_cstr("Int64");
        if (str_eq_cstr(method, "get") || str_eq_cstr(method, "pop")) {
            Str receiver_type = vm_infer_expr_type(compiler, expr->as.method_call.object);
            return get_generic_arg(receiver_type);
        }
        break;
    }
    case AST_EXPR_MEMBER: {
        Str obj_type = vm_infer_expr_type(compiler, expr->as.member.object);
        if (obj_type.len == 0) return (Str){0};
        
        Str base_type = get_base_type_name_str(obj_type);
        TypeEntry* type = type_table_find(&compiler->compiler_ctx->types, base_type);
        if (type) {
            int field_idx = type_entry_find_field(type, expr->as.member.member);
            if (field_idx >= 0) {
                return get_base_type_name(type->field_types[field_idx]);
            }
        }
        
        // Fallback for some common member types if possible
        if (str_eq_cstr(expr->as.member.member, "length")) return str_from_cstr("Int64");
        break;
    }
    case AST_EXPR_INDEX: {
        Str target_type = vm_infer_expr_type(compiler, expr->as.index.target);
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

bool compiler_ensure_local_capacity(BytecodeCompiler* compiler, uint32_t required,
                                           int line) {
  if (required <= compiler->allocated_locals) {
    return true;
  }
  emit_op(compiler, OP_ALLOC_LOCAL, line);
  emit_uint32(compiler, required, line);
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
  uint32_t patch_offset = (uint32_t)compiler->chunk->code_count;
  emit_uint32(compiler, 0, line);
  emit_byte(compiler, arg_count, line);
  if (!function_entry_add_patch(entry, patch_offset)) {
    diag_error(compiler->file_path, line, column, "failed to record function call patch");
    compiler->had_error = true;
    return false;
  }
  return true;
}

bool emit_native_call(BytecodeCompiler* compiler, Str name, uint8_t arg_count, int line,
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
  uint32_t index = chunk_add_constant(compiler->chunk, sym_value);
  emit_uint32(compiler, index, line);
  emit_byte(compiler, arg_count, line);
  return true;
}

bool emit_return(BytecodeCompiler* compiler, bool has_value, int line) {
  emit_op(compiler, OP_RETURN, line);
  emit_byte(compiler, has_value ? 1 : 0, line);
  return true;
}

bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr);
bool emit_lvalue_ref(BytecodeCompiler* compiler, const AstExpr* expr, bool is_mod);

bool emit_flattened_struct_args(BytecodeCompiler* compiler, const AstExpr* root_expr, Str type_name, uint32_t* total_args, int line) {
    const AstDecl* type_decl = find_type_decl(compiler->module, type_name);
    bool is_c_struct = (type_decl && type_decl->kind == AST_DECL_TYPE && has_property(type_decl->as.type_decl.properties, "c_struct"));
    
    if (!is_c_struct) {
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
            emit_uint32(compiler, (uint32_t)field_idx, line);
            
            char temp_name[64];
            snprintf(temp_name, sizeof(temp_name), "__flatten_tmp_%d_%d", line, field_idx);
            int slot = compiler_add_local(compiler, str_from_cstr(temp_name), field_type, false, false);
            if (slot < 0) return false;
            if (!compiler_ensure_local_capacity(compiler, compiler->local_count, line)) return false;
            
            emit_op(compiler, OP_SET_LOCAL, line);
            emit_uint32(compiler, (uint32_t)slot, line);
            emit_op(compiler, OP_POP, line); // Return from SET_LOCAL
            
            AstExpr tmp_ident = { .kind = AST_EXPR_IDENT, .line = (size_t)line, .as = { .ident = str_from_cstr(temp_name) } };
            if (!emit_flattened_struct_args(compiler, &tmp_ident, field_type, total_args, line)) return false;
        } else {
            // Primitive or regular field
            if (!compile_expr(compiler, root_expr)) return false;
            emit_op(compiler, OP_GET_FIELD, line);
            emit_uint32(compiler, (uint32_t)field_idx, line);
            (*total_args)++;
        }
        
        field = field->next;
        field_idx++;
    }
    return true;
}

bool emit_spawn_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line,
                               int column, uint8_t arg_count) {
  if (!entry) return false;
  if (entry->is_extern) {
    diag_error(compiler->file_path, line, column, "cannot spawn extern functions yet");
    return false;
  }
  emit_op(compiler, OP_SPAWN, line);
  uint32_t patch_offset = (uint32_t)compiler->chunk->code_count;
  emit_uint32(compiler, 0, line);
  emit_byte(compiler, arg_count, line);
  if (!function_entry_add_patch(entry, patch_offset)) {
    diag_error(compiler->file_path, line, column, "failed to record spawn call patch");
    compiler->had_error = true;
    return false;
  }
  return true;
}

bool compile_call(BytecodeCompiler* compiler, const AstExpr* expr, bool is_spawn) {
  if (expr->is_builtin_sizeof) {
      // In the VM, all values are currently 8 bytes (union part).
      emit_constant(compiler, value_int(8), (int)expr->line);
      return true;
  }

  if (expr->as.call.callee->kind != AST_EXPR_IDENT) {
  
    diag_error(compiler->file_path, (int)expr->line, (int)expr->column,
               "VM currently only supports direct function calls");
    compiler->had_error = true;
    return false;
  }

  uint32_t arg_count = 0;
  {
    const AstCallArg* arg = expr->as.call.args;
    while (arg) {
      arg_count++;
      arg = arg->next;
    }
  }

  Str name = expr->as.call.callee->as.ident;
  if (str_eq_cstr(name, "sizeof")) {
      emit_constant(compiler, value_int(8), (int)expr->line);
      return true;
  }
  bool is_log = str_eq_cstr(name, "log");
  bool is_log_s = str_eq_cstr(name, "logS");
  if (is_log || is_log_s) {
    if (is_spawn) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn log/logS");
        return false;
    }
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
    if (is_spawn) {
        diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn rae_str/rae_str_concat");
        return false;
    }
    const AstCallArg* arg = expr->as.call.args;
    while (arg) {
      if (!compile_expr(compiler, arg->value)) return false;
      arg = arg->next;
    }
    return emit_native_call(compiler, name, (uint8_t)arg_count, (int)expr->line, (int)expr->column);
  }

  /* Intrinsic Buffer Ops */
  if (str_eq_cstr(name, "__buf_alloc")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    emit_op(compiler, OP_BUF_ALLOC, (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_free")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    emit_op(compiler, OP_BUF_FREE, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_get")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    if (!arg->next || !compile_expr(compiler, arg->next->value)) return false;
    emit_op(compiler, OP_BUF_GET, (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_set")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    if (!arg->next || !compile_expr(compiler, arg->next->value)) return false;
    if (!arg->next->next || !compile_expr(compiler, arg->next->next->value)) return false;
    emit_op(compiler, OP_BUF_SET, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_len")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    emit_op(compiler, OP_BUF_LEN, (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_resize")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
    const AstCallArg* arg = expr->as.call.args;
    if (!arg || !compile_expr(compiler, arg->value)) return false;
    if (!arg->next || !compile_expr(compiler, arg->next->value)) return false;
    emit_op(compiler, OP_BUF_RESIZE, (int)expr->line);
    emit_constant(compiler, value_none(), (int)expr->line);
    return true;
  }
  if (str_eq_cstr(name, "__buf_copy")) {
    if (is_spawn) { diag_error(compiler->file_path, (int)expr->line, (int)expr->column, "cannot spawn buffer ops"); return false; }
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
    for (uint32_t i = 0; i < arg_count; ++i) {
      arg_types[i] = vm_infer_expr_type(compiler, arg->value);
      arg = arg->next;
    }
  }

  entry = function_table_find_overload(&compiler->compiler_ctx->functions, name, arg_types, arg_count);
  free(arg_types);

  if (entry) {
  }

  if (!entry) {
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
  uint32_t current_arg_idx = 0;
  while (arg) {
    bool handled_arg = false;
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
        // Check signature to see if we should pass by reference
        bool is_ref_param = false;
        if (current_arg_idx < entry->param_count) {
            Str p_type = entry->param_types[current_arg_idx];
            if (str_starts_with_cstr(p_type, "mod ") || str_starts_with_cstr(p_type, "view ")) {
                is_ref_param = true;
            }
        } else {
            // Fallback for non-extern calls without full signature (shouldn't happen with entry)
            Str arg_type = vm_infer_expr_type(compiler, arg->value);
            if (arg_type.len > 0 && !vm_is_primitive_type(arg_type)) {
                is_ref_param = true;
            }
        }

        if (is_ref_param) {
            int slot = compiler_find_local(compiler, arg->value->as.ident);
            if (slot >= 0) {
                // Determine if we need mod or view ref
                bool is_mod = false;
                if (current_arg_idx < entry->param_count) {
                    is_mod = str_starts_with_cstr(entry->param_types[current_arg_idx], "mod ");
                }
                emit_op(compiler, is_mod ? OP_MOD_LOCAL : OP_VIEW_LOCAL, (int)expr->line);
                emit_uint32(compiler, (uint32_t)slot, (int)expr->line);
                handled_arg = true;
            }
        }
    } else if (!entry->is_extern && !explicitly_referenced && arg->value->kind == AST_EXPR_MEMBER) {
        // Check signature to see if we should pass by reference
        bool is_ref_param = false;
        bool is_mod = false;
        if (current_arg_idx < entry->param_count) {
            Str p_type = entry->param_types[current_arg_idx];
            if (str_starts_with_cstr(p_type, "mod ") || str_starts_with_cstr(p_type, "view ")) {
                is_ref_param = true;
                is_mod = str_starts_with_cstr(p_type, "mod ");
            }
        } else {
            Str arg_type = vm_infer_expr_type(compiler, arg->value);
            if (arg_type.len > 0 && !vm_is_primitive_type(arg_type)) {
                is_ref_param = true;
            }
        }

        if (is_ref_param) {
            if (emit_lvalue_ref(compiler, arg->value, is_mod)) {
                handled_arg = true;
            }
        }
    }

    if (!handled_arg) {
        // STRUCT-TO-STRUCT FFI (VM Flattening)
        // If it's a native call, check if the argument type is a c_struct
        bool handled_flattening = false;
        if (entry->is_extern) {
            Str type_name = vm_infer_expr_type(compiler, arg->value);
            if (type_name.len == 0) {
                // Try fallback to expected parameter type
                type_name = entry->param_types[current_arg_idx];
            }
            
            Str base_type_name = get_base_type_name_str(type_name);
            const AstDecl* type_decl = find_type_decl(compiler->module, base_type_name);
            if (type_decl && type_decl->kind == AST_DECL_TYPE && has_property(type_decl->as.type_decl.properties, "c_struct")) {
                // Flatten! Push each field (recursively)
                uint32_t flattened_count = 0;
                if (!emit_flattened_struct_args(compiler, arg->value, base_type_name, &flattened_count, (int)expr->line)) return false;
                arg_count = (uint32_t)(arg_count + flattened_count - 1);
                handled_flattening = true;
            }
        }

        if (!handled_flattening) {
            if (!compile_expr(compiler, arg->value)) {
              return false;
            }
        }
    }
    arg = arg->next;
    current_arg_idx++;
  }
  
  if (is_spawn) {
      if (!emit_spawn_call(compiler, entry, (int)expr->line, (int)expr->column, (uint8_t)arg_count)) return false;
      // Spawned calls don't return a value to the caller stack (immediately)
      emit_constant(compiler, value_none(), (int)expr->line);
      return true;
  }

  return emit_function_call(compiler, entry, (int)expr->line, (int)expr->column,
                            (uint8_t)arg_count) && !compiler->had_error;
}


bool compile_block(BytecodeCompiler* compiler, const AstBlock* block) {
  compiler->scope_depth++;
  const AstStmt* stmt = block ? block->first : NULL;
  bool success = true;
  while (stmt) {
    if (!compile_stmt(compiler, stmt)) {
      success = false;
    }
    stmt = stmt->next;
  }
  
  if (!vm_emit_defers(compiler, compiler->scope_depth)) {
    success = false;
  }
  vm_pop_defers(compiler, compiler->scope_depth);
  
  compiler->scope_depth--;
  return success;
}

static bool compile_function(BytecodeCompiler* compiler, const AstDecl* decl) {
  if (!decl || decl->kind != AST_DECL_FUNC) {
    return false;
  }
  const AstFuncDecl* func = &decl->as.func_decl;
  
  uint32_t param_count = 0;
  for (const AstParam* p = func->params; p; p = p->next) param_count++;
  
  Str* param_types = NULL;
  if (param_count > 0) {
    param_types = malloc(param_count * sizeof(Str));
    const AstParam* p = func->params;
    for (uint32_t i = 0; i < param_count; ++i) {
      param_types[i] = get_base_type_name(p->type);
      p = p->next;
    }
  }

  FunctionEntry* entry = function_table_find_overload(&compiler->compiler_ctx->functions, func->name, param_types, param_count);
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

  // Emit jump over function body so execution flows to next top-level statement
  uint32_t jump_over = emit_jump(compiler, OP_JUMP, (int)decl->line);

  entry->offset = (uint32_t)compiler->chunk->code_count;
  
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
    
    // SEMANTICS: view-by-default
    bool is_ptr = false;
    bool is_mod = false;
    if (param->type) {
        is_mod = param->type->is_mod;
        if (param->type->is_id || param->type->is_key) {
            is_ptr = false;
        } else {
            bool is_val = param->type->is_val;
            bool is_explicit_view = param->type->is_view;
            bool is_primitive = vm_is_primitive_type(type_name);
            is_ptr = (is_mod || is_explicit_view || (!is_val && !is_primitive));
        }
    }

    if (compiler_add_local(compiler, param->name, type_name, is_ptr, is_mod) < 0) {
      compiler->current_function = NULL;
      return false;
    }
    param = param->next;
  }
  compiler->allocated_locals = compiler->local_count;

  bool success = true;
  if (!compile_block(compiler, func->body)) {
    success = false;
  }

  // Implicit return at end of function
  if (!vm_emit_defers(compiler, 0)) {
    success = false;
  }
  emit_return(compiler, false, (int)decl->line);
  compiler->current_function = NULL;
  
  patch_jump(compiler, jump_over);

  return success;
}

bool vm_compile_module(CompilerContext* ctx, const AstModule* module, Chunk* chunk, const char* file_path, VmRegistry* registry, bool is_patch) {
  if (!module || !chunk) {
      return false;
  }
  
  if (module->had_error) {
      return false;
  }

  chunk_init(chunk);
  BytecodeCompiler compiler = {
      .compiler_ctx = ctx,
      .chunk = chunk,
      .module = module,
      .file_path = file_path,
      .registry = registry,
      .is_patch = is_patch,
      .had_error = false,
      .current_function = NULL,
      .local_count = 0,
      .allocated_locals = 0,
      .expected_type = {0},
  };

  if (!collect_metadata(ctx, file_path, module, &ctx->functions, &ctx->types, &ctx->enums, registry)) {
    diag_error(file_path, 0, 0, "failed to prepare VM metadata");
    compiler.had_error = true;
  }

  Str main_name = str_from_cstr("main");
  FunctionEntry* main_entry = function_table_find(&ctx->functions, main_name);
  if (!main_entry) {
    diag_error(file_path, 0, 0, "no `func main` found for VM execution");
    compiler.had_error = true;
  }

  const AstDecl* decl = module->decls;
  while (decl) {
    if (decl->kind == AST_DECL_FUNC) {
      if (!compile_function(&compiler, decl)) {
        compiler.had_error = true;
      }
    } else if (decl->kind == AST_DECL_GLOBAL_LET) {
        // Handle global let as a statement in the "module entry" area
        // We reuse the existing logic for LET statements, but adapt it for DECL
        Str name = decl->as.let_decl.name;
        uint32_t global_idx = VM_GLOBAL_NOT_FOUND;
        if (compiler.registry) {
            Str type_name_str = get_base_type_name(decl->as.let_decl.type);
            global_idx = vm_registry_ensure_global(compiler.registry, name, type_name_str);
        }
        
        if (global_idx != VM_GLOBAL_NOT_FOUND) {
            emit_op(&compiler, OP_GET_GLOBAL_INIT_BIT, (int)decl->line);
            emit_uint32(&compiler, global_idx, (int)decl->line);
            emit_op(&compiler, OP_NOT, (int)decl->line);
            uint32_t skip_init_jump = emit_jump(&compiler, OP_JUMP_IF_FALSE, (int)decl->line);
            
            if (decl->as.let_decl.value) {
              if (!compile_expr(&compiler, decl->as.let_decl.value)) {
                  compiler.had_error = true;
                  break;
              }
            } else {
              if (!emit_default_value(&compiler, decl->as.let_decl.type, (int)decl->line)) {
                  compiler.had_error = true;
                  break;
              }
            }
            
            emit_op(&compiler, OP_SET_GLOBAL, (int)decl->line);
            emit_uint32(&compiler, global_idx, (int)decl->line);
            emit_op(&compiler, OP_SET_GLOBAL_INIT_BIT, (int)decl->line);
            emit_uint32(&compiler, global_idx, (int)decl->line);
            
            patch_jump(&compiler, skip_init_jump);
        }
    }
    decl = decl->next;
  }

  // Always emit a return for the module entry code area
  if (!compiler.had_error && !is_patch) {
      if (!emit_function_call(&compiler, main_entry, 0, 0, 0)) {
          compiler.had_error = true;
      } else {
          emit_op(&compiler, OP_POP, 0);
      }
  }
  emit_return(&compiler, false, 0);

  if (!compiler.had_error) {
    if (!patch_function_calls(&ctx->functions, chunk, file_path)) {
      compiler.had_error = true;
    }
  }

  if (compiler.had_error) {
    chunk_free(chunk);
  }
  
  bool success = !compiler.had_error;
  // free_function_table(&compiler.functions);
  // free_type_table(&compiler.types);
  // free_enum_table(&compiler.enums);
  // Method table free not yet implemented but should be added when used.

  return success;
}

bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line) {
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

  if (str_eq_cstr(type_name, "Int64") || str_eq_cstr(type_name, "Int")) {
    emit_constant(compiler, value_int(0), line);
  } else if (str_eq_cstr(type_name, "Float64") || str_eq_cstr(type_name, "Float")) {
    emit_constant(compiler, value_float(0.0), line);
  } else if (str_eq_cstr(type_name, "Bool")) {
    emit_constant(compiler, value_bool(false), line);
  } else if (str_eq_cstr(type_name, "String")) {
    emit_constant(compiler, value_string_copy("", 0), line);
  } else if (str_eq_cstr(type_name, "Char") || str_eq_cstr(type_name, "Char32")) {
    emit_constant(compiler, value_int(0), line);
  } else if (str_eq_cstr(type_name, "List") || str_eq_cstr(type_name, "Array")) {
    emit_op(compiler, OP_NATIVE_CALL, line);
    emit_uint32(compiler, chunk_add_constant(compiler->chunk, value_string_copy("rae_list_create", 15)), line);
    emit_constant(compiler, value_int(0), line);
    emit_byte(compiler, 1, line);
  } else if (enum_table_find(&compiler->compiler_ctx->enums, type_name)) {
    emit_constant(compiler, value_int(0), line);
  } else {
    TypeEntry* entry = type_table_find(&compiler->compiler_ctx->types, type_name);
    if (!entry) {
      char buf[128];
      snprintf(buf, sizeof(buf), "unknown type '%.*s' for default initialization", (int)type_name.len, type_name.data);
      diag_error(compiler->file_path, line, 0, buf);
      return false;
    }

    for (size_t i = 0; i < entry->field_count; i++) {
      if (entry->field_defaults[i]) {
        if (!compile_expr(compiler, entry->field_defaults[i])) {
          return false;
        }
      } else {
        if (!emit_default_value(compiler, entry->field_types[i], line)) {
          return false;
        }
      }
    }
    emit_op(compiler, OP_CONSTRUCT, line);
    emit_uint32(compiler, (uint32_t)entry->field_count, line);
    uint32_t type_idx = chunk_add_constant(compiler->chunk, value_string_copy(entry->name.data, entry->name.len));
    emit_uint32(compiler, type_idx, line);
  }

  return true;
}
