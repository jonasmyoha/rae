# Module namespacing for Rae

Status: **finalized design**, 2026-06-27. Supersedes the earlier "flat vs UFCS
modules" and "flat-off" drafts. Companion to `tech-stack-and-dependencies.md` and
`filesystem-and-paths.md`.

## Core idea

A function belongs to a module namespace and can be called two ways:

```rae
filesystem.exists(path)
path.exists()
```

Both call the **same** function. The second is normal Rae **UFCS**: the value
becomes the first argument. Namespace qualification and UFCS are **orthogonal**:

- `namespace.function(args)` explicitly identifies the module.
- `value.function(args)` is valid when the value matches the function's first
  parameter.
- There is **no** `path.filesystem.exists()` form (a namespace never appears
  inside a UFCS chain).
- There is **no** classification of modules as "flat" vs "UFCS"; no module is
  special-cased (not even `core`).
- UFCS does **not** depend on functions being flattened into global scope.

A module-level **`const`/`let`** is reachable the same qualified way:

```rae
Keys.keyW            # the module's const, resolved to its value
const restart: Int = Keys.keyR   # also valid in a const initializer (folds to the literal)
```

`Module.value` resolves to the named global; a same-named *function* does not
shadow it, since `Keys.keyW` cannot be a call.
A genuine value binding of the same name in scope does win.

