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

### File and folder names follow the same rule — no `snake_case` on disk either:
- A `.rae` file is a MODULE, and its name is spelled in code at every `import`/
  `open` and every qualified call (`GpuTiming.createGpuTiming()`). `snake_case`
  there is the same C habit as a `snake_case` identifier, so it is banned too.
- **Module FILES are `PascalCase`; package FOLDERS are `camelCase`.** A module is
  a namespace, spelled like a type, so its file is PascalCase — `GpuTiming.rae`,
  `CameraRig.rae`, `Vec3.rae`, `Math.rae`, and EQUALLY `WorldBiome.rae`,
  `Gpu2dText.rae`, `NoiseWgsl.rae`. There is NO "bag of functions stays
  camelCase" exception any more: every `.rae` module file is PascalCase whether
  or not it has a single owning type. A folder is a PACKAGE, not a module, so it
  is camelCase — `ui/`, `ecs/`, `renderSystem/`, `hierarchySystem/`, `webgpu/`,
  `compress/`, `legacyRaylib/`. A system module therefore lives at
  `ui/renderSystem/RenderSystem.rae`: camelCase folder, PascalCase file.
- The import separator is `/` (never `.`): `import ui/renderSystem/RenderSystem`,
  `open ecs/hierarchySystem/HierarchySystem`. The program entry file is the
  `Main` module — `Main.rae`.
