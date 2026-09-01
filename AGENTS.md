# AGENTS.md — Codex working instructions (Rae compiler)

This repository contains the **Rae programming language compiler**.  
Codex is used as an implementation agent, while the human maintains architectural direction, language design, and final decisions.

These instructions define **how Codex should work**, communicate progress, and interact with the surrounding workflow.

---

## Global Language Rules (MUST BE ENFORCED)

### Mandates:
- **Timeouts**: ALWAYS run long-running commands (builds, tests, examples) with explicit timeouts (e.g. `timeout 60 ...` or `perl -e 'alarm shift; exec @ARGV' 60 ...`) to prevent infinite hangs.

### Naming conventions (language-wide, normative):
- Function names MUST be `camelCase` (e.g. `add`, `removeLast`, `ensureCapacity`)
- Type names MUST be `PascalCase` (e.g. `List`, `Map`, `HashMap`, `Ptr`)
- EVERYTHING that is NOT a type is `camelCase` — functions, parameters, locals,
  struct fields, enum cases, AND constants. A `const` is a value binding, not a
  special category: write `const serialize: Int = 1`, `const maxRetries: Int = 5`.
- `snake_case` and `SHOUTING_SNAKE_CASE` are BANNED, everywhere, no exceptions.
  They are C / other-language habits, not Rae. In particular constants are NOT
  `SCREAMING_SNAKE_CASE` (that is the single most common way this slips in) — a
  const named `SERIALIZE` or `MAX_RETRIES` is wrong; use `serialize` /
  `maxRetries`. Enum cases are `camelCase` too (`roundedRect`, not `ROUNDED_RECT`).
- Violations MUST produce a compiler diagnostic

### No single-letter names for parameters or meaningful locals:
- Write `state`, `settings`, `world` — not `s`, `c`, `w`. A single letter says
  nothing about what it holds, and the reader has to scroll back to the
  signature to find out.
- It is WORSE IN RAE THAN ELSEWHERE, because arguments are named at the call
  site: the parameter name is what a reader sees at every call. `syncUi(world:
  world, s: state)` puts a meaningless label in front of a meaningful value;
  `syncUi(world: world, state: state)` reads. A one-letter parameter makes the
  language's best readability feature carry no information.
- Loop counters (`i`, `j`) and genuinely conventional maths (`x`, `y`, `dt`) are
  fine — those names ARE the meaning.
- Not yet swept through existing code. Fix it in files you are already editing;
  do not open a mass rename.

### `pub` is BAD STYLE — do not write it:
- **Everything is cross-file visible by default.** `pub` on a function changes
  nothing: a plain `func f()` in one module is already callable from another.
  `pub` on a type changes nothing either — types are always cross-file visible,
  which is why the compiler rejects the bare `type X pub` spelling outright.
- So `pub` is pure noise. It reads as if it were load-bearing — as if some other
  declaration nearby were private — and no such distinction exists. A reader who
  believes it means something will draw wrong conclusions about the API surface.
- Write `func createThing(...) ret Thing`, NOT `func createThing(...) pub ret Thing`.
- The keyword is still accepted (in the `type Name: pub` position for types), so
  existing occurrences are legal and do not need a sweep. Do not add new ones,
  and drop them from code you are already editing.

---

## Project overview

Rae is a **language designed for both humans and AI agents**.

It is intended to work in **two complementary modes**:
- **Live (bytecode VM)**: for rapid iteration, tooling, analysis, hot-reload, and AI-driven workflows
- **Compiled (C backend)**: for performance, distribution, and production use

Use these names consistently. “Live/Compiled/Hybrid” are the official short labels for CLI flags, docs, UI, and marketing copy. When extra clarity is needed, append the descriptive form (“Live (bytecode VM)”, “Compiled (C backend)”, “Hybrid Dev/Hybrid Release”). Avoid introducing new labels like “native” or “interpreted” unless a sentence specifically contrasts implementation details.

The language prioritizes:
- Clear, readable syntax that is easy to reason about
- Semantics that are explicit, stable, and machine-interpretable
- Minimal syntactic noise and few special cases
- Predictable behavior suitable for automated reasoning and transformation

Rae should be:
- Easy for humans to read, review, and maintain
- Easy for AI agents to parse, generate, refactor, and analyze
- Deterministic and structured, avoiding “clever” ambiguity
- Friendly to tooling: linters, formatters, static analysis, and AI assistants

