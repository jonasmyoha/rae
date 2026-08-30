# Rae Format: Canonical Source Formatting

**Status:** Proposed design, no implementation in this document  
**Date:** 2026-08-30

## Summary

Rae already has a built-in formatter:

```sh
compiler/bin/rae format file.rae
compiler/bin/rae format --write file.rae
compiler/bin/rae format --output formatted.rae file.rae
```

It parses Rae source and prints a canonical representation. The test runner
already verifies formatter output, idempotence, and AST equivalence. The
formatter also supports `# raefmt: off` / `# raefmt: on` verbatim regions.

The missing part is not a formatter executable. The missing parts are:

1. a complete and explicitly documented formatting policy;
2. project-wide formatting and check modes;
3. integration with normal development commands and editors;
4. reliable comment and line-wrapping behavior suitable for default use;
5. concise machine-readable instructions that AI agents can discover.

The recommendation is to make `rae format` the only authority on source
layout. Rae should have no formatting configuration file and no selectable
styles. Short forms remain compact. Long declarations, calls, literals, and
other lists become vertical with one item per line and no commas.

Compilation should check canonical formatting by default, but should not
silently rewrite files. `rae watch` and editor format-on-save may rewrite files
because those are explicitly interactive editing workflows.

## Goals

- Every formatted Rae project has the same layout regardless of author or AI.
- Formatting decisions do not depend on how the input happened to be wrapped.
- The format is intentionally line-oriented and easy to review.
- Long function signatures are vertically scannable.
- The formatter is safe to run repeatedly and never changes program meaning.
- Normal builds detect unformatted source before code generation.
- AI agents can discover the command and rules from the repository itself.
- The policy works with Rae's hard 1,000-line source-file limit.

## Non-goals

- User-configurable widths, indentation, comma styles, or brace styles.
- Preserving hand alignment or personal wrapping choices.
- Formatting invalid programs by guessing what the author intended.
- Automatically splitting a source file into modules.
- Teaching pretrained language models about Rae without repository context.
- Replacing semantic diagnostics or a future language server.

## Current implementation

### Existing strengths

The active implementation is in `compiler/src/pretty.c` and the CLI wiring is
in `compiler/src/main.c`.

It already provides:

- `rae format file.rae`, which writes formatted source to stdout;
- `rae format --write file.rae`, which rewrites one file;
- `rae format --output path file.rae`;
- relaxed lexing so CRLF and a missing final newline can be repaired;
- two-space indentation;
- LF output and a final newline;
- canonical spaces around syntax;
- single-line comma-separated and multiline comma-free lists;
- automatic wrapping of parameters, arguments, returns, object fields, and
  collection elements;
- comment emission and formatter-disabled regions;
- formatter tests in `compiler/tests/cases/200_*` through `208_*` and
  `568_format_global_bindings`;
- test-runner checks for idempotence and AST equivalence.

The parser already enforces an important Rae rule:

- a single-line list uses commas;
- a multiline list uses newlines and no commas;
- trailing commas are prohibited.

The formatter therefore chooses between two legal layouts rather than
inventing punctuation during arbitrary line breaks.

### Current weaknesses

The current formatter is not ready to be mandatory for every build:

- It formats only one file per invocation.
- It is not called by `run`, `build`, `watch`, or the repository test gate.
- The default command prints to stdout instead of behaving like a project
  formatter.
- Parameter wrapping estimates every type as roughly 15 columns rather than
  measuring the actual rendered declaration.
- Calls and declarations use different wrapping heuristics.
- Different constructs use different item-count thresholds.
- Comment wrapping compares against 100 columns but can emit toward 120.
- Binary expressions and some nested constructs have no complete wrapping
  policy.
- The VS Code extension provides syntax highlighting but no formatting
  provider.
- Comment attachment is based mostly on source lines and needs stronger
  regression coverage before all builds depend on it.
