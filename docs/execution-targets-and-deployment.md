# Rae execution targets & deployment strategy

Status: **decision record**, 2026-06-25. Supersedes the TypeScript-target
musings; complements `live-compiled-hybrid-plan.md` and `web-live-target-plan.md`.

This document records where Rae code should *run* in the real world, why, and
what to build next. It is a strategic direction, not a finished spec — the
prototypes below exist to validate it.

---

## TL;DR decisions

1. **No TypeScript target.** We will not build a TS/JS code-generation backend.
   Its only real value was *ecosystem reach* (being consumed by existing JS
   codebases), which is not a goal for our own apps. For everything we actually
   want to ship, WASM does the job better.
2. **WASM is the primary deployment target** for apps and games — including in a
   webview on iPhone/Android. It runs near-native, in-process with any web UI,
   shares memory with zero/one copy, is OTA-updatable, and is portable across
   browser / Node / iOS WKWebView / Android WebView.
3. **The C backend stays** as the native-speed / native-API path: desktop pro
   apps, hardware and real-time interfaces, and the last 1.5–2× on proven hot
   kernels. On mobile it appears as a **native plugin** the WASM layer talks to.
4. **The Live VM is kept for now, on probation.** Its job is iteration,
   hot-reload, tooling, and concurrency testing — *not* speed (it is ~100× a C
   build). If WASM + good tooling end up covering iteration well enough, the VM
   can be dropped later. We keep it while it earns its place.
5. **One source, many substrates.** The defensible thesis: Rae is the single,
   type-checked source that emits the UI logic, the fast core, *and* the glue
   between them. The compiler keeps every target and every boundary coherent —
   removing the hand-maintained-FFI drift that makes RN/Capacitor/Flutter-FFI
   painful.

---

## The target matrix

| Target | Role | Speed vs C | Notes |
|---|---|---|---|
| **Live VM** (bytecode) | iteration, hot-reload, tooling, concurrency testing | ~1/100× | On probation; not a speed path. `spawn` runs synchronously (VM is not thread-safe). |
| **WASM** (via the C backend → wasm32) | default for apps/games; web + mobile webview | ~0.5–0.9× (≈1.1–2× slower than native; SIMD+threads narrow it) | In-process with the web UI; shared memory; OTA-updatable; portable. JIT'd inside WKWebView on iOS. |
| **C / native** | desktop pro apps, hardware, hard-real-time, last-mile speed | 1× | On mobile, ships as a native plugin driven at control rate. |

Reached cheaply: **WASM is produced *through* the existing C backend**
(`clang --target=wasm32` / Emscripten / WASI) — it is mostly toolchain config,
not a new code-generation backend. This is the key leverage point and the reason
WASM is affordable to pursue.

---

## Why WASM, not TypeScript

- **In-process, no bridge.** WASM runs inside the same engine as the web UI, so
  UI↔core calls are direct, not serialized messages.
- **Shared linear memory.** A raytracer/graphics core writes pixels into a
  `SharedArrayBuffer`; the UI draws it to canvas with zero or one copy.
- **Near-native speed.** ~1.1–2× of native for compute; with WASM SIMD128 and
  threads (SharedArrayBuffer + Web Workers) it gets close, and threads give real
  parallelism for tiled work like the raytracer.
- **OTA-updatable and App-Store-legal.** Apple permits downloading and executing
  JS *and WASM* in a webview (this is how RN/CodePush OTA works). New *native
  binary* code may not be downloaded — so the WASM layer updates over the air;
  the native layer ships in the app binary.
- **Portable.** The same `.wasm` runs in a browser, Node, iOS WKWebView, and
  Android WebView.
- **We may not need a UI codegen at all.** For our own apps the UI can be Rae
  compiled to WASM driving Canvas/WebGL/WebGPU/DOM via a thin JS bootstrap. A
  *TypeScript* codegen would only be for plugging Rae into someone else's JS
  ecosystem — not a goal — so we skip it.

---

## The iOS reality: how WASM and native C coexist

The single most important constraint, because it is counter-intuitive:

