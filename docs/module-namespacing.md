# Module namespacing for the standard library

Status: **decision record / design**, 2026-06-26. Approved direction (Option A).
Companion to `tech-stack-and-dependencies.md` and `filesystem-and-paths.md`.

This records how Rae names and accesses standard-library functions. It exists
because the stdlib currently lands every imported name in one flat scope, which
forces libraries to hand-roll namespaces with prefixes (`sdlInitWindow`,
`gpuReset`, `sdfBlitGlyph`). The prefix is a workaround for a missing feature —
this design removes the need for it.

---

## Decision: namespace-qualified stdlib modules

Standard-library APIs are organized into **module namespaces** and accessed with
the module name as an explicit qualifier:

```rae
filesystem.desktopDir()
filesystem.exists(path)
math.sin(angle)
gpu.reset()
```

Definitions inside a stdlib module are written **without a prefix** — the module
*is* the namespace:

```rae
# lib/filesystem.rae
func exists(path: view String) ret Bool { ... }
func desktopDir() ret String { ... }
```

### What does NOT change

- **Project files and subfolders** keep Rae's existing automatic visibility. A
  user project's own modules are auto-imported and callable directly, unqualified.
  Namespacing applies to the **standard library**, not the user's own code.
- **Core language essentials remain globally available** (flat), so everyday code
  is not littered with `core.` — e.g. `List`, `print`, `createList`. Only `core`
  is global; ordinary stdlib modules (`math`, `string`, `io`, `sys`, `sdl3`,
  `gpu`, `image`, `filesystem`, …) are namespaced.

---

## One general rule: qualification and UFCS are two ways to call one function

A function belongs to its module (its namespace). There is **one** definition and
**two** syntactic ways to call it — they are equivalent:

```rae
func exists(path: String) ret Bool   # lives in module `filesystem`

filesystem.exists(path)   # namespace-qualified: says where it comes from
path.exists()             # UFCS: rearranges the first argument into dot form
```

So all of these are natural and valid:

```rae
string.length(text)    text.length()
math.sin(angle)        angle.sin()
filesystem.exists(p)   p.exists()
json.jsonString(v)     v.jsonString()
```

- **Namespace qualification** (`module.func(args)`) controls *where the function
  comes from*. The LHS is an in-scope module name; resolve `func` in that module.
- **UFCS** (`value.func(args)` → `func(value, args)`) only *rearranges the first
  argument*. It is valid whenever: (1) `func` is visible through an available
  module, (2) the value's type matches `func`'s first parameter, and (3)
  resolution finds **one** unambiguous match.

There is **no `this`-only restriction and no compiler-maintained list of "flat"
modules**: namespaced functions participate in normal UFCS resolution like any
other. The LHS disambiguates the two forms — an in-scope module name → qualified;
a value → UFCS.

### Ambiguity is an error, not a silent pick

If a UFCS call `value.func(args)` matches functions in **more than one** visible
module (same name, first parameter accepts the value), the compiler reports an
**ambiguity error** and requires the explicit qualified form:

```rae
filesystem.exists(path)   # disambiguates when several modules export `exists`
```

This is what keeps UFCS from re-flattening the stdlib: collisions surface as
errors at the call site, not as an arbitrary winner.

### Bare unqualified calls

A bare `func(args)` (no namespace, no receiver) resolves only against the user's
**project** modules and the always-available **`core`** essentials (`List`,
`print`, `createList`, …). A stdlib function is reached by qualification or UFCS,
never as a bare global — that is the single mechanism that removes the dual
flat/namespaced state, with no per-module list.

### Local binding shadows a module name

If a local binding has the same name as an in-scope module, the local **value
wins** (lexical scoping) and `name.x` is read as UFCS/field access on that value.

### No namespace inside a UFCS chain

`path.filesystem.exists()` is **not Rae** and is explicitly rejected. A namespace
can never appear between a value and a function name. UFCS means *only*
`value.function(args)` → `function(value, args)`. To call a namespaced stdlib
function, use the explicit form `filesystem.exists(path)`.

---

## Why this fits Rae

- **Explicit & analyzable:** every stdlib call states its module; a tool knows the
  origin of every name with zero inference. No hidden flat pool.
- **No prefix soup:** `gpu.reset()` not `gpuReset()`; the abbreviation, if any, is
  the caller's local `as` choice.
- **No member functions:** `filesystem.exists` is a namespace path, not a method
  bound to a type. UFCS stays orthogonal free-function sugar.
- **Deterministic:** one LHS rule, lexical shadowing, no multi-namespace search —
  "few special cases."

---

## Migration plan (staged; suite green at each step)

1. **Compiler support (additive).** `import <mod>` registers `<mod>` as an
   in-scope namespace; resolve `mod.func(...)` to that module's `func`. Flat
   auto-loading of stdlib stays working *during* migration, so nothing breaks and
   new code can already use qualified calls.
2. **Migrate stdlib + examples module by module.** Drop the per-module prefix
   from definitions; update call sites to qualified form. Keep the suite at 62/0
   after each module.
3. **Flip the default.** Stop flat-auto-loading non-`core` stdlib so the
   namespaced form is the only form (except `core`). Remove the legacy prefixes.
4. **Then** introduce `lib/filesystem.rae` namespaced from day one (its first real
   consumer is the raytracer PNG-save path — see `filesystem-and-paths.md`).

The migration is done eagerly (not left half-flat-half-namespaced): once a module
is migrated and the default flipped, its functions are reachable *only* through
the namespace (except `core`).