- `pretty.c` is already over the repository's preferred 1,000-line C-file
  size and should be split before substantial formatter work.

### The motivating example

This source is technically parseable but not acceptable canonical layout:

```rae
func addCastleTower(app: mod App, x: copy Float, y: copy Float, groundZ: copy Float,
                    dia: copy Float, body: copy Float, roofH: copy Float, stone: view Vec3) {
  # ...
}
```

The existing formatter already changes it to the desired shape because the
function has more than four parameters:

```rae
func addCastleTower(
  app: mod App
  x: copy Float
  y: copy Float
  groundZ: copy Float
  dia: copy Float
  body: copy Float
  roofH: copy Float
  stone: view Vec3
) {
  # ...
}
```

This should become the documented canonical form. Every parameter is on its
own line, indentation is two spaces, the closing parenthesis is on its own
line, and multiline parameters have no commas.

## Canonical formatting policy

### Fixed policy

Rae format has one style:

- indentation: 2 spaces;
- target line width: 100 columns;
- line endings: LF;
- exactly one final newline;
- no trailing whitespace;
- opening braces remain on the declaration/control-flow line;
- one blank line between top-level declarations;
- no configurable overrides for width or indentation.

The 100-column target is a layout decision, not permission to damage data.
Unbreakable strings, raw strings, URLs, and generated symbol names may exceed
it. The formatter should never alter literal contents merely to meet width.

### Function declarations

Keep a function declaration on one line only when all of these hold:

1. it has at most three parameters;
2. the complete header, including modifiers and return type, fits in 100
   columns;
3. no parameter or return item is itself multiline.

Otherwise use the vertical form:

```rae
func createRenderer(
  device: view Device
  window: mod Window
  settings: view RendererSettings
  diagnostics: mod Diagnostics
) ret Renderer {
  # ...
}
```

The `)` is placed at declaration indentation. Function modifiers and the
return clause follow it on the same line when they fit:

```rae
func readAsset(
  path: view String
  allocator: mod Allocator
) extern ret opt Asset
```

If a named return list is itself too long, it uses the same one-item-per-line,
comma-free policy. This needs an explicit grammar/formatter fixture before it
is enabled as a default rule.

Three parameters is a deliberate readability threshold, not a parser rule.
Compact mathematical helpers remain compact:

```rae
func clamp(value: copy Float, low: copy Float, high: copy Float) ret Float {
  # ...
}
```

Four or more parameters become vertical even if their names are short. This
keeps renderer, ECS, and platform APIs from becoming dense horizontal records.

### Function calls

Calls use the same policy: at most three arguments and a complete expression
that fits in 100 columns may remain on one line. Otherwise every argument gets
its own line:

```rae
addCastleTower(
  app: app
  x: towerX
  y: towerY
  groundZ: terrainHeight
  dia: towerDiameter
  body: bodyHeight
  roofH: roofHeight
  stone: stoneColor
)
```

Rae's named arguments make this layout especially readable. The formatter
must not align colons into columns; alignment creates noisy diffs when a name
changes.

### Types, objects, and collections

The same general list rule applies:

- short and fitting: one line with commas;
- long, nested, or more than the construct's compact threshold: one item per
  line without commas;
- closing delimiter on its own line for multiline forms.

Examples:

```rae
type CameraSettings {
  verticalFov: Float
  nearPlane: Float
  farPlane: Float
  movementSpeed: Float
}

let material: Material3d = {
  baseColor: color
  metallic: 0.1
  roughness: 0.65
  emissive: black
}
```

Thresholds should be shared rather than independently hard-coded for every
AST node. A first implementation should use three items for parameter and
argument lists, and four items for simple data literals, subject to the same
100-column fit check.

### Expressions

Expression formatting should be conservative in the first release:

- keep ordinary binary expressions on one line when they fit;
- wrap at an existing syntactic list boundary before splitting operators;
- do not add a complex general expression-breaking algorithm initially;
- never change parentheses or precedence to obtain a shorter line.