> **iOS forbids JIT for everyone except the system webview.**

Consequences:

- **WASM in WKWebView** → JIT-compiled by JavaScriptCore → *fast*, but
  **sandboxed**: it cannot call native C in-process. Reaching native means
  `WASM → imported JS function → native bridge → C` — a serializing,
  message-passing hop.
- **WASM in our own embedded runtime** (Wasmtime/WAMR/wasm3) → *can* call native
  C directly and cheaply, but **must run interpreted** (no third-party JIT on
  iOS) → much slower. (AOT-compiling WASM→native is possible but then it is a
  native binary — no OTA.)

So on iOS you cannot have "fast WASM that also calls native cheaply in-process."
**Do not try to make WASM call native C on hot paths.** Resolve it by
partitioning instead.

### The governing principle: partition by data rate

- **Control-rate crossings** (note on/off, "load this", "set parameter",
  once-per-frame "render now") — the bridge is fine. Thousands/sec, no problem.
- **Audio-rate / pixel-rate** (per-sample DSP, per-pixel ops) — must stay
  **entirely on one side**; never cross per unit.

Get the partition right and the bridge cost disappears because you cross rarely.

### Worked example: an audio engine

Audio is the worst case for a bridge and the clearest illustration:

- A real engine runs on a dedicated high-priority callback thread
  (CoreAudio/AudioUnit) with a hard ~5–10 ms deadline and zero
  allocation/locks/GC in the callback. Routing that through
  `WASM → JS → bridge → C` per buffer guarantees dropouts.
- Correct decompositions:
  - **Native AudioUnit in C**, driven by **control messages** from the WASM/UI
    side (events, params). Samples never cross the bridge. — preferred for true
    low latency / hardware.
  - Or run the DSP (compiled to WASM) **inside an `AudioWorklet`**, on the audio
    thread in the webview — no native plugin, but more constrained / higher
    latency than a native AudioUnit.

Either way: **samples stay native (or in the worklet); only events cross.**

---

## When is the C backend actually required?

The working hypothesis was *"only a desktop pro app (Logic Pro / Premiere Pro
class) really needs the C compiler; almost all other apps and games can use the
WASM engine."* That is **mostly right, with nuance** — keep the nuance:

**C / native is genuinely needed when:**

- **Hard real-time**, deadline-bound DSP/audio (native AudioUnit) — latency, not
  throughput, is the issue.
- **Hardware / OS APIs the web sandbox does not expose**: Metal/CoreML directly,
  camera/ISP pipelines, BLE, secure enclave, background execution, low-level file
  or device access.
- **Pro-desktop, data-heavy tools** (Logic Pro, Premiere Pro class): huge working
  sets, plugin hosts (AU/VST), tight integration with native frameworks, and the
  last 1.5–2× of throughput that matters at that scale.
- **Maximum-performance / console games**, or games needing a specific native
  engine, native GPU API, or store/SDK that isn't web-reachable.

**WASM is sufficient for almost everything else**, because WASM is rarely the
*throughput* bottleneck (SIMD + threads put it within ~1.1–2× of native):

- Most apps, tools, utilities, productivity, business, and 2D/casual and many 3D
  games.
- Graphics/raytracing compute (our raytracer included) — and GPU work via
  **WebGPU**, which runs in the webview and is portable (this is where a future
  GPU raytracer "step 6" should aim).

So the refined statement: **default to WASM; reach for native C only for
hard-real-time, hardware/OS access, pro-desktop scale, or top-tier game
performance** — and even then, native owns the *resource* while WASM/UI talks to
it at control rate.

---

## Recommended app shape (iPhone)

```
WKWebView  (JIT'd WASM — fast, OTA-updatable)
   ├─ UI + app logic + graphics / raytracer   → Rae → WASM (SIMD + threads; WebGPU for GPU)
   └─ control messages ↓ (coarse, cheap)      → bridge generated by Rae
Native layer  (in the app binary; fast; hardware)
   └─ audio engine, hardware / OS APIs         → Rae → C plugin (Capacitor-style)
```

