/* The may-wait analysis (docs/lightweight-spawn-design.md §4.1, step S2):
 * which functions can wait, and which of those a lightweight spawn would
 * have to compile as resumable; and, for code generation, which spawns run
 * as lightweight tasks on the scheduler (may_wait_spawn_on_pool). */
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

/* The same analysis kept for code generation (docs/lightweight-spawn-design
 * .md §8 step 3): build it once after discovery, ask it per spawn, free it. */
struct WaitGraph* may_wait_build(CompilerContext* ctx, const AstModule* merged);
void may_wait_free(struct WaitGraph* graph);

/* Whether a spawn of `decl` (non-generic) can run as a task on the
 * scheduler's worker pool: nothing it can reach waits except through task
 * joins (`get`, a Task dropped at scope end, a taskScope), and every extern it
 * can reach is the runtime's own non-blocking kind (lib/core, Math, Parallel,
 * Time and Channel, minus the sleeps). With `allow_sleep` (the spawned
 * function has a resumable twin, c_twin.c) its sleeps may be reached too,
 * since they suspend. A task that waits on a socket or calls other C stays a
 * thread until its wait can suspend (S4) or is marked `blocking` (S5). */
bool may_wait_spawn_on_pool(struct WaitGraph* graph, const AstDecl* decl, bool allow_sleep);

/* Whether a sleep (sleep, Time.waitUntil) can be reached from `decl` */
bool may_wait_reaches_sleep(struct WaitGraph* graph, const AstDecl* decl);
/* Whether `decl` may wait at all */
bool may_wait_decl_may_wait(struct WaitGraph* graph, const AstDecl* decl);
/* Whether `decl` may wait and is reachable from a spawn: a candidate for a
 * resumable twin (c_twin.c) */
bool may_wait_twin_candidate(struct WaitGraph* graph, const AstDecl* decl);

#endif
