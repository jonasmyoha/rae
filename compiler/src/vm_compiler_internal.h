#ifndef VM_COMPILER_INTERNAL_H
#define VM_COMPILER_INTERNAL_H

// Internal-only header shared by vm_compiler.c and the vm_emit_*.c files.
// Anything declared here was previously a static helper inside vm_compiler.c;
// callers outside the vm_compiler must continue to use vm_compiler.h.
//
// Helpers that share a name with the C-backend equivalents have a `vm_`
// prefix here to avoid linker collisions.

#include <stdbool.h>
#include <stdint.h>

#include "ast.h"
#include "str.h"
#include "vm.h"
#include "vm_chunk.h"
#include "vm_compiler.h"
#include "vm_value.h"

// Low-level chunk emission.
void emit_byte(BytecodeCompiler* compiler, uint8_t byte, int line);
void emit_op(BytecodeCompiler* compiler, OpCode op, int line);
void emit_uint32(BytecodeCompiler* compiler, uint32_t value, int line);
void emit_constant(BytecodeCompiler* compiler, Value value, int line);

// Forward jumps.
uint32_t emit_jump(BytecodeCompiler* compiler, OpCode op, int line);
void patch_jump(BytecodeCompiler* compiler, uint32_t offset);

// Defer-stack bookkeeping (vm_-prefixed because c_backend has its own variant).
bool vm_emit_defers(BytecodeCompiler* compiler, int min_depth);
void vm_pop_defers(BytecodeCompiler* compiler, int depth);

// Native call shim.
bool emit_native_call(BytecodeCompiler* compiler, Str name, uint8_t arg_count, int line, int column);

// Default-value emission for `let x: T` without initializer and similar.
bool emit_default_value(BytecodeCompiler* compiler, const AstTypeRef* type, int line);

// Type / name helpers (vm_-prefixed where they shadow c_backend equivalents).
bool str_matches(Str a, Str b);
bool is_stdlib_module(Str name);
bool is_module_import(BytecodeCompiler* compiler, Str name);
bool vm_is_primitive_type(Str type_name);
Str strip_generics(Str type_name);
Str get_base_type_name_str(Str type_name);
bool vm_types_match(Str entry_type_raw, Str call_type_raw);
Str vm_get_local_type_name(BytecodeCompiler* compiler, Str name);
Str get_generic_arg(Str type_name);
Str vm_infer_expr_type(BytecodeCompiler* compiler, const AstExpr* expr);

// Compile entry points used cross-file.
bool compile_expr(BytecodeCompiler* compiler, const AstExpr* expr);
bool compile_call(BytecodeCompiler* compiler, const AstExpr* expr, bool is_spawn);
bool compile_stmt(BytecodeCompiler* compiler, const AstStmt* stmt);
bool compile_block(BytecodeCompiler* compiler, const AstBlock* block);
bool emit_lvalue_ref(BytecodeCompiler* compiler, const AstExpr* expr, bool is_mod);
bool emit_flattened_struct_args(BytecodeCompiler* compiler, const AstExpr* root_expr, Str type_name, uint32_t* total_args, int line);
bool emit_spawn_call(BytecodeCompiler* compiler, FunctionEntry* entry, int line, int column, uint8_t arg_count);

#endif /* VM_COMPILER_INTERNAL_H */