- **There is no lowercase-file exception any more: EVERY `.rae` file is
  PascalCase.** `core` used to be the one lowercase module; it is now the
  package `lib/core/` (`Core.rae`, `List.rae`), a camelCase folder with
  PascalCase files like every other package. It is still the prelude —
  auto-loaded and auto-opened, so `log(x)` and `List` are bare-callable with no
  import — but the folder name being `core` is just the normal camelCase-package
  rule, not a special case (#818).
- **Rule of thumb — core vs collections:** put a type in `core/` only if the
  *compiler itself* understands it (like `List`, which collection loops and the
  ECS are built on). A data structure written in ordinary Rae over `List`/
  `Buffer` — the hash maps, and future `Set`/`Queue`/`RingBuffer` — goes in the
  `collections/` package, which is NOT in the prelude: you `open collections/
  StringMap` to use it. core = what the language needs; collections = a library
  you ask for (#819, see `docs/collections.md`).
- Exceptions that keep their existing spelling:
  - Number-prefixed example/test directories (`106_*`, `414_*`, `550_*`) are a
    separate bucket scheme, not packages, and keep their names.
  - Non-module data on disk is not a module and is not renamed: shader assets
    (`*.wgsl`), `.raescene` files, images and other assets are referenced only by
    string paths. And the generated `lib/webgpu/` bindings mirror the WebGPU C
    API verbatim, so their `WGPU*` symbol names keep the C spelling — the same
    C-boundary exception `extern` gets.
- When you rename a module, update every `import`/`open` path and every
  `oldName.` qualifier that referenced it (they live in code, never in strings —
  a `"lib/foo_bar.wgsl"` asset path is a string and stays). A package-FOLDER
  rename only changes the folder component of import paths — the file component
  stays PascalCase (`ui/RenderSystem/RenderSystem` -> `ui/renderSystem/RenderSystem`).

### No single-letter names for parameters or meaningful locals:
- Write `state`, `settings`, `world` — not `s`, `c`, `w`. A single letter says
  nothing about what it holds, and the reader has to scroll back to the
  signature to find out.
- It is WORSE IN RAE THAN ELSEWHERE, because arguments are named at the call
  site: the parameter name is what a reader sees at every call. `syncUi(world:
  world, s: state)` puts a meaningless label in front of a meaningful value;
  `syncUi(world: world, state: state)` reads. A one-letter parameter makes the
  language's best readability feature carry no information.
- Loop counters (`i`, `j`) and genuinely conventional maths coordinates (`x`,
  `y`, `z`, `w`) are fine — those names ARE the meaning. (`dt` is NOT — spell it
  `deltaTime`; see the no-abbreviations rule below.)
- Not yet swept through existing code. Fix it in files you are already editing;
  do not open a mass rename.

### No abbreviations — a binding is named for what it HOLDS (usually its type):
- `cam` -> `cameraSystem`, `io` -> `inputSystem`, `ctx` -> `context`,
  `cfg` -> `config`, `tmp` -> `temporary`. An abbreviation makes the reader
  decode it and, because arguments are named at the call site, stamps the
  decoded-in-your-head short form onto every call — the exact spot the language
  wants to READ. Write the whole word.
- **A local/parameter that holds ONE instance of a type is named the camelCase
  of that type**: `renderSystem: RenderSystem`, `world: GameWorld`,
  `settings: Settings`. This is the default; reach for a different name only when
  it says something the type does not (a ROLE: `previous`/`current`, `source`/
  `destination`, `hero`/`crowd`).
- **Resolve a name conflict by making the name MORE specific, never by
  abbreviating.** Two cameras are `playerCamera` and `debugCamera`, not `cam1`/
  `cam2` or `cam`/`c`. Add words; do not remove them. `io` is doubly wrong — it
  is an abbreviation AND misleading (it reads as filesystem/stream I/O, not an
  input-system state).
- **The ONE exception: an established initialism that IS the thing's real,
  universally-recognised name** — where spelling it out would read as wrong or
  pedantic to any practitioner. `ui`/`Ui` (user interface — `UiSystem`,
  `uiWorld`) is the case that prompted this rule and is explicitly fine. So are
  `id` (identifier — `EntityId`, `nodeId`), the hardware/format/protocol
  initialisms (`gpu`, `cpu`, `api`, `json`, `html`, `css`, `url`, `uri`, `http`,
  `png`, `wgsl`, `sdf`), the colour initialisms tied to the already-allowed
  `r`/`g`/`b`/`a` (`rgb`, `rgba`, `hsl`), and the dimensional tags `2d`/`3d`
  (`World3d`, `Gpu2d`). Plus the maths coordinates already blessed above
  (`x`/`y`/`z`/`w`, `r`/`g`/`b`/`a`) and loop counters `i`/`j`.
  - **Case an initialism as an ordinary word** (the PascalCase/camelCase rule
    still applies): `Ui`, `Id`, `Gpu`, `Json`, `Html`, `Url` — NEVER `UI`, `ID`,
    `GPU`, `JSON`, `HTML`. So `EntityId` and `Gpu2d`, not `EntityID` / `GPU2D`.
  - This is a **CLOSED set of real names, not a licence for short names.** It
    does NOT reopen ad-hoc contractions: `cam`, `ctx`, `cfg`, `tmp`, `btn`,
    `msg`, `idx`, `elem` are still banned — they abbreviate a WORD, they are not
    the thing's name. Two specific bans worth calling out: `io` (spell the intent
    — `input`/`output`; `io` reads as stream I/O AND was misused for an
    input-system state) and `dt` (spell it `deltaTime`; it abbreviates the two
    words "delta time", it is not an initialism-name).
  - **The allowed set and its EXACT spelling** (casing follows the
    PascalCase-type / camelCase-everything-else rule — the initialism is one
    word, only its first letter capitalises in a type):

    | concept | camelCase (value) | PascalCase (in a type) |
    |---|---|---|
    | user interface | `ui`, `uiWorld` | `Ui`, `UiSystem`, `UiWorld` |
    | identifier | `id`, `nodeId` | `Id`, `EntityId` |
    | 2D / 3D | `world2d`, `gpu3d` | `World2d`, `Gpu3d` |
    | GPU / CPU | `gpu`, `cpu` | `Gpu`, `Cpu` |
    | API | `api` | `Api` |
    | JSON | `json` | `Json` |
    | HTML / CSS | `html`, `css` | `Html`, `Css` |
    | URL / URI | `url`, `uri` | `Url`, `Uri` |
    | HTTP | `http` | `Http` |
    | PNG | `png` | `Png` |
    | WGSL | `wgsl` | `Wgsl` |
    | SDF | `sdf` | `Sdf` |
    | RGB / RGBA / HSL | `rgb`, `rgba`, `hsl` | `Rgb`, `Rgba`, `Hsl` |

    Maths coordinates `x`/`y`/`z`/`w`, colour channels `r`/`g`/`b`/`a`, and loop
    counters `i`/`j` stay single-letter (those letters ARE the meaning).
- Same phasing as the single-letter rule: fix it in files you are already
  editing; do not open a mass rename of unrelated code.

### NO type inference — every binding is written with its type (by design):
- Rae has **no type inference and it is strictly FORBIDDEN to add it.** Types are
  mandatory. Every `let`/`var`/`const` value binding states its type on the
  left-hand side: `let count: Int = 5`, `let track: Track = loadTrack(...)`,
  `const maxRetries: Int = 5`.
- A binding with no type is a **compile error** (enforced in the parser): the
  spellings `let count = 5` and `let q = makeR()` (type inferred from the RHS)
  are rejected, not accepted as a convenience. Never write them, and never
  "restore" inference to make code shorter.
- This is the same rule as named arguments and parameter modes: the type of a
  local is information the reader (human or tool) needs on the page, not
  reconstructed from the right-hand side or an editor hover. An inferred binding
  also hides a wrong/changed RHS type until it surfaces far away; a written type
  is checked at the binding.
- A **struct is constructed with the type on the LEFT and bare braces on the
  right** — `let p: Point = { x: 1, y: 2 }`, NOT `let p = Point { x: 1, y: 2 }`
  (no LHS type) and NOT `let p: Point = Point { ... }` (type written twice). In a
  `ret`, where there is no LHS, the literal carries the type: `ret Point { ... }`.
- What is written but not "inferred": `if let v: T = opt` (you still write `T`),
  `=>` aliases whose `view T`/`mod T` you write, and `loop var i: Int = 0`. The
  ban is specifically on inferring the type of a binding from its initializer.

### NO globals — mutable module-level state is forbidden (see docs/globals-and-app-ownership.md):
- "No globals" means precisely: a module-level **`var`** (mutable global state)
  and a module-level **`let` that owns heap** (a `String`, `List`, `Map`, or any
  struct carrying heap — a global with no owner) are **not allowed**. There is no
  hidden, program-wide, mutable channel between functions.
- **`const` is allowed, and so is a plain constant `let`.** A module-level
  `const` (`const maxRetries: Int = 5`) is a compile-time value, not state. A
  module-level `let` bound to a POD literal or a string literal
  (`let title: String = "Rae"`) is likewise a constant with a static buffer, not
  a global — it should usually just be spelled `const`. What is banned is
  *mutable* state and *heap ownership* at module level, not named constants.
- The replacement is **ownership**, matching the ECS architecture: a singleton
  becomes a **resource on the World or App** and is threaded explicitly through
  the `mod`/`view` parameter that already says who may touch it — e.g. a former
  `var activeTheme` becomes `app.theme`, passed to the systems that read it.
- Enforcement is phased: it is a **hard compiler error in `lib/`** (the stdlib is
  globals-clean) and a **warning in app/example code** whose singletons are still
  being migrated onto their owners. Do not add new module-level `var`/heap-`let`
  anywhere; when you touch a file that still has one, prefer moving it onto its
  owner.

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

### NO `@`-sigil keywords / attributes (strong maintainer preference):
- Do NOT propose or add language constructs spelled with a leading `@` (or the
  `#[...]` variant) — `@derive`, `@component`, `@world`, `@inline`, `@test`,
  `#[derive]`, etc. The maintainer strongly dislikes them, and the reasoning is
  worth carrying, not just the rule:
- **There is no real "logic vs metadata" line that justifies the sigil.** The
  tempting defence — "`@` is for metadata/annotations, real keywords are for
  code that executes" — does not survive contact with actual languages. The SAME
  concept is a sigil in one language and a bare keyword in another: Java
  `@Override` vs Kotlin/Swift `override`. Whether something "is metadata" is not
  a property of the concept; it is a spelling each language picks. `const`,
  `public`, `override`, `derive` are all just declaration MODIFIERS — nothing
  stops any of them being `@const`/`@public`, which is proof the category is
  fake.
- **The split is arbitrary even WITHIN one language.** Java sigils `@Override`
  but not `public`/`static`/`final`; Rust sigils `#[derive]`/`#[inline]` but not
  `pub`/`const`/`mut`. Same syntactic role (a modifier on a declaration), split
  across two spellings by history and taste, not principle. So the programmer
  carries TWO vocabularies and must memorise which modifier lives in which set,
  for no rule and no gain — an "additional layer" bolted on.
- **The ONE case where a sigil is actually earned is an OPEN, user-extensible
  set** — Python decorators apply any callable; Rust attributes/macros are
  library-definable. There the sigil is a NAMESPACE ESCAPE HATCH: it lets an
  open set of declaration-transformers exist without reserving new keywords and
  without stealing identifiers (you can still name a value `derive`). A CLOSED,
  compiler-defined set (Java `@Override`, Swift `@escaping`) has no such excuse —
  it could just be the keyword — and is pure convention.
- **Rae does not want that open extensibility.** An open macro/decorator system
  is exactly the "clever, hard-to-analyse" mechanism Rae's goals (few special
  cases, determinism, machine-analyzability) refuse. So Rae never reaches the one
  case where `@` is justified, and every case it WOULD reach is the arbitrary
  one. Hence: one bare-keyword vocabulary, full stop. If a modifier is worth
  having, it is a REAL keyword in the same namespace as `func`/`type`/`if`/`loop`,
  chosen to read as part of the language — never an annotation stapled on top.
