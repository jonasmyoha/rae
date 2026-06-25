# Rae cross-platform tech stack & dependency policy

Status: **decision record / living reference**, 2026-06-25. Companion to
`execution-targets-and-deployment.md` (Live/Compiled/WASM targets) and
`graphics-research.md` / `ui-rendering-tech-stack-comparison.md`.

This document records the cross-platform technology stack we intend to build on,
the dependency policy that governs it, and the staged plan. It is a strategic
direction reached through discussion — not a finished spec, and nothing here is
implemented yet. Decisions are deliberately reversible: everything foreign sits
behind a Rae-owned API.

Targets throughout: **iPhone, macOS, Linux, Windows, and desktop web/WASM.**
Future app classes: **games, a Logic-like audio app, and a video editor.**

---

## TL;DR decisions

1. **We will get rid of raylib eventually.** It is an OpenGL-era convenience
   library; it does not give us GPU compute on macOS (GL capped at 4.1), and it
   is not the right base for the WebGPU/native future. We replace it with our
   own thin layers over modern dependencies. (We may keep **one** raylib example
   in the tree as a historical/minimal reference if that stays cheap — see
   "raylib" below.)
2. **Relax the strict "no-C++ / no-Rust" rule** into a *preference ordering* plus
   a real rule (below). The relaxation is high-leverage: it retires the three
   problems the strict rule created — native WebGPU, complex-script text, and
   VST plugin hosting — in one move.
3. **WebGPU-everywhere + WGSL** is the intended GPU architecture (one API, one
   shader language, native + web identical), *provided* we accept one GPU
   implementation dependency (Dawn or wgpu-native). The C-only fallback
   (SDL_GPU native + browser WebGPU) is documented as the alternative.
4. **SDL3 for the platform layer, miniaudio for audio**, with **Rae-owned
   abstraction layers** over everything so applications never touch a
   third-party API directly.
5. **Own the small/controllable, buy the genuinely hard**, all foreign code
   behind Rae-owned seams. Rewriting hard domains (e.g. HarfBuzz) in Rae is a
   long-term ambition, not an early requirement.

---

## Dependency philosophy

### Preference ordering (a default / tiebreaker, not a hard gate)

1. **Rae** — own it when it is small and controllable.
2. **C** — the lingua franca; smallest, most portable foreign code.
3. **Any language behind a clean C API** — a well-bounded C ABI we wrap once
   ranks *above* a specific language choice. The quality of the seam matters
   more than the implementation language behind it.
4. **Rust** — preferred when consumed as a **prebuilt C-ABI library** (no Rust
   toolchain in our build).
5. **C++** — used surgically, behind a Rae API; avoid frameworks that impose
   their own architecture.
6. **Objective-C** — only for Apple-specific integration (Metal, AVFoundation,
   VideoToolbox, AUv3). It is not C++ and is unavoidable on Apple; keep adapters
   thin.

The ordering decides between *equivalent* options. For genuinely hard domains
there is often only one choice and **necessity overrides rank** — e.g. VST3 and
HarfBuzz are C++-only, so C++ wins there regardless of sitting at #5.

### The real rule

> Rewrite small, controllable pieces in Rae; use proven libraries for genuinely
> hard domains; keep **all** foreign code behind Rae-owned APIs.

Rae application code stays Rae regardless of what implements the libraries. The
abstraction layers are what make every dependency replaceable later without
rewriting applications — so they matter *more* as the language rule loosens, not
less.

### Litmus: "small and controllable enough to own in Rae?"

Own it if it has a **bounded spec, few edge cases, and no per-platform driver /
hardware / Unicode minefield.** A batched 2D renderer or an audio mixer passes.
A GPU driver, a video codec, and a text shaper do **not** — those are "buy, don't
build," potentially for a very long time. Each piece we do pull in-house also
hardens Rae's own stdlib (good dogfooding), so the loop compounds — but gate it
on this litmus so ambition doesn't drag us into reimplementing a hard domain
prematurely.

### Consumption spectrum (how a dependency enters, lightest to heaviest)

- Build-time tool in another language (never ships) — lightest.
- Prebuilt vendored library with a C ABI (link a binary; no foreign toolchain).
- Built-from-source foreign dependency (pulls a C++/Rust toolchain into the build).
- Architecture-imposing framework (JUCE/Qt-style; wants to own the app) — avoid.

