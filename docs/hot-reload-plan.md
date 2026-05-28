# Hot reload — design plan (Live + Compiled)

Status: design draft, not yet implemented.
Scope: end-to-end hot reload UX that works in both Live (bytecode VM)
and Compiled (C backend) targets, with state preservation across code
swaps. This document defines the model, the protocol, the failure
modes, and a phased rollout.

---

## 1. Goals & non-goals

### Goals

- **One mental model** across Live and Compiled. Authors write the same
  hot-reload hooks; the runtime picks the right strategy per target.
- **"Never left the app" feel**: route, scroll position, playback
  state, and app-defined custom state survive a code reload.
- **Three explicit operations**, distinct in user intent:
  - **Hot data reload** — reparse data files (scenes, themes,
    configs). No process restart, no recompile.
  - **Hot code reload** — swap code while preserving state.
  - **Hot restart** — clean reboot. State discarded.
- **Compile failure is recoverable** — the running app keeps running.
  Bad binaries don't replace good ones.
- **Reuse existing infra**: `lib/file_watch.rae` for in-process
  mtime polling, the Compiled-mode toolchain for recompile.

### Non-goals

- True in-process Compiled-mode code patching (function-pointer
  swap, dlopen-style). The runtime cost and edge cases (signatures
  changing, statics, struct layout) make this not worth it for now.
  We use a process restart with state passthrough instead.
- Distributed hot reload (multi-machine, networked dev). Single
  developer, single machine.
- Preserving arbitrary heap graphs. State is explicit, app-defined,
  and round-tripped through a serializable form (text).

---

## 2. The three operations

| Operation         | Trigger                      | Process restart? | State kept?           | Recompile? |
|-------------------|------------------------------|------------------|-----------------------|------------|
| Hot data reload   | data-file mtime change       | no               | yes (still in RAM)    | no         |
| Hot code reload   | source-file change + success | yes (Compiled)   | yes (via state file)  | yes        |
| Hot restart       | explicit, or schema mismatch | yes              | no                    | maybe      |

In Live mode, "hot code reload" doesn't necessarily restart the
process — the VM can re-parse and re-bind without exec. But the
**state contract is the same**: the app's `saveState` runs before
the swap, `loadState` runs after. From the app author's view, Live
and Compiled differ only in latency, not semantics.

---

## 3. What changed? source vs data

The most important distinction. The watcher has to classify file
changes:

- **Data files** — anything the running binary reads dynamically:
  `.raescene`, theme JSON, asset metadata, content. App declares
  these to `createFileWatcher(...)` (already implemented). On
  change → the app's data-reload callback runs in-process.
- **Source files** — `.rae` files that the compiler consumes:
  app source, libs, imports. A change here means the running
  binary is stale and needs to be rebuilt (Compiled) or
  re-parsed (Live).

These two sets are disjoint by convention. The supervisor knows
which is which:

- Source set = transitive closure of imports starting from the
  app's entry `.rae` file. Computed by the compiler's existing
  import resolver — exposed as `rae deps <entry.rae>`.
- Data set = whatever the app passes to its in-process
  `FileWatcher` plus a small project-level allowlist
  (`.rae/watch.toml`) for files the supervisor should track but
  the app doesn't.

If a change touches **both** sets in one batch (e.g. user saved
a `.rae` file *and* a `.raescene` in the same editor save), the
supervisor does the source path (full code reload), which
inherently re-runs init and picks up new data anyway.

---

## 4. Architecture — supervisor model

A separate process supervises the app in Compiled mode. In Live
mode it can be folded into the VM driver but keeps the same shape.

```
              ┌────────────────────────┐
              │   rae watch <project>  │  (supervisor)
              │  ─ watches source set  │
              │  ─ watches data set    │
              │  ─ owns build dir      │
              │  ─ owns state file     │
              └───┬─────────────────┬──┘
                  │ spawn/exec      │ write trigger
                  ▼                 ▼
        ┌─────────────────┐   ┌─────────────────┐
        │   app binary    │◄──┤  .rae/reload    │
        │  (Compiled or   │   │     .signal     │
        │   Live VM)      │   └─────────────────┘
        │  ─ in-proc data │
        │    file watcher │
        │  ─ saveState /  │
        │    loadState    │
        └─────────────────┘
```

### Layout under `.rae/`

```
.rae/
  build/
    current → binaries/<hash>/    (symlink)
    previous → binaries/<hash>/   (symlink, last-known-good)
    binaries/<hash>/app           (a content-addressed build)
  reload.signal                   (supervisor → app, see §6)
  state.json                      (app's serialized state)
  watch.toml                      (optional extra watch globs)
  log/
    supervisor.log
    app.stdout
    app.stderr
```

Content-addressed binaries (hashed over the source tree) mean
swaps are atomic via symlink rename, and we get free deduping
when toggling between two states during testing.

