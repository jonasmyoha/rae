# Rae `.raepack` v2 — Package, Module, and Target Model (Design)

**Status:** Design proposal. **Nothing here is implemented.** This document
establishes the *semantic model* first; all concrete syntax, field names, and
keywords below are **provisional and illustrative** (`executable`, `execution`,
`include`, `requests`, `grants`, `delivery`, …). Do not treat any sketch as the
final grammar — the names exist only to make the model legible.

Supersedes `docs/raepack-targets-proposal.md` (v1) once accepted. Related:
`docs/live-vm-status.md` (Live frozen), `docs/execution-targets-and-deployment.md`,
`docs/webgpu-2d-ui-renderer.md` / `docs/webgpu-3d-renderer.md` (the host APIs a
WASM module would import), `docs/tech-stack-and-dependencies.md`.

This doc does **not** change the compiler or migrate any example. It is the
agreed direction; implementation is split into independently reviewable phases
(see `QUEUE.md` #123–#132).

---

## 0. Scope & non-goals

**In scope:** the data model for `.raepack` v2 — packages, modules, targets,
capabilities, dependencies — and how `devtools.json` is absorbed.

**Non-goals (for this doc):** the final grammar; a package registry; a working
implementation; promising iOS App Store approval or arbitrary OTA executable
code; resurrecting Live.

---

## 1. Design principles & terminology

**Principles**
1. **Native is the primary application model.** iOS games, Steam games, desktop
   apps, and professional audio/video software ship as **native** applications.
2. **WASM is a first-class *embeddable module runtime*** — and the execution
   format for web applications. It is *not* merely "the web target."
3. **Hybrid is composition, not a backend.** The normal hybrid shape is a
   **native host that embeds one or more WASM modules** (OTA logic, remote/
   server-defined UI, downloadable tools, sandboxed plugins, modding,
   hot-reloaded editor logic, portable business logic). There is no `hybrid`
   backend enum.
4. **The native↔WASM boundary is an explicit *module* boundary** with a typed
   Rae interface and generated marshalling — never a free per-source-file mix.
5. **Capabilities are a deny-by-default security contract** and first-class in
   the manifest.
6. **`.raepack` is optional.** `rae run` / `rae watch` must keep working with no
   manifest via the existing zero-config defaults (see §2).
7. **Live is frozen and functionally superseded** by WASM modules for its
   scripting/hot-reload/OTA role. v2 keeps temporary compatibility but is **not**
   centered on Live.
8. **Keep the common case trivial.** A single-binary example needs ~no manifest;
   complexity (modules, capabilities, deps, OTA) is strictly opt-in.

**Terminology**
- **Package** — a unit with metadata, dependencies, modules, and targets; the
  thing `.raepack` describes. (Provisionally still `pack <Name> { … }`.)
- **Module** — a build/execution unit with an entry and an **execution mode**
  (`native` or `wasm`; `live` legacy). Modules are defined **once** at package
  level. A WASM module has a typed interface + a capability *request*.
- **Target** — a deployable configuration for a platform (or platform group). A
  target names the native executable/library, **selects** which modules to
  include, and applies platform-specific overrides + capability **grants**.
- **Platform** — `macos | windows | linux | ios | web` (extensible). Desktop OSes
  are grouped where config is shared (§10).
- **Execution mode** — how a module runs: `native` (compiled to machine code via
  the C backend) · `wasm` (sandboxed module, interpreted/embedded) · `live`
  (bytecode VM, *legacy/frozen*).
- **Host** — the process that loads and runs modules and **exposes** capabilities
  (a native app embedding WASM; the browser runtime for web; the desktop WASM
  runtime).
- **Capability** — a named, typed bundle of host functionality (e.g. "2D
  drawing", "filesystem read", "network"). Subject to the three-way model in §8.
- **Devtools presentation metadata** — name/description/category/etc. used only
  by the devtools UI; carried in the manifest but **clearly separated** from
  build/package semantics.

---

## 2. Minimal zero-config behavior (no `.raepack`)

A folder with **no manifest** still builds, runs, and (optionally) appears in the
devtools. This already partly exists (`rae run`/`rae watch` infer `main.rae` and
default to Compiled — see `docs/live-vm-status.md` / the zero-config CLI work).

Zero-config defaults (no `.raepack` present):
- **Entry:** `main.rae` in the folder.
- **Module:** one implicit `native` module = the whole folder.
- **Target:** one implicit `desktop` target for the host platform.
- **Execution:** Compiled (native). (Live is never the zero-config default.)
- **Capabilities:** a native single-binary app is not sandboxed, so there is no
  capability gate; it links whatever its imports need (raylib/SDL3/wgpu detected
  as today).
- **Presentation (devtools):** when no manifest metadata exists, the devtools
  derives a display name from the folder (`53_raytracer_webgpu_text` → "53 ·
  Raytracer Webgpu Text"), an empty description, and an "Uncategorized" (or
  number-range) category.

**Examples without a `.raepack` still appear in the devtools** (the "nice but not
mandatory" ask): the examples list is a **directory scan**, and a manifest only
*enriches* an entry (description, category, custom targets/actions). This keeps
trivial examples zero-friction — a `.raepack` is added only when an example wants
a description, a non-default category, multiple targets, or modules. (Decision in
§15: do we still want a tiny optional metadata-only file for description-only
examples, or is folder-derived enough? Leaning: optional `.raepack` with just a
`meta` block, never required.)

---

## 3. Minimal `.raepack` example

The smallest useful manifest — a name + description + category, nothing else.
Everything else falls back to zero-config defaults.

```rae
// SYNTAX PROVISIONAL
pack ClearColour {
  meta: {
    name: "2D Renderer — Step 1: Clear colour"
    description: "Opens a GPU window and presents a clear colour."
    category: "2D Renderer"
  }
}
```

No `targets`, no `modules` → implicit single `native` module, implicit `desktop`
target, Compiled. This is the file that replaces today's `devtools.json` for a
trivial example, while staying optional.

---

## 4. Native desktop application example

A normal native app, grouped desktop platforms, no WASM.

```rae
// SYNTAX PROVISIONAL
pack Raytracer {
  meta: {
    name: "Raytracer — GPU + MTSDF text"
    description: "Interactive GPU path tracer with crisp MTSDF text overlay."
    category: "Raytracer"
  }
  modules: {
    module app: { entry: "main.rae", execution: native }   // the whole program
  }
  targets: {
    target desktop: {
      platform: [macos, windows, linux]   // grouped — shared config
      executable: native
      module: app
    }
  }
}
```

Per-OS overrides only where config diverges (§10) — most desktop apps need none.

---

## 5. Web / WASM application example

A web app: the *whole* application is a WASM module, the host is the browser
runtime, and the granted capabilities map to browser-provided host imports
(canvas WebGPU, etc.). This is the one case where "the app is WASM."

```rae
// SYNTAX PROVISIONAL
pack WebUiDemo {
  meta: { name: "Web UI demo", description: "…", category: "Web" }
  modules: {
    module app: {
      entry: "main.rae"
      execution: wasm
      requests: [gpu2d, input]      // capabilities the module needs (a REQUEST)
    }
  }
  targets: {
    target web: {
      platform: web
      // The browser runtime is the host; it exposes a fixed capability set.
      module: app
      grants: { app: [gpu2d, input] }   // host grant (subset of what web exposes)
    }
  }
}
```

Note: on web there is no separate native host *you* write — the runtime is the
host. The same `app` module could also be embedded by a *native* host on desktop
(§6/§7); that's the point of WASM-as-embeddable.

---

## 6. Native iOS / Steam application embedding a WASM module

The headline hybrid shape: a **native** game/app that embeds a sandboxed WASM
module for logic that benefits from portability/updatability. Modules are defined
once; targets select them and override delivery per platform.

```rae
// SYNTAX PROVISIONAL
pack SpaceGame {
  meta: { name: "Space Game", description: "…", category: "Games" }
  modules: {
    module host:      { entry: "main.rae",          execution: native }
    module gameLogic: { entry: "logic/main.rae",    execution: wasm
                        interface: GameLogicApi      // typed boundary (§9)
                        requests: [scene, input, storage] }   // REQUEST only
  }
  targets: {
    target steam: {
      platform: [macos, windows, linux]
      executable: native
      module: host
      embeds: { gameLogic: { delivery: ota,      grants: [scene, input, storage] } }
    }
    target ios: {
      platform: ios
      executable: native
      module: host
      // App Store compliance: bake the module in rather than download code.
      embeds: { gameLogic: { delivery: bundled,  jit: false,
                             grants: [scene, input, storage] } }
    }
  }
}
```

`delivery`/`jit`/`grants` are **per-target overrides** of a package-level module —
the OTA-vs-bundled distinction lives exactly here. See §11 for delivery semantics.

---

## 7. Larger professional application (many native + WASM modules)

A Resolve/Logic-class app: a native host + native performance libraries + several
sandboxed WASM modules (plugins, scripting, server-defined UI), with deps.

```rae
// SYNTAX PROVISIONAL
pack StudioApp {
  meta: { name: "Studio", version: "0.4.0", description: "…", license: "…" }

  dependencies: {
    dep ui:     { path: "../../lib-ui" }
    dep codec:  { git: "https://github.com/acme/rae-codec", rev: "v1.2.0" }
  }

  modules: {
    module host:       { entry: "src/host/main.rae",   execution: native }
    module dsp:        { entry: "src/dsp/main.rae",     execution: native }   // perf-critical, native
    module remoteUi:   { entry: "src/remote_ui/main.rae", execution: wasm
                         interface: UiPanelApi,  requests: [gpu2d, net] }
    module scripting:  { entry: "src/scripting/main.rae", execution: wasm
                         interface: ScriptApi,   requests: [project, storage] }
    module pluginAbi:  { entry: "src/plugin_abi/main.rae", execution: wasm
                         interface: PluginApi,   requests: [audioBuffer] }   // 3rd-party plugins target this
  }

  targets: {
    target desktop: {
      platform: [macos, windows, linux]
      executable: native
      modules: [host, dsp]
      embeds: {
        remoteUi:  { delivery: ota,     grants: [gpu2d] }      // net NOT granted → denied at runtime
        scripting: { delivery: bundled, grants: [project, storage] }
        pluginAbi: { delivery: dynamic, grants: [audioBuffer] } // loaded from user plugin dir
      }
    }
  }
}
```

Note `remoteUi` *requests* `[gpu2d, net]` but the desktop target *grants* only
`[gpu2d]` — the request does not grant; net is denied (§8).

---

## 8. Capabilities — the three-way contract

**A module request must never grant access.** Three distinct concepts, kept
separate in the model (names provisional):

1. **Capability definitions — what a host *exposes*.** A host (native app, the
   web runtime, the desktop WASM runtime) declares the capability interfaces it
   *can* provide, each a typed set of host functions.
   ```rae
   // SYNTAX PROVISIONAL — host-side capability surface
   capabilities: {
     capability gpu2d:   { interface: Gpu2dHostApi }
     capability net:     { interface: NetHostApi }
     capability storage: { interface: StorageHostApi }
   }
   ```
2. **Capability requests — what a module *needs*.** Declared on the module. A
   request is a *demand*, carries no authority: `requests: [gpu2d, net]`.
3. **Capability grants — what a target/host *gives* a module.** Declared where a
   target embeds the module: `embeds: { remoteUi: { grants: [gpu2d] } }`.

**Enforcement (deny-by-default), validated at build + load:**
```
granted(module, target)  ⊆  requested(module)  ⊆  exposed(host/platform)
```
- Anything requested but not granted is **denied at runtime** (the host import is
  absent / traps).
- Granting a capability the module never requested is a manifest error
  (surfaces intent drift).
- Granting a capability the host can't expose on that platform is a manifest
  error (e.g. `net` on a sandbox that has no network host import).
- Native single-binary apps (no embedded WASM) are unsandboxed and have no
  capability gate — capabilities exist to constrain **WASM modules**.

This is the mechanism that makes modding, third-party plugins, OTA logic, and
server-defined UI safe: a downloaded/embedded module can touch *only* the host
functions its target granted.

---

## 9. Typed native↔WASM interface (syntax unresolved)

The boundary between a host and a WASM module is a **typed Rae interface** with
**generated marshalling**. Rae's advantage: one language on both sides, so the
interface is expressed in Rae types and the toolchain emits the host-import /
guest-export glue. **The exact IDL is an open question (§15)** — sketch only:

```rae
// SYNTAX PROVISIONAL — an interface contract referenced by a module's `interface:`
interface GameLogicApi {
  // host -> module (the module implements these; the host calls them)
  exports: {
    init(seed: Int)
    update(dtMs: Float) ret FrameCommands
  }
  // module -> host (the host implements these; the module imports them, gated by capability)
  imports: {
    needs scene                         // pulls in the `scene` capability's host fns
    log(message: view String)
  }
}
```

Constraints the IDL must satisfy (whatever the final form):
- Only value/handle types crossable by the marshaller (no raw pointers across the
  boundary); large data via shared buffers/handles, mirroring `gpu2d`'s opaque
  Int handles.
- Versioned (a host and a downloaded module may differ — see OTA §11).
- Capability-linked: an `imports` clause referencing a capability is only usable
  if that capability is granted by the target.

---

## 10. Target selection & per-platform override rules

- **Modules are package-level; targets select them.** A target lists native
  modules (`modules:`) and embedded WASM modules (`embeds:`). No module *bodies*
  inside targets — only selection + overrides.
- **Platform grouping:** a target may list several platforms when config is
  shared (`platform: [macos, windows, linux]`). Provide **per-OS overrides**
  (link flags, bundle/icon/signing) rather than duplicating whole targets. Split
  into separate targets only where behavior genuinely diverges (iOS lifecycle,
  web runtime).
   ```rae
   // SYNTAX PROVISIONAL
   target desktop: {
     platform: [macos, windows, linux]
     executable: native
     module: host
     overrides: {
       macos:   { bundle: "Studio.app", sign: "Developer ID" }
       windows: { icon: "studio.ico" }
     }
   }
   ```
- **Resolution precedence:** zero-config defaults < package-level module defs <
  target selection < per-OS overrides < explicit CLI flags (`--target`,
  `--profile`).
- **`rae run`/`watch` target choice:** with a manifest, use its default target
  for the current platform; `--target <id>` selects another; without a manifest,
  the zero-config desktop/native default (§2).

---

## 11. Bundled vs OTA delivery semantics & security

Per embedded WASM module, per target (provisional `delivery:`):
- **`bundled`** *(default)* — the module ships inside the app artifact. No code
  download. The only iOS-safe default. Always available offline.
- **`dynamic`** — loaded from a known local location (e.g. a user plugin
  directory) at startup. For modding/plugins. Still local code, capability-gated.
- **`ota`** — fetched/updated at runtime from a configured source. **Explicit,
  opt-in, and policy-controlled.** Requirements the model mandates:
  - **non-JIT** — interpreted execution only (`jit: false`); no native code
    generation from downloaded modules.
  - **integrity-checked** — content hash verified before load (the pure-Rae
    DEFLATE/checksum codec + monocypher are available building blocks).
  - **versioned** — module carries a version compatible with the host interface
    version (§9); incompatible → reject.
  - **rollback-capable** — a failed/incompatible update reverts to the last-good
    (or bundled) module.
  - **capability-gated** — an OTA module still only gets its target's grants.

**iOS reality (no promises):** downloadable *executable* code is restricted.
The plausible path is WASM *interpreted by Rae's own embedded VM* (data, not
code, from the platform's view; the posture JS engines / Roblox rely on), no JIT.
This document **reserves** the OTA fields and semantics but does **not** claim
App Store approval or general iOS compatibility — that is investigated separately
(§15, QUEUE #132) and defaults to `bundled` on iOS.

---

## 12. Package metadata, dependencies, and `rae.lock`

- **`meta`** absorbs `devtools.json`'s human/UI data, but **build/package
  semantics and devtools-presentation data are clearly separated** within the
  file. Provisional split:
  ```rae
  meta: {                       // package identity (build-relevant)
    name: "Studio"  version: "0.4.0"  description: "…"  license: "…"
    repository: "…"  authors: ["…"]
  }
  devtools: {                   // presentation only — never affects builds
    category: "Games"  hidden: false
    display: { width: 1280, height: 720 }
    actions: { action: { id: "…", label: "…", command: "…", target: desktop } }
  }
  ```
- **Dependencies — start with path + Git only** (registry later, §15):
  ```rae
  dependencies: {
    dep ui:    { path: "../../lib-ui" }
    dep codec: { git: "https://github.com/acme/rae-codec", rev: "v1.2.0" }
  }
  ```
- **`rae.lock`** — generated lockfile pinning each resolved dependency to an exact
  revision + content hash, for reproducible builds. Resolution feeds the
  compiler's existing module search (the `RAE_STDLIB` / `resolve_stdlib_root` /
  `try_resolve_lib_module` path is the hook). Workspaces (multiple packages under
  one root, e.g. `examples/`) are a later layer.

---

## 13. Migration — from `.raepack` v1 and `devtools.json`

- **v1 → v2:** the three existing v1 packs (`92_pong_import`, `22_raepack_demo`,
  `302_auto_app` test) are migrated by hand. v1's per-source `emit` splits map to
  module boundaries (§14). `format`/`version` already exist → bump `version: 2`;
  optionally keep a v1 parse path temporarily.
- **`devtools.json` → `meta`/`devtools` blocks:** a one-time script generates a
  minimal `.raepack` (mostly `meta` + `devtools`) from each `devtools.json`, then
  deletes the JSON. Examples needing only a description get the §3 minimal pack;
  examples needing nothing rely on folder-derived metadata (§2).
- **Devtools reads the pack only:** `readExampleMetadata` (the `devtools.json`
  reader) is removed; the devtools consumes the normalized JSON from
  `rae pack --json` (which now includes the `devtools` block). One source of
  truth for the CLI, the web buttons, and the build.
- **Order:** finalize v2 (semantics+syntax) → implement parse + JSON → make
  run/watch/build honor it → migrate devtools metadata + delete `devtools.json`.
  (QUEUE phases below.)

---

## 14. Explicitly deprecated concepts

- **Per-source `emit` (`emit: live|compiled|hybrid` on individual files)** —
  removed as the native↔WASM abstraction. The boundary is the **module**. v1
  packs migrate file-emit splits into module definitions.
- **`hybrid` as a backend / target enum value** — gone. Hybrid is *composition*:
  a native host target that `embeds` WASM modules.
- **Live as the future scripting/hot-reload/OTA solution** — no. Live (bytecode
  VM) is frozen (`docs/live-vm-status.md`) and functionally **superseded by WASM
  modules**. v2 keeps temporary Live compatibility but does not center on it; new
  designs target Compiled (native) + WASM.

---

## 15. Open questions & decisions before implementation

1. **Interface IDL (§9):** dedicated `interface { … }` syntax vs. deriving the
   boundary from ordinary Rae function signatures + annotations? Versioning
   scheme? How are handles/shared buffers expressed? *Biggest unknown.*
2. **WASM runtime choice:** which embeddable WASM engine for the native host
   (interpreter for non-JIT/iOS; optional faster engine elsewhere)? How does it
   relate to the existing Live VM (replace? share infra?)?
3. **Capability surface:** the concrete set of host capabilities and their
   interfaces (gpu2d, input, scene, storage, net, audioBuffer, project, …) — and
   which the web runtime vs a native host vs the desktop WASM runtime each expose.
4. **Naming/grammar:** finalize `executable`/`execution`/`module`/`embeds`/
   `requests`/`grants`/`delivery`/`overrides` — all provisional here.
5. **Metadata-only packs:** keep an optional metadata-only `.raepack` for
   description-only examples, or is folder-derived devtools metadata enough?
6. **Desktop grouping granularity:** how much shares under one `desktop` target
   before per-OS overrides become their own targets?
7. **Dependencies:** version-constraint syntax (path/git first), `rae.lock`
   format, workspace model, and the resolver's integration with stdlib resolution.
8. **OTA per platform (§11):** desktop OTA vs iOS — separate investigations;
   integrity/rollback/versioning protocol; what "compatible interface version"
   means concretely.
9. **Back-compat window:** how long to keep v1 parsing and the `devtools.json`
   reader before removal.
10. **Web host model:** how a `web` target's browser runtime capability surface
    is described in the manifest vs. assumed by the toolchain.

---

*End of design. Implementation is intentionally deferred — see QUEUE #123–#132.*