This works the same for a lib module and for a project sibling (`Pal.shade`
where `Pal.rae` sits next to `Main.rae`), and in every expression position —
a plain initializer, an operand, or a `"{Pal.shade}"` interpolation part. A
project module is matched by its file name (the last path component) no matter
where the project root was inferred from. (#816)

In the C backend a module-level `const`/`let` is emitted as a
module-prefixed symbol, `rae_g_<module>_<name>` (module path with `/` -> `_`),
never under its bare Rae name — so a local named `shade` and `Pal.shade`, or two
modules' same-named consts, never collide in the C namespace. (#816)

## Project folders (one auto-visible tree; folders are namespaces, not boundaries)

A Rae **project is one automatically visible module tree**. Every `.rae` file
under the project root is compiled and mutually visible to every other project
file, with **no `import`/`open`** — across any subfolder depth. Moving a file
into a subfolder never creates an import boundary or changes its visibility.
`import`/`open` are for **`lib/` / external** packages only. The prelude
stays auto-loaded and bare-callable, unchanged: the `core/` package
(`core/Core`, `core/List`) plus `String`, `Math`, `Io`, `Sys` and `List2`.
`core` is a normal camelCase folder-package now (#818) — a package prelude
entry auto-opens its whole folder, so `log` and `List` are bare-callable with
no import.

The hash maps are **not** in the prelude: `StringMap(V)` and `IntMap(V)` live
in the `collections/` package (#819) and are asked for explicitly with
`open collections/StringMap` / `open collections/IntMap`. The rule is core =
the types the compiler itself understands (`List`); collections = pure-Rae
libraries over `List`/`Buffer` that you import. See `docs/collections.md`.


**Project root.** The tree is scanned from the project root. `--project <dir>`
sets that root explicitly, so an entry deep in the tree still sees the whole
project:

```text
rae run --project game game/ui/hud.rae   # hud.rae sees every file under game/
```

Without `--project`, the scan roots at the entry file's own directory. (The
separate lib-marker root — the nearest ancestor containing `lib/core/Core.rae`, used
to resolve `lib/` imports — is *not* the scan root, so it never drags unrelated
files in.)

**Folders are namespaces, only for disambiguation.** Each project file belongs
to the namespace of the folder that directly contains it. This is **not** a
visibility boundary — the whole project stays auto-open — it only gives a name
to qualify with. So two folders may define the same function:

```text
game/
  enemies/tick.rae     # func tick() { ... }
  particles/tick.rae   # func tick() { ... }
```

- A name that is **unique** across the project is called bare, even from a
  subfolder: `spawnWave()`.
- A name defined in **two** folders is a **compile error** when called bare —
  `'tick' is defined in multiple project folders (enemies, particles); qualify
  the call with the folder name` — instead of a silent "one wins".
- Qualify by folder to disambiguate, with **no `import`/`open`**:

```rae
enemies.tick()
particles.tick()
```

The folder qualifier is ordinary namespace qualification (same machinery as
`filesystem.exists(...)`); it just resolves against a project folder name rather
than a `lib/` package.

## Module directives: `import` and `open`

### `import module`

Makes the module available through its namespace, and makes its functions
available to **UFCS** resolution — but **not** as bare receiver-less calls:

```rae
import math
math.sin(angle)        # qualified — ok

import filesystem
filesystem.exists(path) # qualified — ok
path.exists()           # UFCS — ok

import io
io.log("Hello")         # qualified — ok
log("Hello")            # NOT introduced by import — error
```

### `open module`

Does everything `import` does **and** opens the module's namespace into the
current file's **bare** function scope (so `open` implies `import`):

```rae
open io
io.log("Hello")         # qualified — ok
log("Hello")            # bare — ok, because io is opened
```

This is how receiver-less helpers like `log()` stay convenient — a file that
wants them bare writes `open io`. No library is special-cased in the compiler.

### Aliases

```rae
import filesystem as fs
fs.exists(path)         # alias-qualified
path.exists()           # UFCS still works

open io as console
console.log("Hello")    # alias-qualified
log("Hello")            # bare names still come from opening
```

The alias renames the **explicit namespace**; opening still exposes the original
function names as bare calls.

## Name resolution

1. **`module.function(...)`** — if the LHS resolves to an imported/opened module
   name or alias, resolve as a namespace-qualified call into that module.
2. **`value.function(...)`** — otherwise normal **UFCS**: search functions from
   modules visible via `import` or `open`; the value must match the function's
   first parameter; exactly one match → use it; multiple → **ambiguity error**,
   require the explicit qualified form.
3. **`function(...)`** (bare) — resolve local/project functions, **plus**
   functions from modules declared with `open`. Modules declared only with
   `import` do **not** appear in bare lookup. Ambiguity → diagnosed, never
   resolved by arbitrary priority.

### Qualifier clash diagnostics

Two clashes are diagnosed so a `Qualifier.member` reference always resolves to
exactly one thing:

- **Enum-vs-package clash (definition-site).** If an `enum Color` and a
  folder-**package** `Color/` (a folder holding modules) both exist, `Color.x`
  would be ambiguous — an enum case `x` vs a module `x` in package `Color`. This
  is a hard error at the **enum's definition site** (caught once, not at every
  use). Enum names are PascalCase, so it only bites a PascalCase type-grouping
  folder named like an enum. Rename the enum or the folder.
- **Value-shadows-module note.** A local value named like a module **wins
  silently** (see the `keys.keyW` note above) — that is intended and never an
  error. But if reading a *field* off that local fails **and** the shadowed
  module has a member of that name, the compiler says so ("the local shadows
  module `x`, whose member `y` is hidden here") instead of leaving a bare
  unknown-field error. Resolution is unchanged; this only adds context on the
  failure path.

## Contextual keywords

`import` and `open` are **contextual** top-level keywords — special only in
module-directive position near the start of a file. Elsewhere they remain legal
identifiers:

```rae
func open(path: String) ret File { ... }
let file = open(path: path)
```

## Preserved behavior

- **Project subfolder auto-discovery/loading is unchanged.** This design governs
  how a *file* chooses namespace-only (`import`) vs opened-bare (`open`) access.
- Normal UFCS remains first-argument call sugar — `.length()`, `.split()`, list
  ops, `.jsonString()`, etc. keep working.
- No compiler-maintained list of special modules; `core` is not special-cased.
- The earlier "flat-off" design is **not** used. `ui` is **not** rewritten to
  satisfy a namespace restriction.

## Examples

```rae
import math
import filesystem
open io

let wave = math.sin(angle)
if path.exists() { log("Found file") }
let text = io.readLine()
```

Ambiguity — if both `filesystem` and `archive` export `exists(path: String)`:

```rae
import filesystem
import archive
path.exists()           # error: ambiguous; use filesystem.exists(path)
```

## Migration note

The mechanism is implemented and verified before any large call-site migration.
The earlier work already qualified the free-function stdlib call sites
(`sdl3.*`, `gpu.*`, `math.*`, `vec3.*`, `json.*`, `io.*`, …) and moved `log/logS`
into `core`. Under this model the remaining call-site work is: ensure each file
that uses bare receiver-less stdlib helpers declares `open <module>` (e.g.
`open core` for `log`/`createList`, or whatever provides them), and leave
`import`-only files to qualification + UFCS. The exact set is reported after the
mechanism lands.

## Extern C-symbol mangling (full module path)

A public-API `extern` from a stdlib module binds to a namespace-qualified C
symbol so the namespace reaches all the way down to the C ABI:

    rae_ext_<module-path>_<name>

The **complete module path** is encoded — a subfolder package is namespaced
exactly like a top-level one, so the namespace never disappears for nested
packages:

    lib/image.rae            module: image           -> rae_ext_image_loadPng
    lib/compress/oracle.rae  module: compress/oracle  -> rae_ext_compress_oracle_deflate
    lib/sys/spotify.rae      module: sys/spotify      -> rae_ext_sys_spotify_launch

Encoding (`rae_mangle_module_path`, `compiler/src/mangler.c`): path separators
`/` and `\` become `_`; segment characters (including any `_`) are left as-is, so
underscore-named top-level modules are unchanged (`sdf_text` ->
`rae_ext_sdf_text_*`). This map is non-injective only if a `/` in one path aligns
with a `_` in another (e.g. `foo/bar` vs `foo_bar`); `merge_module_graph`
(`compiler/src/main.c`) **rejects that at build time**, so distinct legal module
paths can never silently produce the same C symbol — escaping is therefore
unnecessary.

Exemptions (`is_namespaced_stdlib_extern`): `core` and `raylib` (flat by design),
and internal plumbing names that start with `rae_`/`__` (they bind to fixed
runtime symbols, e.g. `lib/ui/msdf.rae`'s `rae_ext_rae_sys_read_file`).

Library authors therefore do **not** prefix function names for uniqueness — the
package path provides it. Call qualified (`oracle.deflate(...)`,
`spotify.launch(...)`); the qualifier is the module's file stem.
