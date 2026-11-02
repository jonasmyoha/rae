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

/* Record a struct type a toBinary / fromBinary call uses: its helpers are
 * emitted, with those of every struct it contains. */
void binary_register_type(CompilerContext* ctx, const AstTypeRef* type, bool is_from);

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
