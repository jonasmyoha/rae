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
- Violations MUST produce a compiler diagnostic

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

## SAY: spoken summaries (IMPORTANT)

This project may be used with **voice-driven development**.  
Codex must emit **spoken summaries** using a strict format.

### How to use SAY:

- Use `SAY:` to emit **short, human-friendly summaries**
- Each `SAY:` line must be **one sentence**
- Maximum length: ~15 words
- Do **not** include code, filenames, or symbols unless essential
- Do **not** overuse — only speak meaningful milestones

### When to use SAY:

Use `SAY:` when:
- A task or subtask finishes
- Tests pass or fail
- A blocking error is encountered
- Human input or decision is required
- Codex is about to stop and wait

### When NOT to use SAY:

Do NOT use `SAY:` for:
- Routine logging
- Code listings
- Internal reasoning
- Step-by-step narration
- Debug spam

### Examples (GOOD):

```

SAY: Parser changes complete and all tests pass.
SAY: Build failed due to a missing enum case.
SAY: I need clarification on match expression semantics.
SAY: Task finished. Ready for the next item in the queue.

```

### Examples (BAD):

```

SAY: I am now editing compiler/src/parser.c and adding a new function...
SAY: Here is the diff:
SAY: fn parse_expression(...)

```

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
  2. Emit exactly one final `SAY:` summary
  3. Stop and wait

## Testing

To run the full test suite, use the following command from the `rae/compiler` directory. It is recommended to use a timeout to prevent hanging:

```bash
# Using perl if 'timeout' command is not available (common on macOS)
perl -e 'alarm shift; exec @ARGV' 60 make test
```

Example:
```

SAY: Lexer refactor complete and all tests pass.

```

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

- **THE QUEUE IS THIS ONE FILE ONLY: the top-level
  `rae-lang-dev/QUEUE.md`.** It is the single source of truth. Do NOT
  create, read, or write a `QUEUE.md` anywhere else — in particular
  never inside the `rae/` submodule. SUMU only shows the user this
  top-level file, so any queue elsewhere is invisible to them and
  will silently rot (this happened once: a stale `rae/QUEUE.md`
  shadowed the real one for months). If you find a `QUEUE.md` outside
  the top level, treat it as a bug: move any still-open tasks here and
  delete it. Watch your working directory — if a `cd` leaves you
  inside `rae/`, a bare `QUEUE.md` path resolves to the wrong place;
  always use the absolute top-level path for the queue.

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