Prefer the top of this spectrum. The minimalism that motivated the original
strict rule survives as a **bias**, not an absolute.

---

## Rae-owned abstraction layers (the load-bearing principle)

Applications target Rae APIs; the APIs wrap the dependencies:

| Rae API | Wraps | Notes |
|---|---|---|
| **Rae Platform API** | SDL3 | windows, input, touch, controllers, lifecycle, events |
| **Rae Render API** | WebGPU (Dawn/wgpu native; browser web) — or SDL_GPU+WebGPU in the C-only fallback | device/buffer/texture/pipeline/pass/draw/dispatch |
| **Rae Audio API** | miniaudio (+ native low-latency / AUv3 later) | device I/O + callback; engine is Rae |
| **Rae Media API** | FFmpeg / VideoToolbox / WebCodecs | demux/decode/encode by platform |
| **Rae Plugin API** | Rae plugin ABI (native C ABI + WASM); VST3/AUv3 later | portable first, ecosystem later |

Design each seam at the *intersection* of the backends it must cover, expose
stable Rae types, and never leak a third-party API into application code.

---

## The stack, layer by layer

### Platform — SDL3

C, zlib-licensed, covers **macOS/Linux/Windows/iOS/Android and web (Emscripten)**.
Replaces raylib's windowing/input. GLFW is rejected because it **cannot target
iOS**. We keep SDL3 for the platform even if we do *not* use SDL_GPU (they are
separable). On web, SDL3→Emscripten maps to canvas + DOM events, keeping one code
path.

### GPU — WebGPU-everywhere + WGSL (intended), SDL_GPU+WebGPU (C-only fallback)

See the GPU deep-dive below. Intended: one API (`webgpu.h`) and one shader
language (WGSL) on native and web. Compute supported everywhere (needed for the
GPU raytracer / video effects). The Render API stays in place regardless of
backend so the choice is reversible.

### Shaders

- **Intended (WebGPU-everywhere):** **WGSL only.** Dawn's Tint / the browser
  compile WGSL to each backend. This removes dual-authoring, SDL_shadercross, the
  SPIRV-Cross/DXC build tooling, and the need for a Rae shader transpiler.
- **C-only fallback (SDL_GPU):** SDL_GPU eats backend bytecode (SPIR-V / DXIL /
  MSL); the path is dual-author a tiny set (WGSL for web + one native source) →
  later HLSL + SDL_shadercross (accepts C++ build tooling) → eventually a Rae
  shader language/transpiler. Keep the shader set small either way.
- A **Rae shader DSL → WGSL** remains an attractive *optional* long-term ideal
  (single source, machine-transformable), but it is **not required** once WGSL is
  the single source.

### 2D rendering — Rae-owned batched renderer

Own this; it is small and controllable. A batched renderer over the Render API:
sprites, rectangles, **circles/rounded-rects/nine-slices via SDF or analytic
fragment shaders on quads** (no tessellation, crisp at any scale), gradients,
clipping (scissor for axis-aligned, stencil for arbitrary), and render targets.
MSDF text drops straight in as textured quads + the MSDF shader. Add **libtess2
(C) later**, only for genuinely arbitrary vector fills (concave SVG paths) that
SDF tricks can't express.

### Text — MSDF now, FreeType later, HarfBuzz much later

- **Static MSDF first:** keep the existing atlas; **port only the shader** to
  WGSL (+ native). MSDF is renderer-agnostic — no runtime font library needed.
  Atlas generation (msdfgen / msdf-atlas-gen) stays an offline build tool.
- **FreeType later** (C) for dynamic glyph loading / arbitrary fonts / large
  glyph sets; it has an SDF rasterizer, so dynamic atlases stay C-only.
- **Shaping (HarfBuzz)** is the hard part. Without it you get simple
  Latin/Cyrillic/Greek with basic kerning (fine for most games and Latin-market
  UI) but **cannot correctly render complex scripts** (Arabic joining, Indic
  reordering, Thai), ligatures, contextual forms, or bidi. There is no pure-C
  equal to HarfBuzz. With the relaxed rule, **HarfBuzz (C++ impl, clean C API)**
  is the answer when international text matters — it sits near the far end of
  "buy, don't build." Rewriting a shaper in Rae is a *very* long-term ambition,
  not an early requirement.

