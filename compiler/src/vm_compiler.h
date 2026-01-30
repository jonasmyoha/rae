#ifndef VM_COMPILER_H
#define VM_COMPILER_H

#include <stdbool.h>
#include <stdint.h> // For uint16_t

#include "ast.h"
#include "vm_chunk.h"
#include "str.h" // For Str

typedef struct {
  Str name;
  Str* param_types; // All parameter types for dispatch
  uint16_t offset;
  uint16_t param_count;
  uint16_t* patches;
  size_t patch_count;
  size_t patch_capacity;
  bool is_extern;
  bool returns_ref;
  Str return_type;
} FunctionEntry;

typedef struct {
  FunctionEntry* entries;
  size_t count;
  size_t capacity;
} FunctionTable;

typedef struct {
  Str name;
  Str* field_names;
  const struct AstTypeRef** field_types;
  size_t field_count;
} TypeEntry;

typedef struct {
  TypeEntry* entries;
  size_t count;
  size_t capacity;
} TypeTable;

typedef struct {
  Str type_name; // e.g., "List", "Array"
  Str method_name; // e.g., "add", "len"
  Str actual_function_name; // e.g., "rae_list_add", "rae_list_len"
} MethodEntry;

typedef struct {
  MethodEntry* entries;
  size_t count;
  size_t capacity;
} MethodTable;

typedef struct {
  Str name;
  Str* members;
  size_t member_count;
} EnumEntry;

typedef struct {
  EnumEntry* entries;
  size_t count;
  size_t capacity;
} EnumTable;

typedef struct {
  Chunk* chunk;
  const AstModule* module; // Add module pointer for metadata lookups
  const char* file_path;
  bool had_error;
  FunctionTable functions;
  TypeTable types;
  MethodTable methods; // New field for method table
  EnumTable enums;     // New field for enum table
  const AstFuncDecl* current_function;
  struct {
    Str name;
    uint16_t slot;
    Str type_name; // Store type name for locals
  } locals[256];
  uint16_t local_count;
  uint16_t allocated_locals;
  Str expected_type;
} BytecodeCompiler;

#define INVALID_OFFSET UINT16_MAX

bool vm_compile_module(const AstModule* module, Chunk* chunk, const char* file_path);

// Function prototypes from vm_compiler.c
void free_function_table(FunctionTable* table);
void free_type_table(TypeTable* table);
FunctionEntry* function_table_find(FunctionTable* table, Str name);
FunctionEntry* function_table_find_overload(FunctionTable* table, Str name, const Str* param_types, uint16_t param_count);
TypeEntry* type_table_find(TypeTable* table, Str name);
bool type_table_add(TypeTable* table, Str name, Str* field_names, const struct AstTypeRef** field_types, size_t field_count);
int type_entry_find_field(const TypeEntry* entry, Str name);
void free_enum_table(EnumTable* table);
EnumEntry* enum_table_find(EnumTable* table, Str name);
bool enum_table_add(EnumTable* table, Str name, Str* members, size_t member_count);
int enum_entry_find_member(const EnumEntry* entry, Str name);
bool collect_metadata(const char* file_path, const AstModule* module, FunctionTable* funcs, TypeTable* types, EnumTable* enums);
bool emit_function_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line,
                               int column, uint8_t arg_count);
bool emit_return(BytecodeCompiler* compiler, bool has_value, int line);
int compiler_add_local(BytecodeCompiler* compiler, Str name, Str type_name);
int compiler_find_local(BytecodeCompiler* compiler, Str name);
void compiler_reset_locals(BytecodeCompiler* compiler);
bool compiler_ensure_local_capacity(BytecodeCompiler* compiler, uint16_t required, int line);
// Add other static function prototypes here as needed by vm_compiler.c

#endif /* VM_COMPILER_H */
