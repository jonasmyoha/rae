#ifndef VM_COMPILER_H
#define VM_COMPILER_H

#include <stdbool.h>
#include <stdint.h> // For uint16_t

#include "ast.h"
#include "vm_chunk.h"
#include "str.h" // For Str

typedef struct {
  Str name;
  uint16_t offset;
  uint16_t param_count;
  uint16_t* patches;
  size_t patch_count;
  size_t patch_capacity;
  bool is_extern;
  bool returns_ref;
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
  Chunk* chunk;
  const char* file_path;
  bool had_error;
  FunctionTable functions;
  TypeTable types;
  MethodTable methods; // New field for method table
  const AstFuncDecl* current_function;
  struct {
    Str name;
    uint16_t slot;
    Str type_name; // Store type name for locals
  } locals[256];
  uint16_t local_count;
  uint16_t allocated_locals;
} BytecodeCompiler;

#define INVALID_OFFSET UINT16_MAX

bool vm_compile_module(const AstModule* module, Chunk* chunk, const char* file_path);

// Function prototypes from vm_compiler.c
void free_function_table(FunctionTable* table);
void free_type_table(TypeTable* table);
FunctionEntry* function_table_find(FunctionTable* table, Str name);
TypeEntry* type_table_find(TypeTable* table, Str name);
bool type_table_add(TypeTable* table, Str name, Str* field_names, const struct AstTypeRef** field_types, size_t field_count);
int type_entry_find_field(const TypeEntry* entry, Str name);
bool collect_metadata(const char* file_path, const AstModule* module, FunctionTable* funcs, TypeTable* types /* GEMINI: MethodTable* methods parameter removed to fix build */);
bool emit_function_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line,
                               int column, uint8_t arg_count);
bool emit_return(BytecodeCompiler* compiler, bool has_value, int line);
int compiler_add_local(BytecodeCompiler* compiler, Str name, Str type_name);
int compiler_find_local(BytecodeCompiler* compiler, Str name);
void compiler_reset_locals(BytecodeCompiler* compiler);
bool compiler_ensure_local_capacity(BytecodeCompiler* compiler, uint16_t required, int line);
// Add other static function prototypes here as needed by vm_compiler.c

#endif /* VM_COMPILER_H */
