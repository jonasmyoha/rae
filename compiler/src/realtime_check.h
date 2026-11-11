/* The `realtime` and `noAlloc` checks (docs/realtime-language-design.md §2,
 * §3), over the may-wait graph (may_wait.c, wait_graph.h). */
#ifndef RAE_REALTIME_CHECK_H
#define RAE_REALTIME_CHECK_H

#include <stdbool.h>
#include <stdio.h>

#include "ast.h"

struct WaitGraph;

/* Check every function marked `realtime` or `noAlloc` in the built graph;
 * reports each violation with diag_error and answers false when there was
 * one. Nothing marked: true at once. */
bool realtime_check(CompilerContext* ctx, const AstModule* merged, struct WaitGraph* graph);

/* `rae build --report-realtime`: every marked function, clean or what it
 * reaches, and the C it trusts */
bool realtime_report(CompilerContext* ctx, const AstModule* merged, const char* entry, FILE* out);

#endif
