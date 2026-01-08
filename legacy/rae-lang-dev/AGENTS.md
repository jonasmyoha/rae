# AGENTS.md — Codex working instructions (Rae compiler)

This repository contains the **Rae programming language compiler**.  
Codex is used as an implementation agent, while the human maintains architectural direction, language design, and final decisions.

These instructions define **how Codex should work**, communicate progress, and interact with the surrounding workflow.

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

When a task is complete:
1. Ensure code builds/tests
2. Emit exactly one final `SAY:` summary
3. Stop and wait

Example:
```

SAY: Lexer refactor complete and all tests pass.

```

---

End of instructions.