### Audio — miniaudio + Rae DSP + explicit AudioWorklet on web

- **miniaudio** (C, single-header) as the device/backend layer: CoreAudio
  (mac/iOS), WASAPI, ALSA/Pulse, AAudio, **and Web Audio via Emscripten** — all
  targets from one C API, behind the Rae Audio API.
- **Rae-owned DSP / mixer / synth / effect-graph** written in Rae → C + WASM,
  running allocation/lock-free in the audio callback.
- **Web architecture is explicit and must be designed up front:** the DSP WASM
  runs **inside an AudioWorklet** (audio-thread context), fed control-rate
  messages over a **SharedArrayBuffer ring buffer** from the main thread; samples
  are generated in the worklet and never cross to the UI thread (the
  "partition by data rate" principle from the deployment doc). Note the
  operational catch: SharedArrayBuffer requires cross-origin isolation
  (COOP/COEP) headers — plan for it in the web shell. Confirm miniaudio's
  Emscripten path uses AudioWorklet, not deprecated ScriptProcessor.

### Video (pro / later)

- **FFmpeg** (C) on desktop for demux/decode/encode/mux. Software decode of 4K is
  slow, so:
- **VideoToolbox / AVFoundation via a thin Obj-C/C adapter on Apple** for
  hardware decode/encode.
- **WebCodecs** (browser) for the lighter web version (hardware-accelerated in
  the browser; far better than `ffmpeg.wasm`).
- Effects/compositing/color → **WebGPU compute** (same GPU stack). Video is
  unambiguously native-first; the web build is the "lite" version.

### Plugins

- **Define a Rae plugin ABI first** — a native C ABI **and** a WASM-module
  interface implemented by Rae-authored plugins. It works in **both** native and
  the web build (a WASM plugin runs in the browser DAW — something VST3 never
  can), needs no C++, and fits "one source, many substrates."
- **VST3 hosting** (C++ SDK) becomes possible under the relaxed rule, but it is
  **desktop-native only** (no browser sandbox) and a *later* deepening. Use the
  **VST3 SDK surgically behind the Rae Plugin API** — do **not** adopt **JUCE**
  wholesale; it would impose its own architecture and fight Rae-owned
  abstractions.

---

## GPU deep-dive: why WebGPU-everywhere, and Dawn vs wgpu

### The constraint that forced the question

Native WebGPU has no mature pure-C implementation: **Dawn** exposes a C API but
is C++ (heavy build: Tint, SPIRV-Tools, Abseil; large binary); **wgpu-native**
exposes a C ABI but is Rust. Browser WebGPU is reachable from Rae→C→WASM via
Emscripten because the browser provides the implementation.

### Two options

- **A. WebGPU-everywhere** (relaxed rule). One API on native (Dawn or
  wgpu-native) and web (browser), **WGSL everywhere**, compute everywhere. The
  Render API becomes a thin seam (mostly papering over async device init on web).
  This is the intended direction. **You only need to relax *one* rule for this:**
  Dawn (relax C++) *or* wgpu-native (relax Rust, consumed prebuilt). They are
  partly substitutable for this goal.
- **B. SDL_GPU + browser WebGPU** (strict C-only). SDL_GPU (C, SDL 3.2, over
  Metal/Vulkan/D3D12, supports compute) is the only mature **C** abstraction over
  the native APIs, but it has **no web backend** and eats backend-native shader
  bytecode — so you carry two GPU backends *and* a multi-format shader pipeline.
  Documented as the fallback if we choose not to relax the rule.

### Recommendation

If we relax the rule (we lean to), **A (WebGPU-everywhere)** is the better
architecture — it removes the two largest ongoing complexities (two GPU backends
+ multiple shader languages). SDL_GPU was attractive *only because* of C-purity,
which we are giving up elsewhere anyway.

### Dawn vs wgpu-native

- **wgpu-native** — Rust impl, ships **prebuilt C-ABI libraries**; lightest to
  *consume* (link a binary, no Rust toolchain). Used by Firefox/Deno/Bevy.
- **Dawn** — C++ impl; reference conformance, Google-maintained; heavier build.