### Why a supervisor, not the app self-recompiling

- Compile failures must not crash or hang the running app. A
  supervisor owns the compiler invocation and reports failures
  out-of-band (log + maybe an overlay flag the app can render).
- Binary swap requires exec'ing a different image. The cleanest
  way is for an external parent to do the launch.
- The same supervisor handles Live too — it just chooses
  `rae run --target live` vs `rae build && exec ./app`.

---

## 5. State preservation API

Apps opt in by registering two hooks early in `main`:

```rae
type HotReloadHooks {
  saveState: func() ret String        # called just before exit-for-reload
  loadState: func(s: view String)     # called once at startup if state present
  stateVersion: Int                   # bump when schema changes
}

func registerHotReload(hooks: own HotReloadHooks) pub
```

And at startup:

```rae
# Reads .rae/state.json if present, dispatches to the registered
# loadState hook. Idempotent — safe to call when no state file.
func bootstrapHotReload() pub
```

The state itself is **a string the app produces and consumes**.
JSON is the recommended carrier (use `lib/json.rae`), but the
runtime doesn't care — it just round-trips bytes through a file.

### Versioning

`stateVersion: Int` is checked by the runtime at load time:

- If the state file's version matches → call `loadState(s)`.
- If it differs → discard state, log a notice, do a clean boot
  (effectively promoting hot-reload → hot-restart automatically
  on schema change). App author owns version bumps; doing so is
  how you say "this state shape changed, throw it away."

### What apps should put in state

Recommended:
- Current route / screen / nav stack.
- Scroll positions per scrollable container.
- Form field contents (drafts).
- Playback position + queue, if a media app.

Not recommended:
- Caches and computed values (rebuild on load).
- Anything large enough that disk I/O dominates reload latency.
- Open handles, sockets, GPU resources (re-acquire at startup).

---

## 6. Trigger protocol

`.rae/reload.signal` is a small text file written atomically by
the supervisor and watched by the app. Format is one line:

```
<verb> <build-id> <epoch-ms>
```

Verbs:
- `data` — reparse data files only. App handles in-process.
- `reload` — save state, exit cleanly with code 0. Supervisor
  swaps in the new binary and starts it.
- `restart` — exit cleanly. Supervisor starts new binary
  without restoring state.

The app's main loop polls this file (cheap mtime check, reuses
`lib/file_watch.rae`). On a verb it knows, it:

1. Finishes the current frame.
2. Runs `saveState` (for `reload`) or skips it (for `restart`).
3. Writes `.rae/state.json` atomically (write to `.tmp`, rename).
4. Exits 0.

If the app crashes or hangs, the supervisor's grace timer (e.g.
2 s) elapses, supervisor SIGTERMs, then SIGKILLs. State is lost
in that case — equivalent to hot-restart.

### Why a file, not a signal or socket

- Filesystem mtime polling is already in the stdlib. No new
  IPC dependency.
- Survives editor reloads, dev tooling restarts — both sides
  re-read it on next tick, no handshake needed.
- Cross-platform (no SIGUSR1 nonsense on Windows later).
- Trivial to inspect and reproduce in tests.

A SIGUSR1 fast path can be added later as an optimization
(supervisor sends a signal *and* writes the file; signal handler
wakes the event loop faster than mtime poll). Not in v1.

---

## 7. Compile and swap flow (Compiled mode)

```
source file changes
   │
   ▼
supervisor debounces (e.g. 150 ms)
   │
   ▼
supervisor: rae build → .rae/build/binaries/<new-hash>/app
   │
   ├── compile fails: log error, write .rae/build/error.log,
   │   leave running app alone. (Optional: tell app via
   │   .rae/build.status so it can render an overlay.)
   │
   ▼ (compile succeeds)
supervisor: write .rae/reload.signal = "reload <new-hash> <ts>"
   │
   ▼
app: notices signal → saveState → write state.json → exit 0
   │
   ▼
supervisor: app exited 0 within grace window?
   ├── no  → fall through to "binary unhealthy" below
   ▼ yes
supervisor: atomically rename build/current → build/previous,
            new build dir → build/current
   │
   ▼
supervisor: exec build/current/app
   │
   ▼
app: bootstrapHotReload() reads state.json → loadState
   │
   ▼
app: deletes state.json (consumed)
   │
   ▼
app: ready
   │
   ▼
supervisor: health check — app still alive after 3 s?
   ├── no  → "binary unhealthy", see below
   └── yes → success, drop old state.json backup
```

### Binary unhealthy / compile-fail recovery

If the new binary exits non-zero within the health window, or
if compile failed before we even swapped:

- **Keep `build/previous`** as the last-known-good binary.
  Supervisor exec's `build/previous/app` and passes the state
  file untouched (so the app comes back to where it was).