The compiler and interpreter are treated as **two views of the same language**, sharing:
- The same lexer, parser, and AST
- The same semantic rules
- The same error reporting philosophy

This repository focuses on building the **core language infrastructure** that enables this dual nature:
- Lexing and parsing
- AST representation
- Semantic analysis
- Compilation pipeline
- Interpreter / evaluation support
- Tooling interfaces for humans and AI

Language design choices should always be evaluated by asking:
> “Does this make the language easier to understand, analyze, and evolve for both humans and machines?”

Do not introduce features that increase expressive power at the cost of clarity, analyzability, or determinism.

---

## Workflow & task management

- There is a `QUEUE` file (or similarly named task list).
- Codex should generally:
  1. Pick the top item from `QUEUE`
  2. Implement it carefully
  3. Update tests if applicable
  4. Stop and wait for confirmation before starting unrelated work

- **File size limit**: Aim to keep all source files under 1,000 lines of code (LOC). If a file exceeds this limit, it should be refactored into smaller, domain-specific modules. This helps maintain clarity and reduces LLM context usage.
- **Git operations**:
  - **Commit frequently**: Always commit meaningful work before moving on to the next task in the `QUEUE`.
  - **Commit messages**: Use good, clear, and descriptive commit messages that explain *why* the change was made.
  - Suggest committing and/or pushing when there is meaningful work and a good state to push, but do not push unless explicitly told.
  - When asked to push, commit and push first, then continue with the next task without pushing at the end.
  - **Do not commit large temporary or log files**: Files exceeding 10,000 lines of text (e.g., debug logs, trace outputs) must not be committed to the repository. If such a file is generated, stop, re-evaluate its necessity, and consult with a human for approval before proceeding.

If the task reveals a **design ambiguity**, stop and ask instead of guessing.

If the task turns out to be larger than expected, split it and explain.

---

## Coding style & expectations

- Follow existing code style strictly
- Prefer clarity over cleverness
- Avoid unnecessary abstractions
- Avoid premature optimization
- Do not refactor unrelated code “while here”
- Keep commits logically scoped
- **Rule of thumb for packages**: If an app or example doesn't strictly need a `.raepack` file (or isn't specifically demonstrating `.raepack` features), do NOT include one. Default compiler behavior should be preferred whenever possible.

Compiler code should favor:
- Explicit data structures
- Predictable control flow
- Easy-to-debug logic

---

## UI rendering loops & performance

Hard-won lesson (full postmortem: `rae/docs/ui-render-loop-performance.md`):

- **A wait-based (event-driven) loop cannot drive smooth continuous
  animation; a busy render loop can.** A loop that sleeps in
  `glfwWaitEventsTimeout` and only renders on wake must predict when to
  wake — fine for discrete input, but it starves continuous motion
  (scroll/drag/transitions). Tell-tale symptom: animation is choppy until
  you wiggle the mouse, which floods wake-up events.
- **For smooth animation, render in a busy loop** (poll with timeout `0`,
  paint every iteration, no FPS cap). For an app that must also idle at
  ~0% CPU, use a **hybrid**: busy-render while interacting/animating,
  blocking event-wait when idle (see `examples/98_mobile_ui/main.rae`).
  Games already do this — classic `setTargetFPS(n)` + busy
  `loop not windowShouldClose()`.
- Each app **owns its render loop** (`lib/ui` provides systems + the
  `nextWaitTimeoutSec` policy, not the loop). If you add a new animation
  source, feed it into the loop's "is animating" flag or it silently
  starves to the watcher-poll rate.
- **Keep per-frame work O(n), never O(n²).** ECS component lookups are
  O(1) via a sparse set; never add nested per-entity scans to a per-frame
  system. Optimization flags (`-O2`) cannot fix an algorithmic (O(n²)) or
  scheduling (starved wake-loop) problem — profile before tuning `-O`.

---

## Renderer C-surface gate (#505 — WebGPU bindings)

The renderer is **Rae over the generated low-level WebGPU bindings**
(`lib/webgpu/*.rae`, `lib/gpu*.rae`, `lib/gbuffer*.rae`), NOT a growing set of
renderer-specific C helpers. When you need new renderer functionality (compute
pipelines, indirect draws, storage buffers, texture arrays, query sets,
timestamps, a new pass), reach it **through the bindings** — do NOT add a new
`rae_ext_gbuffer_*` / `rae_ext_gpu3d_*` C function.

