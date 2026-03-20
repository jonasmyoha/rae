<!-- SUMU_QUEUE_START -->
# SUMU queue — C Backend Monomorphisation Stabilization

Baseline: 165 pass, 9 fail (unit tests) + ~29 smoke test failures.
Goal: Fix all remaining compiled-mode failures.

---

## Task 1: Emit collection literals (List init from `{ 10, 20, 30 }`)
**Tests:** 325_list_literal, 337_list_method
**Issue:** `rae_List_int64_t x = ;` — AST_EXPR_COLLECTION_LITERAL not emitted in C backend
**Fix:** In emit_expr, handle AST_EXPR_COLLECTION_LITERAL by:
  1. Emit `createList(T)(count)` call to allocate
  2. Emit `add(T)(list, value)` for each element
  3. Needs a temp variable since it's an expression
**Files:** c_backend.c (emit_expr)
**Impact:** 2 unit tests + smoke tests using list literals

## Task 2: Fix identity/newtype lowering (`let pid: User = 123`)
**Tests:** 341_identity_behavior, 10_memory_basics (smoke), identity_fix (smoke), store_mapping (smoke)
**Issue:** Types with `is id` or `is key` properties should lower to their underlying type (Int or String)
**Fix:** In emit_type_ref_as_c_type and let emission, detect identity types and emit the underlying primitive type instead of the struct
**Files:** c_backend.c (emit_type_ref_as_c_type, emit_stmt LET)
**Impact:** 1 unit test + 3 smoke tests

## Task 3: Fix `toString()` method resolution
**Tests:** 350_auto_init
**Issue:** `rae_toString` undeclared — the method call `s.active.toString()` isn't resolved
**Fix:** Add toString to method resolution or emit as rae_ext_rae_str for the appropriate type (Bool -> rae_ext_rae_str_bool)
**Files:** c_backend.c (emit_call_expr or method call handling)
**Impact:** 1 unit test

## Task 4: Fix `is` operator for Strings in remaining contexts + log(view ref)
**Tests:** 350_auto_init (partial), 336_return_view_ref_alias, 351_mod_ref_assignment
**Issue:** 350 still has `rae_toString` after task 3 fix; 336/351 emit `7` instead of `view 7`/`mod 80`
**Fix:** 336/351 are OUTPUT FORMAT mismatches — the VM prints "view"/"mod" prefix but C backend doesn't. Either update tests or add prefix in C log emission for ref types. 350 needs toString + string eq chaining.
**Files:** c_backend.c, possibly test expected.txt updates
**Impact:** 3 unit tests

---

## CHECKPOINT 1: Evaluate progress after tasks 1-4
Re-run all tests. Update queue based on results. Identify new failures or issues.
Expected: ~165 + 4-6 more unit tests passing.

---

## Task 5: Fix `opt T` return type — unbox RaeAny to concrete type at call sites
**Tests:** 371_string_overhaul, many smoke tests (04_random, 21_stdlib, 25_timer, 26_easing, 94_tetris)
**Issue:** `get(T)` returns `opt T` = `RaeAny`, but callers assign to `rae_String` without unboxing
**Fix:** In emit_call_expr, when the called function returns `opt T` and the usage context expects a concrete type, add `.as.s` / `.as.i` / `.as.f` unboxing after the call
**Files:** c_backend.c (emit_call_expr, c_return_type)
**Impact:** 1 unit test + ~10 smoke tests

## Task 6: Fix `createIntMap`/`createInt64Map` specialization chain
**Tests:** 370_map_basic
**Issue:** `rae_createInt64Map_int64_t_` undeclared — `createIntMap(V)` calls `createInt64Map(V)` internally but inner specialization isn't discovered
**Fix:** The C backend discovery pass needs to follow specialization chains through function bodies more deeply. May need to substitute types in receiver inference for method calls on specialized types.
**Files:** c_backend.c (discover_specializations_expr, infer_expr_type_ref)
**Impact:** 1 unit test + smoke tests using maps

## Task 7: Fix `rae_any` macro for struct types (List, custom structs)
**Tests:** 325_list_literal (partial), generic_test (smoke), multiple smoke tests
**Issue:** `rae_any((x))` where x is `rae_List_int64_t` — _Generic macro has no case for struct types, defaults to `rae_any_ptr(void*)` which doesn't accept structs
**Fix:** Either add struct-specific rae_any cases or avoid boxing structs (emit log differently for struct types)
**Files:** rae_runtime.h (rae_any macro), c_backend.c (avoid boxing structs)
**Impact:** Multiple smoke tests

