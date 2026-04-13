- 2026-03-19: Revalidated previous monomorphisation-lead decision against current repo state.
- Recent git history still centers on Phase 4-6 TAST/monomorphisation work; no evidence that the implementation surface changed enough to invalidate earlier agent selection.
- Current narrow gate still fails in the same shape:
  - `./tools/run_tests.sh 337`: live passes, compiled fails.
  - `TARGET=compiled ./tools/run_tests.sh 370`: fails.
  - Failure pattern remains compiled-only: `rae_log_RaeAny_`/`rae_any` emission noise plus unspecialized or malformed generic C emission (`rae_List_rae_T...`, missing concrete container/buffer types).
- Recommendation remains: Clo should lead the implementation pass, but keep one semantic guardrail: no `RaeAny` shortcut for generic `T`; sema-owned specialization must remain the intended end state.
- New broad-state read from larger failure dump: the project is improving, but the remaining mess is still concentrated in compiled-mode convergence.
- Current high-level failure buckets:
  - Backend/runtime declaration gaps: missing `rae_ext_*`, raylib bridge types/functions, legacy `rae___buf_*`.
  - Monomorphisation/specialization gaps: unspecialized `rae_List_rae_T...` calls, wrong generic returns (`RaeAny` where concrete type expected), broken list/map specializations.
  - C emitter shape bugs: `expected expression`, bad pointer/value member access, bad match-expression lowering, wrong ref wrapper assignment.
  - Type-lowering gaps: view/mod wrappers, identity/newtype lowering, string equality/toString handling.
- Best near-term plan: do staged compiled recovery, not a giant refactor.
  1. Fix runtime declaration / extern emission layer.
  2. Fix generic buffer/list/map emission and specialization substitution.
  3. Fix ref/view/value lowering.
  4. Fix remaining semantic sugar lowering (`match` expr, string compare, identity).
- 2026-03-20 checkpoint: state is much better; only 6 failures remain (172/174 unit, 41/45 examples per Clo memory).
- The remaining failures now look mostly like one core issue: generic template bodies are being shared across specializations, so inner `decl_link`/call resolution sticks to the first specialization.
- Remaining special cases:
  - 370 / 21 / 94 / `list_native_any`: cross-specialization method resolution and wrong concrete type binding.
  - 371: bad lowering for `const void*` / `fromCStr`.
  - 28: undeclared `sys` suggests missed symbol binding or wrong clone/reanalysis path.
- Best next move is no longer wide cleanup; it is targeted sema specialization repair:
  - clone specialized bodies freshly,
  - clear or avoid reusing stale `decl_link`/resolved-type state,
  - rerun call resolution per specialization before C emission,
  - then fix the 1-2 remaining emitter bugs.