Operator-aware wrapping can be designed later with dedicated AST-equivalence
tests. It should not block canonical declaration and call formatting.

### Comments

- Preserve comments and their relative attachment to declarations/statements.
- Wrap ordinary line-comment prose to 100 columns.
- Preserve code-like comments, URLs, and explicit visual diagrams when safe
  wrapping is unclear.
- Keep `# raefmt: off` / `# raefmt: on` as the narrow escape hatch for foreign
  snippets and intentionally aligned tables.
- Formatter-disabled regions remain subject to lexical safety rules and the
  1,000-line file cap.

`raefmt: off` must be rare. It is not a project style configuration mechanism.

## What "runs by default" should mean

Silently rewriting files during every compile is not recommended. A build may
run in a read-only checkout, another process may be editing the same file, and
CI should never modify the tree it is validating. Build output must also not
depend on whether stdout is a terminal.

Instead, Rae should use three explicit behaviors.

### 1. Formatting is checked by default

`rae build` and `rae run` should format each source module in memory and compare
it with the bytes on disk before semantic analysis/code generation. If any file
is not canonical, compilation stops with a concise diagnostic:

```text
error: 3 Rae files are not canonically formatted
  lib/castle.rae
  lib/terrain.rae
  src/main.rae
run: rae format .
```

No source file is changed by a build. The check should cover the transitive Rae
source closure, including project modules actually imported by the entry point.

### 2. Interactive editing writes by default

`rae watch` should format a changed `.rae` source file atomically before
rebuilding. It must ignore the mtime event caused by its own write so one edit
does not trigger two rebuilds.

The Rae VS Code extension should register a document formatter and enable
format-on-save in the generated workspace settings created by `rae init`.
Other editors can invoke `rae format --stdin --stdout` or the future language
server formatting request.

### 3. CI has an explicit project check

The repository gate should run:

```sh
compiler/bin/rae format --check .
```

This should be a fast check before compiler and example tests. It prints only
files that differ and exits nonzero. `--json` should be available for devtools
and AI orchestration.

This model makes formatting unavoidable without making non-interactive builds
mutate source.

## Proposed CLI

The project formatter should accept files and directories:

```sh
rae format .                         # rewrite all Rae/RaePack files below .
rae format src lib                   # rewrite selected trees
rae format file.rae                  # rewrite one file
rae format --check .                 # compare only; nonzero if changed
rae format --stdout file.rae         # print one formatted file
rae format --output out.rae file.rae # retain the current explicit output mode
rae format --stdin --stdout          # editor/LSP integration
rae format --check --json .          # machine-readable changed/error records
```

To become Dart-like, plain `rae format <path>` should eventually write in
place. The current stdout behavior should move to `--stdout`. Existing
`--write` / `-w` can remain as accepted aliases during a deprecation period.

Directory traversal rules:

- include `.rae` and `.raepack`;
- sort paths lexicographically for deterministic output;
- ignore `.git`, build output, dependency caches, and hidden tool worktrees;
- follow no symlinks by default;
- do not rewrite unchanged files or update their mtimes;
- report parse failures and continue checking other files;
- use an atomic temporary-file-plus-rename write in the same directory;
- preserve file permissions.

Rae should not add a formatting configuration file. Tooling may choose which
paths to invoke, but it cannot choose a different Rae style.

## Formatter architecture

### Shared library entry points

Move formatting behind reusable functions instead of keeping it embedded in
the single-file CLI path:

```c
typedef struct {
  bool changed;
  size_t output_bytes;
  size_t output_lines;
  char* output;
} RaeFormatResult;

bool rae_format_source(
    const char* path,
    const char* source,
    size_t source_length,
    RaeFormatResult* result);
```

The CLI, build preflight, watch supervisor, tests, editor bridge, and devtools
must all call the same function. There must not be a second formatter in
JavaScript or an editor extension.