- This applies to design DISCUSSION too: when suggesting a language feature
  (e.g. compile-time field reflection / derive for ECS world helpers), spell it
  as a real keyword, a plain function, or a compile-time construct — never an
  `@attribute` / `#[...]`.

---

## Project overview

Rae is a **minimalistic language designed for both humans and AI agents**.

**Rae is built around the Entity Component System (ECS) as its general
architecture — not just for games, but for programs of any kind.** ECS is the
default way to structure state and behavior: data lives in components on
entities, logic lives in systems that run over them, and shared singletons are
resources owned by a World or App. This is a deliberate bet that one small,
explicit, data-oriented model — the same for a UI, a renderer, a simulation, or
a CLI tool — is easier for a person to reason about and for an agent to generate
and refactor than a grab-bag of ad-hoc patterns. Language features (compile-time
field reflection, the no-globals rule, ownership threaded through `mod`
parameters) exist to make that architecture clean rather than to bolt frameworks
on top.

**The Compiled (C backend) target is the ONLY supported, maintained, tested
target. Treat it as "the compiler."**

> ## The Live / VM (bytecode) target has been REMOVED (#957).
>
> The Live target (a.k.a. the VM, the bytecode interpreter, "Hybrid") was
> deleted — its ~10.6K lines of `vm_*.c`, the `--target live`/`hybrid` CLI, and
> the bytecode fixtures are gone. **Compiled (C backend) is the one and only
> target.** Do NOT reintroduce a bytecode VM, a `--target live`/`hybrid` flag,
> or a `vm_*.c` file. Live's former roles (scripting, hot-reload, OTA logic) are
> reassigned to **native-hosted WASM modules** (`docs/raepack-v2-and-packages.md`).
> Older docs may still describe Rae as "two modes"; that is history — it is one
> mode. The deprecated path can be recovered from git history if ever needed.

