# Merged recommendation

LEAD: claude
REASONING: Clo has the deepest codebase context from the previous implementation session — already read sema.c, c_backend.c, mangler.c, type.c, and ast.h. Already committed 27 test fixes (128→155 passing). Has the categorized root cause analysis (5 root causes across 49 failures) and a prioritized fix order ready to execute. Chattie provided good strategic guardrails but hasn't touched the code. Gem's response was incomplete.
FIRST_TASK: Add missing runtime header declarations (rae_ext_rae_buf_set, rae_ext_rae_buf_get, rae_ext_rae_buf_copy, rae_ext_sizeof) to rae_runtime.h — this is a pure header fix that unblocks ~12 tests with zero compiler logic changes, then move to fixing generic T substitution in the sema specialization engine.