## Task 8: Fix view restriction diagnostic for compiled target
**Tests:** 357_view_restriction
**Issue:** Test expects compiler error messages but compiled target produces empty output
**Fix:** The view restriction check should run during sema (before C emission) and emit diagnostics. Check if sema already has this check and wire it for compiled target.
**Files:** sema.c or main.c (diagnostic emission path)
**Impact:** 1 unit test

---

## CHECKPOINT 2: Evaluate progress after tasks 5-8
Re-run all tests. Major re-assessment of smoke test state.
Expected: Most unit tests passing, significant smoke test improvement.

---

## Task 9: Add raylib C function declarations for compiled target
**Tests:** 90_raylib_basic, 91_pong_implicit, 92_pong_import, 93_raylib_3d, 94_tetris, 95_easing_2d, 96_easing_3d, pong (smoke)
**Issue:** `rae_ext_initWindow`, `rae_ext_drawCircle`, etc. undeclared in generated C
**Fix:** When compiling with raylib, emit forward declarations for all raylib wrapper functions. The C backend should include a raylib bridge header or emit declarations based on raylib.rae imports.
**Files:** c_backend.c (c_backend_emit_module), possibly new raylib_bridge.h
**Impact:** ~8 smoke tests

## Task 10: Add crypto function declarations for compiled target
**Tests:** 19_file_ops, 27_file_locking, 28_crypto_demo (smoke)
**Issue:** `rae_ext_rae_crypto_lock/unlock/argon2i` undeclared
**Fix:** Add crypto function declarations to rae_runtime.h or a separate crypto bridge header. Implement C-side wrappers using monocypher.
**Files:** rae_runtime.h or new file, rae_runtime.c
**Impact:** 3 smoke tests

## Task 11: Fix `random(min, max)` overload for imported modules
**Tests:** 04_random, 92_pong_import, 94_tetris (smoke)
**Issue:** `rae_random_(0, sw)` calls 0-arg overload — the 2-arg `random(min, max)` isn't found when called from imported module context
**Fix:** Extend overload resolution to search imported module functions, not just all_decls
**Files:** c_backend.c (emit_call_expr overload resolution)
**Impact:** ~3 smoke tests

## Task 12: Fix defer statement emission (`return ;` in main with defer)
**Tests:** 15_defer_cleanup (smoke)
**Issue:** `return ;` in main — bare return needs `return 0;` AND defer blocks need to be emitted before returns
**Fix:** Implement defer block emission: collect defer statements, emit them before each return and at function end
**Files:** c_backend.c (emit_stmt for AST_STMT_DEFER, emit_function)
**Impact:** 1 smoke test

---

## CHECKPOINT 3: Evaluate progress after tasks 9-12
Full re-assessment. Expected: most smoke tests fixed.

---

## Task 13: Fix external C function linking (20_external_c smoke)
**Issue:** `rae_ext_external_c_func` undeclared — needs C source file compilation/linking
**Fix:** The C backend build needs to compile and link user-provided .c files alongside the generated output
**Files:** main.c (build pipeline), possibly Makefile changes
**Impact:** 1 smoke test

## Task 14: Fix JSON serialization (toJson/fromJson)
**Tests:** 17_json_demo, 18_complex_json (smoke)
**Issue:** `rae_toJson`/`rae_fromJson` undeclared — JSON is VM-only
**Fix:** Implement C-side JSON serialization or mark these tests as VM-only
**Files:** Potentially new json_bridge.c or test config changes
**Impact:** 2 smoke tests

## Task 15: Fix method `this` parameter passing (value vs pointer)
**Tests:** 25_timer_demo (smoke — `rae_Timer` value passed where `rae_Timer*` expected)
**Issue:** Method calls pass struct values but method signatures expect pointers
**Fix:** In emit_call_expr for method calls, add `&` when passing struct `this` arg to method expecting pointer
**Files:** c_backend.c (emit_call_expr, METHOD_CALL handling)
**Impact:** ~3 smoke tests

## Task 16: Fix float modulo operator (`%` on doubles)
**Tests:** 93_raylib_3d (smoke — `double % double` invalid in C)
**Issue:** Rae `%` operator emits C `%` but that doesn't work for floats
**Fix:** Emit `fmod(a, b)` when operands are float types
**Files:** c_backend.c (emit_expr BINARY)
**Impact:** 1 smoke test

---

## CHECKPOINT 4: Final evaluation
Re-run everything. Identify any remaining issues. Plan next phase.

<!-- SUMU_QUEUE_END -->