Historically Rae was pitched as **two complementary modes** — Live (bytecode VM)
for iteration/tooling/hot-reload and Compiled (C backend) for production. Only
the Compiled half was ever real, and the Live half is now removed (#957). The
short labels "Live"/"Hybrid" survive only in old docs; read every such claim as
history, and reach for **WASM modules** for the scripting/OTA role Live once
nominally filled.

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
  - **Co-author every commit**: End every commit message with a
    `Co-authored-by: <exact LLM model name> <GitHub-associated email>` trailer.
    Name the model as precisely as the agent can, rather than using only the
    product or agent name. Codex uses the repository's GitHub co-author identity
    `codex@openai.com`; for example, GPT-5.6 Sol writes
    `Co-authored-by: GPT-5.6 Sol <codex@openai.com>`. Other agents use their
    established GitHub-associated identity and their exact current model name.
  - Suggest committing and/or pushing when there is meaningful work and a good state to push, but do not push unless explicitly told.
  - When asked to push, commit and push first, then continue with the next task without pushing at the end.
  - **Pull with rebase and autostash before pushing.** Use `git pull --rebase --autostash`
    to replay local commits on the upstream branch and preserve uncommitted edits.
    Resolve rebase or stash-restoration conflicts before pushing; do not discard local
    edits. Autostash preserves edits locally; it does not commit or publish them.
    On each clone, configure the same defaults for plain `git pull`:

    ```bash
    git config --local pull.rebase true
    git config --local rebase.autoStash true
    ```

    These settings live in `.git/config` and are not shared by a push. Keep this
    instruction so agents and new clones apply the same workflow.
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

## Source formatting: `rae format` owns layout (#911)

Rae has ONE canonical layout and the compiler is its authority — there is no
style configuration. The policy (full design: `docs/rae-format-design.md`):

- 2-space indent, 100 columns, LF, one final newline, no trailing whitespace,
  braces on the declaration line; the 1,000-line file cap is unchanged.
- Function parameters and call arguments: 1–3 items may share the line if the
  whole header/call fits in 100 columns; from FOUR items, or whenever it does
  not fit, every item goes on its own line (two spaces deeper, no commas, `)`
  back at the declaration's indentation). Type fields, object and collection
  literals: the same from FIVE items. Never hand-align, never keep personal
  wrapping.
- The compiler formats first: `rae run` / `rae build` / `rae watch` rewrite the
  project's changed `.rae` files canonically BEFORE compiling (atomic, in
  place); `--check-format` / `RAE_FORMAT=check` refuse to write and fail instead
  (what the test suite and the example gate use). A file whose canonical form
  would exceed 1,000 lines is an error to split, never a rewrite.
- Clarifications settled by #916 (the formatter implements exactly these): a
  `type` declaration is ALWAYS one field per line (the compact `{ a, b }` form
  is for literals); a lone argument/field/value that is itself a vertical list
  hugs its delimiters (`ret Point {` … `}`, `foo(a: bar(` … `))`); `ret a, b`
  never goes vertical (the grammar ends the statement at the newline);
  parentheses the source wrote are kept, missing ones are added only where
  precedence needs them; `[a, b]` keeps its brackets; at most ONE blank line
  is kept where the source had one, never at the start of a block; a comment
  keeps its place — before the statement/item it precedes, trailing after a
  one-line statement, on its own line below a statement that went vertical;
  `# ` prose is wrapped at 100 columns at a space, comment lines with no break
  point (URLs, diagrams) and `#`-without-space lines are copied verbatim.
- The `rae format` CLI is complete (#917): `rae format <files|dirs>` formats
  IN PLACE by default (atomic temp+rename, permissions preserved, unchanged
  files untouched), `--check` lists unformatted files and exits non-zero,
  `--stdout` prints, `--write`/`-w` are aliases for the in-place default,
  `--stdin` reads stdin, `--json` emits per-file results, `--rules --json`
  prints the width/indent/threshold rules; a canonical form over 1,000 lines
  is REFUSED (never written). Directory traversal takes `.rae`/`.raepack`
  sorted, follows no symlinks, and skips `.git` / `build` / `.rae` dirs. The
  in-memory `rae_format_source` (src/rae_format.{c,h}) is the ONE formatter the
  CLI, tests and the #919 build preflight all call.
- The tree IS canonical since #918 and STAYS so by construction since #919:
  the build's format preflight (`rae run` / `build` / `watch`) canonicalises
  every `.rae` in the program's source closure before lexing it, `rae watch`
  ignores the mtime events of its own writes, and the test suite, the tree
  check (`compiler/tools/format-check-tree.sh`, run first on every full suite
  run) and the example gate all run in CHECK mode (`RAE_FORMAT=check`) — an
  unformatted file fails them instead of being rewritten under the runner.
  The only deliberately non-canonical inputs are the format fixtures (200–208,
  568, 786, 810–813, 829, 830), the expected-parse-error fixtures, the lexer
  fixtures whose token columns are the test (006, 015, 019) and the CRLF /
  missing-final-newline fixtures (345, 346, 348 — the runner sets
  `RAE_FORMAT=off` for them); the generated `lib/webgpu` bindings are a
  file-wide `# raefmt: off` region (bindgen emits it). `rae init` writes this
  section into new projects; the VS Code extension (`tools/editor/vscode`)
  formats on save through `rae format --stdin --stdout`. The preflight costs
  ~0.5 s on the largest example (106_mobile_ui: 8.0 s → 8.6 s emit; ~130
  modules re-rendered and compared in memory) — no content cache is needed.

---

## Renderer C-surface gate (#505 — WebGPU bindings)

The renderer is **Rae over the generated low-level WebGPU bindings**
(`lib/webgpu/*.rae`, `lib/Gpu*.rae`, `lib/Gbuffer*.rae`), NOT a growing set of
renderer-specific C helpers. When you need new renderer functionality (compute
pipelines, indirect draws, storage buffers, texture arrays, query sets,
timestamps, a new pass), reach it **through the bindings** — do NOT add a new
`rae_ext_Gbuffer_*` / `rae_ext_Gpu3d_*` C function.

- New C is allowed only for genuine platform ABI (a new `rae_gb_*` / `rae_sm_*`
  / `rae_g2d_*` handle accessor or frame-derived uniform upload) or unavoidable
  platform glue / WGSL shader source.
- If you legitimately add such a symbol, add it to
  `tools/webgpu-c-surface-allowlist.txt` in the same commit with a one-line
  justification. Adding a line is the reviewable event.
- The gate `make c-surface-gate` (or `sh tools/webgpu-c-surface-gate.sh`) fails
  if a renderer `rae_ext_(Gbuffer|Gpu3d)_*` symbol exists that is not on the
  allowlist. Full rationale + classification: `docs/webgpu-c-surface-audit.md`.

---

## Compiler versioning (#930 — docs/versioning-and-toolchain.md §1)

The compiler has one version, `compiler/VERSION` — a single semver line
(`0.1.0`), baked into the binary at build time together with `git describe`
(`tools/gen-version-header.sh` -> `build/version_gen.h`, included by
`main.c`). `rae --version` (or `-v`) prints it; `rae --version --json` prints
`{ version, tag, commit, date, dirty }` for tooling.

- **Pre-1.0 semver, MINOR is the breaking axis.** While the version is `0.x`:
  a **MINOR** bump means a breaking change to the language, the compiler CLI,
  or `lib/` (a function signature moved, a keyword changed, a module
  renamed); a **PATCH** bump is compatible (fixes, additions, new
  diagnostics). `1.0.0` is deferred until the language stops moving.
- **Bump discipline: in the SAME commit as the change, not after.** A commit
  that changes a `lib/` signature or CLI behaviour bumps `compiler/VERSION`'s
  MINOR component in that same commit and says so in the commit message. A
  compatible fix/addition bumps PATCH the same way when it's the kind of
  change users should be able to tell apart from the last release (routine
  queue work does not need a bump on every commit — use judgement: bump when
  the change is release-notes-worthy, not for every diagnostic tweak).
- **A release is a tag, nothing else.** `git tag -a v0.1.0` on the commit
  that bumps `compiler/VERSION` to `0.1.0` — no release branches, no build
  artifacts; the toolchain is a source checkout, so the tag IS the release.
  Between tags, `rae --version` reports the NEXT version (the current
  `compiler/VERSION` content) suffixed `-dev.N` for N commits since the last
  tag, and `+dirty` on an uncommitted tree.
- **Projects declare which Rae they need (#931).** A `.raepack` may carry
  `rae: { version: "0.1" }` (bare = caret, `>=0.1.0 <0.2.0`; or an explicit
  `">=A <B"` range). `rae run`/`build`/`watch` check it against this compiler
  BEFORE the format preflight and fail with the repair if it does not match; a
  between-tags `-dev` build counts as its base version (so co-developing Rae
  and a project on `main` does not trip it). `RAE_TOOLCHAIN_CHECK=off` skips the
  non-strict check; `--check-toolchain` is the strict CI form (requires a tagged
  release, cannot be silenced by the env var). No pack, or a pack without a
  `rae` block, means no check. A verified build writes the project's `rae.lock`
  `toolchain { version commit }` block. `rae init` scaffolds the requirement.
- **`rae toolchain` (#932) manages the sibling checkout.** `status` prints this
  compiler's version + checkout and, inside a project, the pack requirement with
  a satisfied/UNSATISFIED verdict (exit 1 on unsatisfied — a scriptable
  `make doctor`); `list` shows the release tags, marking the current one;
  `use <req>` resolves the newest tag matching `<req>` (or `use main`) in the
  checkout the compiler was built from — or `$RAE_ROOT` — `git checkout`s it and
  reruns `make -C compiler build`, refusing when the checkout has uncommitted
  tracked changes. One checkout moved between tags; side-by-side toolchains are
  out of scope.
- **Dependencies (#933).** A `.raepack` may declare
  `dependencies: { dep ui: { path: "../lib-ui" } dep codec: { git: "<url>", rev: "v1.2.0" } }`.
  A dep named `x` owns the import prefix `x/` — `import x/Foo` resolves to
  `<depdir>/Foo.rae`, ahead of `lib/` and the stdlib — and a dep's own modules
  address each other the same way (`import x/Bar`). Path deps resolve in place;
  git deps are cloned into `<project>/.rae/deps/<name>` (gitignored) and pinned
  to the commit the project's `rae.lock` records — the lock wins over the pack's
  `rev`, so delete the lock record to move a dep. The lock's `dep` lines carry
  `source`, `rev`, `commit` and a `git-tree` content hash; its first record is
  `dep lib` — the stdlib resolved through the toolchain, so projects need no
  `lib` symlink. Dependency sources are vendored: the format preflight neither
  rewrites nor check-fails them, and the implicit project scan skips every
  dot-directory so `.rae/deps` is reached only through the resolver.
- **Package CLI (#934).** `rae add <name> --path <dir> | --git <url> [--rev <rev>]`
  edits the project's `.raepack` (creating the `dependencies` block if absent,
  refusing a duplicate name); `rae fetch` resolves dependencies into `.rae/deps`
  honoring the committed lock (offline-safe, never rewrites an existing lock);
  `rae update [<name>]` re-resolves within the pack's requirement — re-fetching
  the remote and ignoring the lock pin — then rewrites `rae.lock`; `rae tree`
  prints the resolved graph (the stdlib `lib` edge + each dep with its rev and
  short commit). All four operate on the pack in the current directory.

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

**Run it in the FOREGROUND.** The run takes ~6 minutes (489 cases, ~350 s
parallel on 10 cores) and the tool call simply returns when it is done. That is
the whole recipe; everything below is only for the case where you truly cannot
block.

**If you background it, watch the RIGHT marker.** This has cost hours, three
times: a background run was waited on with

```bash
# WRONG — this can never match, and burns the entire bound every time.
until grep -q 'RAE_RUN_END' "$f"; do sleep 5; done
```

`RAE_RUN_END` is written by `watch-tests.sh` into **`/tmp/rae-test-live.log`**
(or `$RAE_TEST_LOG`) — never to stdout, so it is never in the background task's
output file. Piping through `| tail -3` makes it worse: nothing at all reaches
that file until the pipeline ends. The marker that IS in the task file is the
runner's own `[exited with code N]`. Wait like this:

```bash
# RIGHT — ends the moment the run ends, gives up if the run dies, bounded.
for t in $(seq 1 110); do
  grep -q '\[exited with code' "$f" && break   # the task wrapper's end marker
  grep -q 'RAE_RUN_END' /tmp/rae-test-live.log && break   # the script's own
  kill -0 "$pid" 2>/dev/null || break           # producer gone: stop waiting
  sleep 6
done
```

Three rules behind it: bound every wait, check the producer is still alive, and
grep a marker that is actually written to the file you are grepping. A wait that
cannot end freezes the whole queue, not just your task.

**Killing a previous run: scope the pattern.** `pkill -9 -f 'make test'` matches
`make test` in EVERY repo on the machine, not just this one — it has killed
unrelated work. Match this repo's path, and give the process a chance to exit
cleanly before SIGKILL (a SIGKILLed `run_tests.sh` can leave a half-written
build cache, which is the corruption the one-run-at-a-time rule exists to
prevent):

```bash
pkill -f "$PWD/compiler/tools/(watch-tests|run_tests)" ; sleep 2
pkill -9 -f "$PWD/compiler/tools/(watch-tests|run_tests)" 2>/dev/null || true
```

**The unit cases run in PARALLEL by default** (`tools/run_tests_parallel.sh`,
#824/#846 — byte-identical verdicts to the serial runner at ~2.7x on 10 cores).
You do not need any flag. The old sequential runner is a debugging-only
fallback: `RAE_TEST_SEQUENTIAL=1 … watch-tests.sh` forces it. See
`docs/parallel-tests.md`.

**The VISUAL example smoke tests are NOT part of the unit suite, but you MAY
run them when a change warrants a visual check.** `run_examples.sh` builds and
renders every example in a real SDL/GPU window and takes screenshots — minutes
of wall time, and a window that briefly takes focus. `make test` /
`watch-tests.sh` run the non-visual unit suite only and print "Example smoke
tests (visual) not run"; they stay the fast default gate. When a renderer/UI
change genuinely needs pixels verified, it is fine to run the visual gate
(`make test-examples`, or `RAE_EXAMPLE_FILTER="106_mobile_ui" make test-examples`
to scope it, or a single `bin/rae run examples/...`) — use a timeout, and
narrow to the affected example(s) rather than rendering all of them. Prefer the
headless `rae build --emit-c` compile-check for pure type/signature changes
(no window, catches the same build errors); reach for the windowed gate when
the change could actually alter what is drawn. If you would rather the user run
it, ending with a note asking them to is still fine — but it is no longer
required, and "NO VISUAL TESTS" in an older task brief is not a hard bar.

**Banned-terms gate (#1016).** A handful of external project/product names
must never appear in this repository — not in code, docs, `QUEUE.md`, or
commit messages (three history rewrites so far). The gate
`compiler/tools/banned-terms-gate.sh` greps every tracked text file and the
messages of `origin/main..HEAD` against a term list that lives OUTSIDE the
repo (`$RAE_BANNED_TERMS_FILE`, default `~/.config/rae/banned-terms.txt`, one
case-insensitive term per line — never commit that list). It runs as the
second pre-suite case of every full run, right after `format-check-tree`; on
a machine without the list it prints `SKIP` and passes. Standalone:
`make banned-terms-gate`. Install the opt-in git hooks once per clone with
`make install-hooks` (`pre-commit` checks the staged changes, `commit-msg`
the message) so a slip is refused before it becomes a commit. Refer to such
a project as "game proto1" (or similar) instead.

**Stress cases (docs/stress-tests.md).** `stress/NN_name/` holds small
foot-gun programs on two shelves: `handles` (Rae does the right thing) and
`footgun` (Rae still gets it wrong — the case asserts the CURRENT bad
behaviour verbatim). `stress/run.sh` (`make stress`, `RAE_STRESS_FILTER` to
scope) runs as one pre-suite case of every full run right after the
banned-terms gate; a `footgun` that stops reproducing FAILS as `FIXED?` on
purpose — flip it to `handles` in the same commit as the compiler fix, with
`fixedBy` filled in. Never "fix" a FIXED? by loosening its expect.

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
- SUMU writes the RUNNING marker (`[>sumu]` / `[>claude]`) when it
  dispatches a task; the agent that ran it writes the OUTCOME marker on
  the same line as its last act (`[x]` / `[failed]` / `[question]` /
  `[postponed]`, in the same commit as the work) — see the per-task brief.
- **You may freely append new items — WITHOUT an id.** A task's `#id`
  is an 8-digit number SUMU mints when it next loads the file; never
  guess a `#<next id>` (a guessed number collides with one guessed
  elsewhere, and a collision deletes a task). Use the `[ ]` marker and
  keep each item to a single line so SUMU's regex-style edits don't
  fight you. Line order is queue order (top = next). Example:

    `- [ ] Implement docs/ownership-model.md: …`

- Keep manual edits simple (single-line items, no multi-line
  formatting) to avoid merge conflicts with SUMU's status updates.