### Render to memory first

The current formatter streams directly to `FILE*`. Mandatory checking and safe
writes need the complete result in memory:

1. lex in formatter mode;
2. parse the module;
3. render canonical text to a growable buffer;
4. verify parse success;
5. verify output line count;
6. optionally reparse and compare ASTs in debug/test builds;
7. compare bytes with the input;
8. atomically write only when changed.

Rendering to memory also makes exact width measurement easier and supports
`--stdin`, JSON reporting, and editor integrations.

### Deterministic layout decisions

Do not adopt a large configurable pretty-printing framework. Rae's grammar and
style are small enough for a compact internal layout API:

- `measure(node, startColumn)` computes the actual flat width;
- `fits(node, remainingColumns)` decides compact versus vertical;
- `writeCompactList(...)` owns commas;
- `writeVerticalList(...)` owns newlines and indentation;
- one shared threshold policy is used by parameters, arguments, return items,
  fields, and collection elements.

Input line breaks must not affect the decision. The same AST always produces
the same bytes.

### Split the implementation

Before adding more policy, split `compiler/src/pretty.c` by responsibility:

```text
pretty.c          public entry point and module traversal
pretty_writer.c   buffer, indentation, width, comments, verbatim ranges
pretty_expr.c     expressions, calls, literals
pretty_decl.c     declarations, parameters, returns, statements
```

This follows the repository's source-size policy and reduces the chance that
formatter work becomes another monolith.

## Interaction with the 1,000-line limit

Rae currently rejects source files above 1,000 lines. A vertical formatter can
increase line count substantially. This is not theoretical: active files
currently include files around 927-998 lines.

Therefore default formatting cannot be enabled safely before a migration pass.

Rules:

1. The formatter renders the complete result before replacing the input.
2. If canonical output exceeds 1,000 lines, `rae format` does not overwrite the
   file.
3. It reports the original and formatted line counts and asks the author to
   split the module.
4. `rae format --check` reports this as a formatting failure.
5. The initial repository migration identifies and splits near-cap files before
   enabling the default build check.

Example:

```text
error: canonical formatting would expand lib/ui/layout.rae from 996 to 1084 lines
split the module before formatting; Rae source files are limited to 1000 lines
```

The formatter must not weaken or add an override to the language's file cap.

## Making the formatter discoverable to AI agents

No repository change can make an already-trained model globally know Rae's
style. The practical goal is that an agent entering a Rae repository discovers
the canonical command immediately and can verify its output mechanically.

Use all of these surfaces:

1. Add a short normative formatting section to the root `AGENTS.md`:

   ```text
   Rae source formatting is owned by `rae format`.
   Before completing Rae edits, run `rae format <changed paths>`.
   Do not hand-align or preserve personal wrapping.
   ```

2. Have `rae init` generate the same instruction in project `AGENTS.md`.
3. Put the canonical examples in `README.md`, `spec/rae.md`, and
   `docs/SYNTAX_RULES.md`.
4. Extend `docs/rae_syntax.json` with a versioned `format` section containing
   the command, width, indentation, and comma/newline rules.
5. Add `rae format --rules --json` so tools can query the installed compiler's
   exact policy rather than copying prose.
6. Make every formatting diagnostic include the exact repair command.
7. Add formatter invocation to SUMU/Codex/Claude task templates where Rae files
   may be edited.
8. Register the formatter in the repository's VS Code extension and eventually
   expose it through the Rae language server.

The executable remains authoritative. Prompt text helps agents find it; it
does not replace running it.

## Tests and acceptance gates

Formatter tests must cover more than golden output.

For every formatter fixture:

1. format input to output;
2. format output again and require byte-for-byte equality;
3. parse input and output and require equivalent ASTs;
4. require LF and exactly one final newline;
5. require no trailing whitespace;
6. require every breakable line to be at most 100 columns.

