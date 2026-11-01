#ifndef C_MODULE_EXPORTS_H
#define C_MODULE_EXPORTS_H

#include <stdbool.h>
#include <stdio.h>

#include "ast.h"

/* The names of the functions a WASM module exports (space- or comma-
 * separated, from the .raepack's wasm target), or NULL for a program */
extern const char* g_wasm_module_exports;

/* Append the exports' wrappers (docs/wasm-modules.md); false after a
 * diagnostic */
bool emit_wasm_module_exports(CompilerContext* ctx, FILE* out);

#endif
