#ifndef C_MODULE_EXPORTS_H
#define C_MODULE_EXPORTS_H

#include <stdbool.h>
#include <stdio.h>

#include "ast.h"

/* The names of the functions a WASM module exports (space- or comma-
 * separated, from the .raepack's wasm target), or NULL for a program */
extern const char* g_wasm_module_exports;

/* Register the struct, List, opt and enum values the exports take and
 * return, so their encoders are emitted (before binary_emit_helpers). */
void wasm_module_exports_register(CompilerContext* ctx);

/* Append the exports' wrappers and the `rae.interface` custom section
 * (docs/wasm-modules.md, docs/wasm-interfaces.md §3); false after a
 * diagnostic */
bool emit_wasm_module_exports(CompilerContext* ctx, FILE* out);

#endif