Lean: **wgpu-native prebuilt** for consumption simplicity, or **Dawn** for
maximum spec conformance. Either is vendored behind the Render API, so the choice
is reversible (and you could later support both).

### Honest costs of WebGPU-everywhere

- **Bigger native binaries** (Dawn especially). **Web stays light** — the browser
  provides WebGPU, so no GPU lib ships to the browser.
- **iOS-native maturity** (Dawn/wgpu → Metal) is less-trodden than desktop —
  verify before committing the native-iOS route. The WKWebView WebGPU path is
  independent (browser-provided).
- You **track the WebGPU spec's evolution** rather than SDL's frozen API.

---

## raylib: deprecation plan

**We probably want to remove raylib.** It is an OpenGL/GLFW convenience layer; it
gave us fast 2D/3D + windowing + simple shaders + basic audio, and it compiles to
web via Emscripten (WebGL) — but:

- No GPU **compute** on macOS (OpenGL capped at 4.1) → blocks the GPU raytracer.
- OpenGL is legacy / deprecated on Apple; the future is WebGPU/Metal/Vulkan.
- It overlaps with the layers we now want to own (Platform, Render, 2D, Audio).

Plan: build the Rae Platform/Render/2D/Audio layers, migrate the examples off
raylib, then drop the dependency. We **may keep a single raylib example** in the
tree as a historical/minimal reference **only if** it stays cheap to keep
building (no special-casing the build, no divergent toolchain). If keeping it
adds friction, remove it entirely — minimalism wins.

---

## Staging

### Initial — the actual raylib replacement (window/input/2D/textures/basic audio, native + web)

- SDL3 (Platform API)
- Render API over WebGPU (Dawn/wgpu native; browser web) — or the SDL_GPU
  fallback if we defer the relaxation
- Rae-owned batched 2D renderer (sprites/rects/SDF-circles/clipping/gradients/
  render targets/nine-slices)
- Static MSDF text (shader ported to WGSL)
- miniaudio basic playback
- a small WGSL shader set

### Mid

- Compute pipelines (GPU raytracer "step 6", effects)
- FreeType dynamic glyphs
- Rae DSP / mixer / synth + the web AudioWorklet architecture
- libtess2 for complex vector fills
- (optional) a Rae shader DSL → WGSL

### Pro / later

- FFmpeg / VideoToolbox / WebCodecs video
- low-latency pro-audio tuning + AUv3
- the Rae plugin ABI (native + WASM)
- HarfBuzz (complex-script text) and VST3 hosting — the two explicit
  "we accept C++ here" decisions

---

## Honest cost ledger (where choices still bite)

- **Complex-script text** needs HarfBuzz (C++); no C equal. Acceptable under the
  relaxed rule; a Rae rewrite is a very-long-term ambition only.
- **VST3 hosting** is C++ and desktop-native only; the web build can only host
  Rae/WASM plugins. The Rae plugin ABI is the portable answer.
- **Pro video and pro audio pull you native** — the web/WASM versions are the
  "lite" tier; the C backend is mandatory for the pro tier.
- **WebGPU-everywhere** trades a C++/Rust dependency + bigger native binaries +
  spec-tracking for one API + one shader language. We judge that worth it.
- **Obj-C is unavoidable on Apple** for Metal/AVFoundation/VideoToolbox/AUv3 —
  fine (not C++), keep adapters thin.
- Every relaxation is a **per-dependency** decision weighed on build weight +
  binary size + portability + maintenance — not a blanket "anything goes."

---

## Verify before committing

- iOS WebGPU status in WKWebView, and Dawn/wgpu → Metal maturity on iOS.
- SDL3 ↔ WebGPU surface creation glue on each platform (native handle → surface;
  canvas selector on web).
- miniaudio Emscripten = AudioWorklet (not ScriptProcessor); COOP/COEP headers
  for SharedArrayBuffer.
- Prebuilt wgpu-native availability/size vs Dawn build effort for our toolchain.

---

## Next step

The highest-value first move is still the **WASM-generation spike** (see
`execution-targets-and-deployment.md`): get the raytracer compiling to `.wasm`
through the C backend and drawing to a canvas. The GPU-stack decision
(WebGPU-everywhere vs SDL_GPU) can follow once we see real WASM output. The
raylib removal proceeds incrementally as the Rae Platform/Render/2D/Audio layers
land.
