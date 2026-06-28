# Live (bytecode VM) — status: PRESERVED BUT UNSUPPORTED

**Policy (2026-06-28):**

> **Live is preserved but unsupported. It remains buildable on a best-effort
> basis, is absent from normal Rae workflows, and is no longer a compatibility
> target for new language development.**

This is stronger and clearer than "frozen," while staying **completely
reversible** — nothing is deleted. The future replacement for Live's unique
roles (scripting, hot-reload, OTA logic) is **native-hosted WASM module
replacement** (see `docs/raepack-v2-and-packages.md`).

---

## What this means

- **No feature development for the Live VM.** Do not implement new language
  features specifically for Live, and do not work *around* VM gaps.
- **Live is not a compatibility target.** New front-end work (lexer/parser/sema)
  may keep the VM emitter *compiling* as cheap hygiene, but is under no
  obligation to preserve VM behaviour or pass VM conformance.
- **Compiled is the universal default** everywhere: CLI, tests, CI, devtools.
- **Live is absent from normal product surfaces** (devtools UI, example target
  lists) but remains **explicitly invocable** as a legacy backend.

Live is fully hidden in devtools rather than labelled "legacy" on purpose: a
visible legacy option invites users to try it, report gaps, and expect fixes.
Devtools presents the supported *future* of Rae, not its archaeological layers.

---

## Explicit (legacy) invocation

Live stays reachable only by asking for it by name:

```
rae run main.rae --target live
rae test --target live
```

These print a warning:

```
Warning: the Live VM is frozen and may not support current Rae features.
```

There is no Live chip in the devtools, and `--target live` is never a default.

---

## Tests

The Live test suite is **not** a regular responsibility. Files are kept, divided
conceptually (mechanics tracked in QUEUE):

- **Compiled tests** — the authoritative language-correctness suite. Required to
  pass. The default `make test` / CI green-gate runs **Compiled only**.
- **Live smoke tests** — a *very small* opt-in / separate non-blocking job: VM
  starts, a basic function call, a basic allocation/ownership case, perhaps one
  hot-reload case. **Not** the language-conformance gate. Acts as a canary so
  shared front-end changes don't silently brick the VM.
- **Historical Live tests** — retained but disabled.

Rationale: running a large frozen suite with known failures creates an unhealthy
definition of "green" and pressures future compiler work to preserve accidental
VM behaviour.

---

## Hot reload

State-preserving **in-process VM hot reload** remains available only through the
explicit legacy Live target. The future replacement is native-hosted WASM module
replacement (`docs/raepack-v2-and-packages.md`). A restart-based Compiled
hot-reload path also exists (`rae watch` supervisor + `lib/hot_reload.rae` /
`lib/file_watch.rae`).

This remaining unique Live capability does **not** block deprecation. Examples
that specifically demonstrate Live hot reload (`23_code_hot_reload`,
`24_code_hybrid_hot_reload`, `99_data_hot_reload`, `100_code_hot_reload`) are
kept but moved out of the normal workflow — relocated under `examples/legacy/live/`
and/or hidden in devtools (QUEUE) — so they no longer influence the standard Rae
experience.

---

## Do NOT delete yet

Keep all of it: the **VM implementation**, the **bytecode format**, the **Live
emitter**, the **existing tests**, the **Live-specific examples**, and **explicit
CLI support**. Deletion happens only *after* embedded WASM covers the important
replacement cases, and only once there's confidence that no useful VM design or
debugging machinery should be reused. Until then, soft-deprecation only.

Known Live-only gaps are non-blockers and must not gate work:
[[project_qualified_call_live_gap]], [[project_live_spawn_synchronous]],
[[project_live_target_list_struct_at]], [[project_extern_binding_two_backends]],
[[project_opt_is_some_codegen_gap]].

---

## Implementation status of this policy

Reversible product-surface changes are landing now (devtools default →
Compiled + Live chip hidden, default test target → Compiled, `--target live`
warning). The larger mechanical steps (formal test split, relocating the Live
hot-reload examples, stripping `live` from example `supportedTargets`) are
sequenced in `QUEUE.md`.