- Default everything to **Rae → WASM in the webview**.
- Drop a component into a **Rae → C native plugin** only for real-time or
  hardware, and talk to it at **control rate**.
- Do **not** attempt in-process WASM→native calls on iOS (blocked by JIT rules;
  the data-rate partition makes it unnecessary).
- The TS UI codegen is **not** built. The UI is Rae→WASM + a thin JS host.

---

## Where Rae wins

The product is not "a web UI calling C." It is **Rae as the single source that
emits the UI logic, the fast core, and the control-rate bridge between them —
all from one type-checked program.** The pain in native+web stacks is the
hand-maintained, drift-prone bridge schema written twice in two languages; Rae
generates it. `view` / `own` / `mod` are exactly the vocabulary for expressing
ownership across that boundary ("this buffer is handed to the audio thread and
owned there" vs "borrowed for one frame").

---

## Plan: what to prototype next

The direction above is validated by building, in order:

1. **WASM generation (through the C backend).** Get a non-trivial Rae program —
   the raytracer is the natural pick — compiling to `.wasm` via the existing C
   backend + a wasm32 toolchain (clang/wasi-libc or Emscripten). Run it from a
   ~20-line JS harness that writes pixels into a `SharedArrayBuffer` and draws to
   a `<canvas>`. Goal: prove "fast Rae core in a browser/webview" for minimal new
   compiler work. Measure WASM-vs-native speed.
2. **WASM threads + SIMD** for the tiled raytracer — confirm real parallelism in
   the WASM sandbox (SharedArrayBuffer + workers; SIMD128).
3. **iPhone proof-of-concept app with OTA updates.** Wrap the WASM app in a
   Capacitor-style WKWebView shell on iOS. Host the WASM/JS bundle remotely and
   demonstrate **over-the-air updates** of the WASM layer without an App Store
   resubmission (native shell fixed, web/WASM layer updatable). Confirm the
   App-Store-legal split (native binary shipped; WASM downloaded).
4. **Control-rate native bridge (only if needed for the PoC).** If the PoC needs
   a native capability (e.g., audio), add one Rae→C native plugin and exercise
   the *control-rate* bridge (events/params, not samples/pixels) — and have Rae
   generate both ends of the bridge.
5. **WebGPU path** (later): target WebGPU from the WASM app for GPU compute /
   the future GPU raytracer — portable across webviews.

Each step is a stop/think checkpoint: read the result before committing to the
next. Step 1 is by far the highest value-per-effort and gates the rest.

---

## Live VM: status and exit criteria

Kept for now because it currently serves iteration, hot-reload, tooling, and
(potentially) concurrency testing. It is **not** a deployment or speed path.
Re-evaluate once WASM generation lands: if a fast WASM build plus tooling covers
the iteration loop well enough, the VM becomes a candidate for removal. Until
then it stays, but no major speed investment goes into it — the speed levers
(unboxing small structs, move-not-copy via ownership, dispatch tweaks) are only
worth pursuing if the VM proves it must stay.

---

## Open questions / risks

- **WASM toolchain choice**: wasi-libc + clang vs Emscripten — depends on how
  much of the C runtime (`rae_runtime.c`, raylib) needs to come along, and how
  graphics/IO are reached (Canvas/WebGL/WebGPU shims). Resolve in step 1.
- **Cross-boundary ABI**: struct layout, ownership, and who-frees across the
  JS↔WASM and WASM↔native lines must be designed once and generated. Rae owning
  both ends makes this tractable but it is real design work.
- **Graphics in a webview**: rendering is via Canvas/WebGL/WebGPU, not Metal.
  Fine, but it is the real integration surface; the raytracer's stream-texture
  pattern maps onto a canvas blit.
- **iOS WebGPU maturity**: check current WKWebView WebGPU support before relying
  on it for the GPU path.
- **Maintenance surface**: each target is something to keep semantically
  consistent. WASM-via-C is cheap; resist adding a TS backend (or any target)
  without a concrete need that WASM cannot meet.
