# Migrating existing Rae code after the namespace + strictness overhaul

**Audience:** anyone bringing an older Rae codebase up to the current language and
conventions — in particular the game-proto app repo, which predates all of the
changes below and will trip every one of them.

This is a task-oriented checklist. Each section links the authoritative spec/doc;
read those for the full rules. The changes, and the queue items that landed them:

| Change | What it means | Authoritative doc |
|---|---|---|
| No type inference | every `let`/`var`/`const` writes its type on the LHS (#791 spec) | `spec/rae.md` §2.4 |
| Struct-literal field-name check (#778) | unknown/renamed fields fail at the literal, not at gcc | `spec/rae.md` §2.4 (end) |
| No module-level globals (#764/#766/#789) | singletons are resources on a World/App, threaded through `mod`/`view` | `docs/globals-and-app-ownership.md`, `spec/rae.md` §2.5 |
| Naming (#777/#790) | no `snake_case`; types/enums/modules PascalCase, everything else camelCase | `docs/naming-conventions.md` |
| Module model (#779/#786/#787) | project auto-visibility, folder = package, file = module namespace, `import`/`open`, `export` merge, `Package.Module.func()` qualifiers | `docs/module-namespacing.md` |
| Dropped redundant prefixes (#780) | `gpuTimingEnabled(t)` → `GpuTiming.enabled(t)` | — |

Suggested order (do them in this order to minimise re-work): **naming → module
model → no-globals → no-inference → field-name fallout**. Renaming files/folders
first means every later grep/codemod runs against the final paths.

---

## 1. No type inference — write every binding's type

**Rule (`spec/rae.md` §2.4):** every `let`/`var`/`const` states its type on the
left; a binding with no type is a **parse error**. A struct is built with the type
on the LEFT and **bare braces** on the right.

Before → after:

```rae
let count = 5                      # ERROR
let track = loadTrack(path)        # ERROR
let p = Point { x: 1, y: 2 }       # ERROR (type on the RHS)
var total = 0                      # ERROR
```
```rae
let count: Int = 5
let track: Track = loadTrack(path: path)
let p: Point = { x: 1, y: 2 }      # type LEFT, bare braces RIGHT
var total: Int = 0
```

In a `ret` there is no left-hand side, so the literal carries the type there:
`ret Point { x: 1, y: 2 }`. Nested literals whose type is not already known from
context must still be typed: `let t: Transform = { position: Pos { y: 12 } }`.

What you still write but is *not* "inference": `if let v: T = opt`, a `=>` alias's
`view T`/`mod T`, and `loop var i: Int = 0`.

**Codemod recipe** (semi-automatic — the RHS type still needs a human eye):

1. Grep for untyped bindings: `grep -rnE '\b(let|var)\s+[a-zA-Z_][a-zA-Z0-9_]*\s*='`
   and `grep -rnE '\bconst\s+[a-zA-Z_][a-zA-Z0-9_]*\s*='` (a typed binding has a
   `:` before the `=`, so filter those out: append `| grep -v ':'`).
2. For a literal RHS the type is obvious (`= 5` → `: Int`, `= 0.0` → `: Float`,
   `= true` → `: Bool`, `= "x"` → `: String`, `= []`/`createList(...)` →
   `: List(T)`). For a call RHS, read the callee's return type.
3. Rewrite `let p = Type { ... }` (type on the right) to `let p: Type = { ... }`.
4. The parser reports the exact site for anything missed — it is a hard error, so
   nothing slips through to runtime.

## 2. Struct-literal field names are checked (#778)

**Rule:** every field name in a struct literal must be a real field of the struct.
An unknown or renamed field is a **compile error at the literal** — not a surprise
from the C backend later. A field with a default may be omitted; only *unknown*
names are rejected.

```rae
type Rect { position: Vec2, size: Vec2 }
let r: Rect = { x: 0, y: 0, w: 1, h: 1 }   # ERROR: Rect has no field `x`/`w`/…
let r: Rect = { position: p, size: s }     # ok
```

This mostly bites *after* you rename a field or a type gains/loses a member: old
call sites now fail loudly at the literal. Fix each named field to the current
member name. (This check is also what makes the no-globals and naming sweeps safe
— a stale field surfaces immediately.)

## 3. No module-level globals — thread state onto an owner

**Rule (`spec/rae.md` §2.5, `docs/globals-and-app-ownership.md`):** a module-level
**`var`**, or a module-level **`let` that owns heap** (`String`/`List`/`Map`/any
struct carrying heap), is a **compile error** everywhere. A module-level `const`
and a `let` bound to a POD/string **literal** are fine (they are constants — prefer
`const`).

The replacement is **ownership**: a singleton becomes a field (a "resource") on the
`World` or `App`, threaded explicitly through the `mod`/`view` parameter that
already says who may touch it.

```rae
# BEFORE — a hidden global
var activeTheme: Theme = defaultTheme()
func themeColor(name: String) ret Color { ret lookup(activeTheme, name) }
```
```rae
# AFTER — the theme is a resource on the world, passed in
type App { theme: Theme /* …other resources… */ }
func themeColor(app: view App, name: String) ret Color { ret lookup(app.theme, name) }
```

Mechanical steps:

1. `grep -rnE '^\s*var\s' <dir>` and `grep -rnE '^\s*let\s' <dir>` at file scope;
   POD/string-literal `let`s are fine (convert to `const`), the rest must move.
2. Find the natural owner (the `World`/`App`/resource struct already threaded
   through the systems that read the global) and add the singleton as a field.
3. Add the field to that owner's `create…()` constructor.
4. Thread it: give each reader a `view Owner`/`mod Owner` parameter and read
   `owner.field` instead of the bare global. Writers take `mod`.
5. C-side runtime state may stay global behind `extern` — only the *Rae* module
   level is constrained.

## 4. Naming — no snake_case; PascalCase types/modules, camelCase the rest

**Rule (`docs/naming-conventions.md`, enforced as a hard error #790):**
`snake_case`/`SCREAMING_SNAKE_CASE` are banned. Types, enums and **modules** are
`PascalCase`; functions, parameters, locals, struct fields, enum cases and
**constants** are `camelCase`. A `const` is not special — `const maxRetries: Int`,
never `const MAX_RETRIES`.

Files and folders are modules, so they follow the same rule — with one refinement:

- A module whose **main export is a type** is named after that type, in
  **PascalCase**: `type GpuTiming` lives in `GpuTiming.rae`; a folder grouping it
  matches (`ui/RenderSystem/RenderSystem.rae`).
- A module that is a **bag of functions** with no single owning type is
  **camelCase**: `worldBiome.rae`, `gpu2dText.rae`.
- This applies to folders too (`legacyRaylib/`, not `legacy_raylib/`).
- **Non-module data on disk keeps its own scheme:** `*.wgsl` shaders, `.raescene`
  files, images, and the number-prefixed example/test dirs (`106_*`, `550_*`).

When you rename a file/folder, update every `import`/`open` path and every
`OldName.` qualifier that referenced it (they live in code, never in strings — a
`"lib/foo.wgsl"` asset path is a string and stays). See §6 for the extern-C
consequence.

**C-interop is the one exception:** `extern` function names + their params (they
name C symbols), a `type X: c_struct` name (mirrors a C struct like `div_t`), and
the generated `lib/webgpu/` bindings are skipped by the checker.

## 5. The module model — project visibility, imports, qualifiers, `export`

Full design: `docs/module-namespacing.md`. The parts that change existing code:

**Project files are one auto-visible tree — no `import`/`open` between them.** Every
`.rae` file under the project root is compiled and mutually visible; moving a file
into a subfolder does *not* create an import wall. `import`/`open` are for **`lib/`
and external packages only.** So delete intra-project `import`s.

**A folder is a package; a file is a module namespace.** Each project file belongs
to the namespace of the folder directly containing it, used only to *disambiguate*
when a bare name is defined in two folders:

```rae
enemies.tick()      # qualify by folder when `tick` exists in two folders
particles.tick()
```

**`import` path separator is `/` (a path); the qualifier is `.` (a name).** This is
the single most common thing to get wrong — the import path is slash-separated like
a filesystem path, and only the *call-site* qualifier uses dots:

```rae
import ecs/ComponentTable       # import PATH — slashes
import app3d/CameraRig
open ui/RenderSystem/RenderSystem

let t: ComponentTable(Foo) = createComponentTable(Foo)   # opened → bare
CameraRig.createCameraRig(camera: cam)                   # QUALIFIER — dots
geo.Point.make(x: 1, y: 2)                               # package.Module.func (3-level)
```

- `import M` — makes `M.func(...)` and UFCS (`value.func()`) work, but **not** bare
  `func(...)`.
- `open M` — everything `import` does, **plus** module `M`'s names become bare in
  this file (so a UI file writes `open ui/RenderSystem/RenderSystem` for its bare
  helpers). `open` implies `import`. The prelude (`core/Core`, `core/List`,
  `String`, `Math`, `Io`, `Sys`) is auto-opened for every file — bare
  `log`/`createList` need no `import` or `open`.
- Aliases: `import filesystem as fs`, `open io as console`.

**`export` merges a file into its folder's same-named main module.** To split a
module across files, a secondary file starts with a bare `export` directive; its
symbols then resolve under the folder's main module name:

```rae
# widgets/Extra.rae
export
func extraValue() ret Int { ret 41 }   # callable as widgets.Widgets.extraValue()
```

**Dropped redundant module-name prefixes (#780).** Now that the qualifier carries
the module name, function names no longer repeat it:

```rae
gpuTimingEnabled(timing)     # BEFORE
GpuTiming.enabled(timing: timing)   # AFTER — or timing.enabled() via UFCS
```

When you drop a prefix, update the call sites to the qualified/UFCS form.

## 6. Gotchas found in the rae sweep

- **A module named the same as a type resolves (#786/#721).** `Widget.makeWidget(...)`
  works when module `Widget` (file `Widget/Widget.rae`) defines `type Widget` — the
  qualifier resolves to the module, the field/type to the type. Do not rename to
  avoid a non-existent clash.
- **`extern` C symbols follow the module path.** A public-API `extern` binds to
  `rae_ext_<module-path>_<name>` (`lib/compress/oracle.rae` →
  `rae_ext_compress_oracle_deflate`). **Renaming a module renames its C symbols**,
  so a `.rae` module rename must be matched by the runtime C definition (this is a
  real break class — a missed one fails only at link, e.g. `rae_ext_sdfText_blitGlyph`
  vs the old `rae_ext_sdf_text_blitGlyph`). Exemptions: `core`/`raylib` (flat) and
  `rae_`/`__`-prefixed internal plumbing. Details: `docs/module-namespacing.md`
  ("Extern C-symbol mangling").
- **Named-call argument order is positional.** Calls are *spelled* with names
  (`f(a: x, b: y)`) but the Compiled (C) backend matches them **positionally, in
  source order** — it does not reorder to the signature. Writing the arguments in a
  different order than the function declares its parameters **passes `--emit-c`** and
  fails only at `gcc`/`clang` link ("incompatible type"). Always keep call-site arg
  order identical to the declaration, and gcc-LINK (not just emit-c) to catch it.
- **Struct-literal field-name validation (§2) is your safety net for all of the
  above.** After a field/type rename it fails at the literal, not deep in codegen —
  lean on it rather than chasing gcc errors.

## Verifying a migrated tree

Build the whole thing to Compiled C and **link it** (emit-c alone misses the
positional-arg and extern-mangling classes above), then run the test/example suite.
A green Compiled build plus a green suite is the bar — the strictness rules mean the
front end already rejects untyped bindings, unknown fields, module-level globals,
and snake_case names before the backend ever runs.
