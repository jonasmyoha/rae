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
