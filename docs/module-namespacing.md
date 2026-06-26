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

## The dot operator: two roles, one deterministic rule

Rae already uses `.` for **UFCS** (first-argument dot dispatch): `a.f(b)`
desugars to `f(a, b)` when `f`'s first parameter type matches `a`'s type. This is
free-function call sugar, not a member function. Module qualification adds a
second use of `.`. They are disambiguated by the **left-hand side**:

> **If the LHS resolves to an in-scope stdlib module name, the `.` is namespace
> qualification. Otherwise the LHS is a value and the `.` is normal UFCS / field
> access.**

- `filesystem.exists(path)` — `filesystem` is a module → qualified call;
  resolve `exists` in module `filesystem`.
- `path.exists()` — `path` is a value (a `String`) → UFCS; resolve `exists` over
  the functions **in scope** (project + core), *not* across stdlib namespaces.
- Collision (a local binding shadows a module name) → the local **value wins**
  (lexical scoping); reach the module via an `as` alias. The compiler emits a
  diagnostic when a binding shadows an imported module name.

### Unqualified UFCS does NOT search stdlib namespaces

`path.exists()` must **not** auto-search every imported stdlib module for an
`exists(String, …)`. That would re-flatten the stdlib and collide on common names
(`exists`, `open`, `reset`, `create`, `close`). UFCS dispatches only over names
already in scope (the user's project + global `core`). To call a namespaced
stdlib function on a value, use the qualified form `filesystem.exists(path)`.

### Namespace-qualified UFCS (deferred)

`path.filesystem.exists()` — UFCS dispatched into a *named* module — is an
attractive ergonomic (explicit origin + value-first reads). It is **not required
for v1**; `filesystem.exists(path)` is the canonical form. Revisit once the base
feature lands.

---

## Aliasing (optional, v1-or-later)

`import filesystem as fs` lets a *caller* choose a short local qualifier
(`fs.exists(path)`) without the library ever baking in an abbreviation. Whether
`as` ships in v1 or follows is an implementation-staging call, not a semantic one.

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

Optional follow-ups: `import … as` aliasing; namespace-qualified UFCS.
