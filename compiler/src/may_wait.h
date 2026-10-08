/* The may-wait analysis (docs/lightweight-spawn-design.md §4.1, step S2):
 * which functions can wait, and which of those a lightweight spawn would
 * have to compile as resumable. A report only; it changes no codegen. */
#ifndef RAE_MAY_WAIT_H
#define RAE_MAY_WAIT_H

#include <stdbool.h>
#include <stdio.h>

#include "ast.h"

/* Analyse the whole program (the merged module after sema and the backend's
 * discovery of generic specialisations) and print the report to `out`.
 * `entry` names the program in the first line. The counts are the program's;
 * `with_lib` adds lib/'s and lists lib/'s twins with their lines. */
bool may_wait_report(CompilerContext* ctx, const AstModule* merged, const char* entry, bool with_lib, FILE* out);

#endif