- New C is allowed only for genuine platform ABI (a new `rae_gb_*` / `rae_sm_*`
  / `rae_g2d_*` handle accessor or frame-derived uniform upload) or unavoidable
  platform glue / WGSL shader source.
- If you legitimately add such a symbol, add it to
  `tools/webgpu-c-surface-allowlist.txt` in the same commit with a one-line
  justification. Adding a line is the reviewable event.
- The gate `make c-surface-gate` (or `sh tools/webgpu-c-surface-gate.sh`) fails
  if a renderer `rae_ext_(gbuffer|gpu3d)_*` symbol exists that is not on the
  allowlist. Full rationale + classification: `docs/webgpu-c-surface-audit.md`.

---
## Interaction rules

- Assume the human may not be at the keyboard
- Summaries must make sense **without seeing the screen**
- If unsure, ask instead of continuing silently
- If something seems wrong, stop and report it

---

## Scope boundaries

Codex MAY:
- Implement queued tasks
- Fix obvious bugs related to the task
- Add or update tests related to the task

Codex MUST NOT:
- Redesign the language
- Introduce new syntax or semantics without approval
- Perform large refactors unless explicitly asked
- Change unrelated parts of the compiler

---

## Completion behavior

- **Completion behavior**:
  When a task is complete:
  1. Ensure code builds/tests
  2. Stop and wait

## Testing

**Always run the suite through the official script `compiler/tools/watch-tests.sh`,
NOT a bare `make test`.** The script streams `make test` into the ONE shared log
file the devtools web UI tails (`/tmp/rae-test-live.log`, override `RAE_TEST_LOG`)
and wraps it in `@@RAE_RUN_START@@` / `@@RAE_RUN_END exit=N@@` sentinels, so the
"Test log" tab AND the Compiler tab's Test Runner both show the run live — with
exact start/end boundaries and the real exit code. This matters for subagents
too: a subagent that runs tests MUST use the script so the human can see the run.
A bare `make test > /tmp/some-other-file.log` is invisible to the UI (wrong file)
— that is the single most common reason a run "doesn't show up".

```bash
# From the repo root (the script cd's into rae/compiler itself). Use a timeout
# so a hung test can't block forever (perl form works when `timeout` is absent,
# common on macOS):
perl -e 'alarm shift; exec @ARGV' 600 bash compiler/tools/watch-tests.sh
```

**Only ONE test run at a time.** Concurrent `make test` / `watch-tests.sh`
processes corrupt each other's build cache and interleave the shared log, which
shows up as spurious failures. Before starting a run, KILL any earlier one:

```bash
pkill -9 -f 'watch-tests.sh'; pkill -9 -f 'make test'; pkill -9 -f 'run_tests.sh'; pkill -9 -f 'bin/rae run'
```

Then start the single run. If you need to re-run, kill first, then re-launch —
never launch a second run on top of a live one.

---

End of instructions.

<!-- SUMU_QUEUE_NOTE -->
## SUMU AI
SUMU AI is a voice-first desktop AI assistant app for running project tools and agent workflows.

## QUEUE.md
`QUEUE.md` is the shared task list for this project. The assistant
(you, running through SUMU) is the one that implements queue items
— so you are also expected to **add** new items when the user
identifies follow-up work that doesn't fit the current turn.

- **THE QUEUE IS THIS ONE FILE ONLY: the monorepo-root `QUEUE.md`.** It is
  the single source of truth. Do NOT create, read, or write a `QUEUE.md`
  anywhere else. SUMU only shows the root file, so another queue would be
  invisible and silently rot. If you find a second `QUEUE.md`, move any
  still-open tasks into the root queue and delete the duplicate.

- SUMU may create this file when queue features are used.
- SUMU updates queue item status markers — `[>sumu]` (or
  `[>claude]`) for in-progress and `[x]` for done. Leave those
  markers to SUMU; you don't need to update them yourself.
- **You may freely append new items.** Use the next unused numeric
  id, the `[ ]` unchecked marker, and keep each item to a single
  line so SUMU's regex-style edits don't fight you. Example:

    `- [ ] #196 Implement docs/ownership-model.md: …`

- Keep manual edits simple (single-line items, no multi-line
  formatting) to avoid merge conflicts with SUMU's status updates.
