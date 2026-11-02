/* The derived binary encoding (docs/wasm-interfaces.md §2, §3):
 * `value.toBinary()`, `T.fromBinary(bytes:)`, `binarySchemaHash(T)` and
 * `binarySchemaText(T)`. */
#ifndef C_BINARY_H
#define C_BINARY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ast.h"

/* Whether `type` can cross the encoding. When it cannot, `message` says which
 * field (a path from `type`) has which type, and false is returned. */
bool binary_type_crossable(CompilerContext* ctx, const AstTypeRef* type, char* message, size_t cap);

/* The canonical description of `type` (`Point{x:Int,y:Int}`), allocated in
 * the AST arena, and its FNV-1a 64-bit hash. */
const char* binary_schema_text(CompilerContext* ctx, const AstTypeRef* type);
int64_t binary_schema_hash(const char* text);

/* Record a type whose encoding the program uses: the helpers of every struct
 * it contains are emitted. `use` 1 adds the toBinary wrapper, 2 the fromBinary
 * one (a struct type only), 0 neither (a module export's value). */
void binary_register_type(CompilerContext* ctx, const AstTypeRef* type, unsigned char use);

/* For a value of any crossable type (a module export's parameter or result):
 * rae_binGetValue_<name>(in, value) -> 0 when malformed or not used up,
 * rae_binPutValue_<name>(out, value) and rae_binFreeValue_<name>(value).
 * Emitted after binary_emit_helpers. */
void binary_emit_value_codec(CompilerContext* ctx, FILE* out, const char* name, const AstTypeRef* type);

/* The struct type of a `value.toBinary()` / `T.fromBinary(bytes:)` call,
 * substituted through the enclosing generic context and without view / mod;
 * NULL for any other expression. `is_from` tells the two apart. */
#ifdef C_BACKEND_INTERNAL_H
const AstTypeRef* c_binary_call_type(CFuncContext* ctx, const AstExpr* expr, bool* is_from);
#endif

/* `List(UInt8)` and `opt T`, the two result types, as type references. */
const AstTypeRef* binary_bytes_type(CompilerContext* ctx);
const AstTypeRef* binary_opt_type(CompilerContext* ctx, const AstTypeRef* type);

/* The C helpers for every registered type (after the struct definitions). */
void binary_emit_helpers(CompilerContext* ctx, FILE* out);

#endif
