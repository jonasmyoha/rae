# Rae Web Live Target — Design Note

**Status:** design only. No implementation yet. This document captures the
*need* and *reasoning* so the decision can be revisited later without
re-deriving the trade-offs.

## TL;DR

Add a third terminal-emission backend, **`--target web-live`**, that
transpiles Rae to TypeScript for browser/Bun deployment. Keep the existing
custom bytecode VM as the *only* Live backend for native and hybrid apps.

```
┌──────────────────────────┐
│  Frontend (lex/parse/    │
│  sema/TAST/monomorph)    │  ← shared
└──────────┬───────────────┘
           │
   ┌───────┼─────────────────────────┐
   ▼       ▼                         ▼
┌──────┐ ┌──────────────┐  ┌────────────────────┐
│ C    │ │ Bytecode VM  │  │ TypeScript (NEW,   │
│ src  │ │ chunk        │  │ deferred)          │
└──────┘ └──────────────┘  └────────────────────┘
   │            │                     │
   ▼            ▼                     ▼
Compiled     Live (native,         Web Live
(C+raylib,   in-process with       (browser /
 WASM via    Compiled, hot-        Bun, HMR via
 Emscripten) reload via            host runtime)
             vm_patch)
```

## Why we're keeping the custom bytecode VM

The hard requirement that drives this is **two-way Rae↔Compiled FFI in the
same address space**, plus `Hybrid` apps where a Compiled raylib game
hot-reloads its scripts at runtime. Today:

- `VmRegistry` lets bytecode call C runtime functions via direct function
  pointers (`OP_NATIVE_CALL`). Cost is microseconds.
- `vm_patch.c` swaps function bodies in a running VM while preserving
  global state and the call stack.
- A `.hybrid` bundle ships VM bytecode alongside the Compiled binary; the
  binary loads the bytecode at startup and can replace it on signal.
- `GOALS.md` explicitly excludes GC for determinism (60 fps games).

Replacing this with an embedded JS engine (V8 ~50 MB, QuickJS ~1 MB, Hermes
few MB) would:

- Multiply binary size for hybrid apps.
- Marshal every Rae↔C call across the JS boundary, losing the direct-call
  performance.
- Hand memory layout to a GC, breaking the determinism guarantee.
- Cede hot-reload mechanics (state preservation, partial reload) to the JS
  engine's HMR model, which is tuned for web pages, not in-process C
  hosts.

So the bytecode VM stays. It's a few thousand lines we already understand
and control end-to-end.

## Why a *parallel* TypeScript target makes sense anyway

For browser-only and devtools-style deployments, **the constraint that
drives the custom VM doesn't apply** — there is no Compiled C side in the
same process to integrate with. In that environment a Rae→TS transpiler
inherits, for free:

- V8's optimisation pipeline.
- Mature HMR (Vite, Bun's hot reload, etc.).
- Source maps and Chrome DevTools debugging.
- The existing `rae-devtools-web` Bun + TS stack — Rae code could live
  inside the dashboard without a WASM bridge.
- A trivial path to a browser playground.

The current roadmap covers the web via **WASM through the C backend**
(Emscripten). That is correct for Compiled web deployments — performance,
shared C libraries, etc. But for *Live* on the web (interactive
playgrounds, dashboards, downloadable scripts), WASM-of-bytecode-VM is a
heavier path than just emitting JS-native code.

## Comparison

| Need                                | Custom VM (today) | JS-only Live (replace) | Custom VM + Web Live (proposed) |
|-------------------------------------|-------------------|------------------------|---------------------------------|
| Native hybrid (raylib + scripting)  | ✅ direct C↔VM     | ❌ embedded JS engine  | ✅ unchanged                     |
| Two-way Rae↔Compiled FFI            | ✅ in-process      | ❌ marshalling cost    | ✅ unchanged                     |
| Native hot-reload (vm_patch)        | ✅                 | needs custom HMR       | ✅ unchanged                     |
| Web hot-reload                      | ❌ not impl.       | ✅ free via HMR        | ✅ free via TS target            |
| Browser playground                  | ❌ not impl.       | ✅ trivial             | ✅ via TS target                 |
| Deterministic, GC-free              | ✅                 | ❌ JS has GC           | ✅ native; web is opt-in JS      |
| Compiler core complexity            | baseline          | -1 (no VM)             | +1 (one more backend)            |
| Sema / TAST changes                 | n/a               | n/a                    | none — same TAST                 |

## Architecture sketch (when we get to it)

The TS target slots in beside `c_backend` as a peer:

```
src/
  c_backend.c          # → C source
  c_call.c, c_expr.c, ...
  vm_compiler.c        # → bytecode VM chunks
  vm_emit_expr.c, vm_emit_stmt.c
  ts_backend.c         # → TypeScript source        (NEW, deferred)
  ts_emit_expr.c       #                            (NEW, deferred)
  ts_emit_stmt.c       #                            (NEW, deferred)
```

It consumes the same TAST that `c_backend` and `vm_compiler` consume. No
sema/parser changes; this is purely a terminal emission format.

CLI:

```
rae build --target web-live          # Rae → TS files
rae build --target web-live --bundle # also bundle via Bun/esbuild
```

The Compiled/web story (`--target web` via Emscripten) remains separate
and unchanged.

## What we are *not* doing

- Not replacing the bytecode VM.
- Not changing `--target hybrid` (still: bytecode VM + Compiled C).
- Not changing the FFI registry for the native VM.
- Not promising `Hybrid Web` (Compiled-WASM + Web-Live-TS in one
  bundle) — that's a separate question we'll address only if a real use
  case appears.

## Open questions (to resolve before implementing)

1. **String / number model.** TS strings are immutable JS strings; Rae
   `String` is a `(uint8_t*, length)` pair. How do we map view/mod
   references — by-value copy on the JS boundary, or thread a small
   `RaeString` class through? Probably the latter.
2. **`opt T` representation.** RaeAny tag-union maps cleanly to a TS
   discriminated union. Easy.
3. **Generics.** TS has structural generics; Rae monomorphises. Emit
   monomorphised functions (consistent with C/VM) or rely on TS generics?
   The former keeps semantics identical across all three backends.
4. **`view` / `mod`.** TS has no notion of a borrow. Probably elide and
   emit plain references; document that Live-Web cannot enforce
   `view`/`mod` at runtime (sema still does at compile time).
5. **`Buffer(T)` and `__buf_*`.** Map to `Uint8Array` / `Int32Array` /
   typed arrays where elem type allows; otherwise plain arrays.
6. **External integration.** `extern func` bindings for the web target
   need a separate registry (browser/Bun globals, fetch, DOM). Defer to
   when we have a concrete consumer.
7. **Devtools integration.** Should the existing `rae-devtools-web`
   compile and run user Rae snippets via this target as a first
   consumer? If so, that drives the priority.

## Decision

Architecture decision: **add the target, defer implementation**. Land the
file-splitting and stdlib work first (current QUEUE Phase 6/7). Reopen
this when one of the following becomes real:

- The devtools dashboard wants to execute Rae snippets in the browser.
- A user requests a Rae→Bun CLI script story.
- The browser-playground discussion gets concrete.