- The bad binary stays in `build/binaries/<hash>/` for forensic
  inspection; supervisor doesn't promote it to `current`.
- The supervisor surfaces the failure — stderr stream, an
  optional overlay flag, and stops trying that exact hash.
  Next source change triggers a fresh build attempt.

This means the worst case of a broken edit is: "your saved
edit didn't compile, you keep running the previous good
version." No crashes, no data loss.

---

## 8. Live mode integration

Live shares the same outer protocol (same supervisor, same
`reload.signal`, same `saveState`/`loadState` contract). The
difference is what happens after the signal:

- **Hot data reload**: identical to Compiled — app handles
  in-process via existing `FileWatcher` callback.
- **Hot code reload**:
  - Option A (simple, v1): treat Live the same as Compiled.
    The supervisor sends `reload`, the app exits, supervisor
    re-launches `rae run --target live`, app reloads state.
    Latency is dominated by VM warmup, not compile.
  - Option B (later): the VM driver itself reparses changed
    `.rae` modules in-process and rebinds globals. App still
    runs `saveState`/`loadState` to handle any state-shape
    changes. No exec. Much lower latency, but harder — needs
    careful handling of in-flight closures, type-id stability,
    and any C-backed natives that captured pointers.

V1 ships Option A so Live and Compiled behave identically.
Option B is a follow-up optimization, tracked separately.

---

## 9. CLI surface

```
rae watch [path]            # spawn supervisor, default path = "."
rae watch --no-restore      # disable state passthrough (always restart)
rae deps <entry.rae>        # print transitive source set, used by watcher
rae reload                  # manual: poke .rae/reload.signal = "reload"
rae restart                 # manual: poke .rae/reload.signal = "restart"
```

`rae watch` is the dev-loop entry point: it builds, exec's the
app, watches sources + data, and drives the protocol above.

`rae reload` / `rae restart` are convenience commands for IDE
keybindings or external tooling (Vim's `:! rae reload`, etc.).

---

## 10. Phasing

Ship in order; each phase is independently useful.

- **Phase 1 — hot data reload, done already.** `lib/file_watch`
  exists. Document it as the data-side of this design.
- **Phase 2 — state API.** Add `registerHotReload` /
  `bootstrapHotReload` stdlib hooks and the `.rae/state.json`
  read/write helpers. Apps can already use this manually with
  `rae restart`, even before the supervisor exists.
- **Phase 3 — `rae watch` supervisor (Compiled-only first).**
  Source set computation, debounced rebuild, signal file,
  process spawn + exec, content-addressed build dir.
- **Phase 4 — binary fallback / health check.** Previous-good
  symlink, post-launch health window, failure log surfacing.
- **Phase 5 — Live mode under the supervisor** (Option A).
- **Phase 6 — overlay surface.** A tiny stdlib UI widget the
  app can opt into that shows "compile error", "reloading…",
  "fell back to previous build", etc. Built on the existing
  debug-layer infra (parentless overlay, already in `lib/ui`).
- **Phase 7 (optional) — Live in-process reload** (Option B).
- **Phase 8 (optional) — SIGUSR1 fast path.**

---

## 11. Open questions

- **Source-set discovery.** `rae deps` doesn't exist yet — does
  the compiler currently expose import graph as a CLI? If not,
  the simplest v1 is "watch all `*.rae` under the project root
  + any directories listed in `.rae/watch.toml`". Coarser, but
  no compiler changes needed.
- **State file location for multi-instance dev.** If a user
  runs two `rae watch` against the same dir, they collide on
  `.rae/state.json`. Solution: namespace under the supervisor's
  pid or a `--name` flag. Defer until anyone hits it.
- **Pre-reload hooks beyond `saveState`.** Some apps may want a
  "cleanly close socket / flush log" hook distinct from state
  save. For now `saveState` is the single hook; cleanup goes in
  the existing scope-drop machinery.
- **Data-file diffing.** Today `FileWatcher` reports "something
  changed in this set". The app then has to figure out what.
  Worth surfacing per-file change events? Not required for v1.
- **Schema migration.** When `stateVersion` bumps, today we
  discard state. A future enhancement: `migrateState(old: view
  String, fromVersion: Int) ret String`. Probably not worth it —
  in dev, throwing away state is acceptable.

---

## 12. Summary

Two file-watching surfaces (in-process for data, supervisor for
source), one trigger protocol (a text file with `data | reload
| restart` verbs), one state contract (`saveState`/`loadState`,
JSON-on-disk, versioned), and one safety net (content-addressed
builds with a `previous` symlink fallback). Live and Compiled
implement the same outer contract; Compiled adds a compile step,
Live adds a VM re-parse. The user-visible model is identical:
"save the file, the app comes back where you left it; if your
edit didn't compile, you keep running the last good version."