Add focused fixtures for:

- short and long function declarations;
- exactly three versus four parameters;
- long modifiers and return types;
- nested generic parameter types;
- short and long named calls;
- comments between parameters/arguments;
- multiline return items;
- object, list, and collection literals;
- raw strings and interpolated strings;
- `raefmt: off` regions;
- CRLF and missing-final-newline repair;
- project traversal and deterministic ordering;
- unchanged-file mtime preservation;
- atomic write failure behavior;
- near-1,000-line expansion rejection;
- `--check` and `--json` exit/output contracts;
- watch mode avoiding a formatter-triggered rebuild loop.

The repository should also have one integration test that deliberately dirties
a Rae file, proves `rae build` rejects it, runs `rae format`, and then proves the
build succeeds.

## Rollout plan

### Phase 1: stabilize policy

- Adopt this document as the formatting design.
- Change the parameter/argument compact threshold to three.
- Replace estimated declaration widths with exact measurement.
- Unify the 100-column policy.
- Add missing comment and nested-layout tests.
- Split `pretty.c` before it grows further.

### Phase 2: project formatter

- Add reusable in-memory formatting APIs.
- Add file/directory traversal.
- Make plain `rae format <path>` write in place.
- Add `--check`, `--stdout`, `--stdin`, and `--json`.
- Use atomic writes and preserve unchanged mtimes.

### Phase 3: repository migration

- Run check mode over the monorepo.
- Split active Rae files that would exceed 1,000 formatted lines.
- Format all active Rae source in one dedicated mechanical commit.
- Keep semantic changes out of that commit.
- Update examples, docs, benchmark source, and generated formatter fixtures.

### Phase 4: default enforcement

- Add format checking to `rae build` and `rae run`.
- Add auto-format-on-change to `rae watch`.
- Add `rae format --check .` before the compiler test suite.
- Add VS Code format-on-save support.
- Add concise rules to `AGENTS.md`, `rae init`, and machine-readable syntax
  metadata.

### Phase 5: formatter completeness

- Add operator-aware expression wrapping only where real source demonstrates a
  need.
- Add language-server formatting support using the same C formatter API.
- Remove deprecated implicit stdout behavior after the transition period.

## Risks

### Large mechanical migration

Canonicalizing the current monorepo will touch many files. Keep the migration
in one formatter-only commit and validate AST equivalence to preserve useful
history and reviewability.

### Comment movement

AST printers often lose exact comment attachment. Mandatory formatting should
not ship until comments before, after, and inside multiline constructs have
strong golden tests.

### File-cap failures

Vertical formatting will push near-cap modules over 1,000 lines. Those files
must be split before enforcement, not exempted.

### Build latency

Formatting the transitive source closure on every build adds parsing work.
Mitigate it with content/mtime caching in watch mode. First measure before
adding a persistent cache; Rae projects are currently small enough that a
simple in-memory comparison may be sufficient.

### Concurrent editors and watchers

Only interactive edit workflows should write automatically. Atomic replacement
and self-write event suppression are required for watch mode.

### Formatter bugs block builds

A mandatory formatter becomes part of the language frontend. Its idempotence,
AST preservation, and comment handling need the same quality bar as parsing.

## Final recommendation

Rae already has `rae format`; evolve it rather than creating another tool.

Adopt a single 100-column, two-space canonical style. Keep functions with at
most three fitting parameters on one line; format four or more parameters, or
any over-width signature, vertically with one parameter per line and no
commas. Apply the same principle to calls and data lists.

Make `build` and `run` check formatting without rewriting. Make `watch` and
editor format-on-save rewrite interactively. Add project-wide `rae format .`
and `rae format --check .`, then expose the same rules through `AGENTS.md`,
`rae init`, `docs/rae_syntax.json`, diagnostics, and editor tooling.

Before enabling the default check, split near-1,000-line modules and format the
entire active repository in one mechanical migration commit.
