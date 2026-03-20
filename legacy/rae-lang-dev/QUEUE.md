<!-- SUMU_QUEUE_START -->
# SUMU queue

Use chat text that starts with "add to queue" to append tasks.

## C Backend Monomorphisation — Remaining Fixes

- [ ] Task 1: Emit collection literals (`let x: List(Int) = { 10, 20, 30 }`) — handle AST_EXPR_COLLECTION_LITERAL in c_backend emit_expr by emitting createList + add calls. Fixes 325_list_literal, 337_list_method.
- [ ] Task 2: Fix identity/newtype lowering (`let pid: User = 123`) — detect `is id`/`is key` types and emit underlying primitive type. Fixes 341_identity_behavior + smoke 10_memory_basics, identity_fix, store_mapping.
- [ ] Task 3: Fix toString() method resolution — `s.active.toString()` emits `rae_toString` undeclared. Emit as type-specific rae_ext_rae_str call. Fixes 350_auto_init (partial).
- [ ] Task 4: Fix output format mismatches for ref types — 336 outputs `7` instead of `view 7`, 351 outputs `80` instead of `mod 80`. Decide: update test expectations or add prefix in C log. Also finish 350_auto_init string eq chain.
- [ ] CHECKPOINT 1: Re-run all tests after tasks 1-4. Evaluate results. Update remaining tasks.
- [ ] Task 5: Fix opt T return type unboxing — `get(T)` returns RaeAny but callers expect rae_String. Add .as.s/.as.i unboxing at call sites when return type is opt T. Fixes 371_string_overhaul + ~10 smoke tests.
- [ ] Task 6: Fix createIntMap/createInt64Map specialization chain — inner specialization not discovered. Improve C backend discovery pass transitive following. Fixes 370_map_basic.
- [ ] Task 7: Fix rae_any macro for struct types — _Generic has no case for List/custom structs. Add struct handling or avoid boxing. Fixes multiple smoke tests.
- [ ] Task 8: Fix view restriction diagnostic for compiled target — test expects compiler errors but compiled target emits nothing. Wire sema view checks for compiled path. Fixes 357_view_restriction.
- [ ] CHECKPOINT 2: Re-run all tests after tasks 5-8. Major smoke test re-assessment.
- [ ] Task 9: Add raylib C function forward declarations — emit declarations for rae_ext_initWindow etc. when raylib is imported. Fixes ~8 raylib smoke tests.
- [ ] Task 10: Add crypto function declarations and C wrappers — rae_ext_rae_crypto_lock/unlock using monocypher. Fixes 19_file_ops, 27_file_locking, 28_crypto_demo.
- [ ] Task 11: Fix random(min,max) overload in imported modules — 2-arg random not found in import context. Extend overload search. Fixes 04_random, 92_pong_import, 94_tetris.
- [ ] Task 12: Implement defer statement emission — collect defers, emit before returns and at function end. Fixes 15_defer_cleanup.
- [ ] CHECKPOINT 3: Re-run all tests after tasks 9-12. Full re-assessment.
- [ ] Task 13: Fix external C function linking — compile user .c files alongside generated output. Fixes 20_external_c.
- [ ] Task 14: Fix JSON serialization (toJson/fromJson) — implement C-side or mark VM-only. Fixes 17_json_demo, 18_complex_json.
- [ ] Task 15: Fix method this parameter passing (value vs pointer) — add & for struct this args in method calls. Fixes 25_timer_demo + others.
- [ ] Task 16: Fix float modulo operator — emit fmod(a,b) instead of a%b for doubles. Fixes 93_raylib_3d.
- [ ] CHECKPOINT 4: Final evaluation. Plan next phase.

<!-- SUMU_QUEUE_END -->
