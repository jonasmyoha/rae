# VM Research and Fixing Log

## Current Status (2026-01-09)
- Reverted problematic VM changes that were causing corruption.
- Increased stack size to 1024.
- Fixed stack leak in `AST_STMT_DEF`, `AST_EXPR_MATCH`, and `AST_STMT_MATCH` by adding `OP_POP` after `OP_SET_LOCAL`.
- Made `OP_ALLOC_LOCAL` idempotent and use absolute local counts instead of deltas.
- Verified that all 73 tests pass, including the regression test `321_stack_leak`.

## Identified Issues (Resolved)
1. **Stack Leak in Loops**: FIXED. `def` statements inside loops now correctly pop their initializer value.
2. **Cumulative Stack Growth**: FIXED. `OP_ALLOC_LOCAL` no longer increments `slot_count` if the required depth has already been reached.

## Next Steps
- Consider more robust memory management (GC or ownership-based `value_free`) in small, verifiable steps.
- Monitor for any other stack or memory related issues in larger examples.