# Rae UI — Rendering & Compile-Target Stack Comparison

A focused comparison of the realistic graphics-backend and compile-target
choices for Rae's UI pipeline, evaluated against what we actually need for
a "world-class" mobile-first UI demo and beyond.

Companion to:
- `docs/graphics-research.md` — original raylib decision (game-scale).
- `docs/live-compiled-hybrid-plan.md` — current Live/Compiled targets.
- `docs/ui-viewport-and-safe-area-plan.md` — the viewport / safe-area / device-sim work this stack must support.

This document **commits to recommendations**; it doesn't just survey.

## Why Reopen This?

`graphics-research.md` (T009) picked raylib for the early Pong / Tetris
demos. That was right: raylib is batteries-included, MIT, single dep,
covers windows + input + audio + simple draw calls. For game-scale 2D it
remains hard to beat.

The mobile-UI work (`examples/98_mobile_ui`) has now exposed places where
raylib *as a UI renderer* falls short:

1. **No real path rasterizer.** Rounded rects and circles are triangulated
   polygons; without `FLAG_MSAA_4X_HINT` edges are visibly jagged. Even
   with MSAA enabled, fine curves like icon outlines lose detail.
2. **Text rendering is acceptable, not world-class.** `LoadFontEx` bakes a
   bitmap atlas (`stb_truetype`-derived); bilinear filtering smooths
   non-native sizes but there's no subpixel positioning, no harfbuzz-style
   shaping, no system font integration. Material Icons render fine because
   they're square-em glyphs; complex scripts (Arabic, Devanagari) won't.
3. **No system-native text.** On iOS, "world-class" *is* CoreText. raylib
   doesn't reach into it.
4. **iOS port is fiddly.** Possible (people have done it), not first-class.

The viewport plan (`ui-viewport-and-safe-area-plan.md`) keeps the renderer
behind a thin adapter (`lib/ui/render.rae`, ~330 lines). That makes a
multi-backend story realistic, *if* we want one.

## What "Best" Means Here

Concrete criteria, weighted by what Rae actually needs:

| Criterion                                            | Weight | Notes |
| ---------------------------------------------------- | ------ | ----- |
| **2D vector quality** (paths, AA, rounded rects, gradients) | High   | Mobile UI demos depend on it. |
| **Text rendering on iPhone**                         | High   | User-stated explicit requirement. |
| **Text rendering on macOS / Windows / Linux**        | High   | Cross-platform parity. |
| **Minimal binary / build complexity**                | High   | Rae prizes clarity; bloated deps fight that. |
| **License**                                          | High   | MIT/Apache/zlib — no GPL contamination. |
| **Multi-platform reach** (iOS, Android, web, desktop)| Medium | Each platform doesn't need to be best-in-class day one, but the path should exist. |
| **Dev-loop velocity** (hot reload, devtools)         | Medium | Already strong via Live VM. |
| **AI/LLM friendliness** (predictable, analyzable backend) | Medium | The scene-graph abstraction handles this; backend is mostly behind it. |
| **Asset-pipeline simplicity** (font files, images)   | Low–Med | Existing examples are doing fine. |

## Part 1 — Compile / Transpile Targets

### Current

| Target            | What it is                                  | Strengths                                                    | Weaknesses                                  |
| ----------------- | ------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------- |
| **Live (VM bytecode)** | Rae → bytecode → in-process VM         | Fast hot reload, devtools-friendly, no native toolchain.     | No deployable binary; perf below compiled.  |
| **Compiled (C backend)** | Rae → C → native binary (clang/gcc)  | Production perf, links against any C lib (raylib, Skia, …).  | Per-platform native build; no browser path. |

These two are good. The hybrid plan covers Live + Compiled in one project.

### Candidates to add

#### A. **TypeScript transpile target**

Rae → TypeScript → run anywhere JS does (browser, Node, Electron, Bun, Deno, mobile via Capacitor / React Native).

**Pros**

- Largest deployable surface area instantly: web demos, devtools embedding, electron apps, hybrid mobile via Capacitor (royalblush-pixi already ships iOS through this path).
- Browser fonts are *already* world-class — no font work to do for web target.
- PixiJS / Canvas / WebGL / WebGPU are mature 2D backends with great AA and text.
- Source-level deploy means no native build for web; iteration loop is instant.
- TypeScript's structural-type system is compatible with Rae's types; transpile is fairly mechanical.
- Royalblush-pixi is an existing proof point that ECS-driven Rae-style UI works at scale in TS + Pixi.

**Cons**

- We'd carry an extra backend. Test surface doubles.
- TS isn't a runtime — to embed into a host (game console, microcontroller, …) you'd still want C.
- Performance for tight numeric inner loops is ~5–20× slower than C.

**Verdict:** **Highly worth adding** as a third target, specifically for **web/devtools/Electron deployment**. It's not a replacement for Compiled C; it's complementary.

#### B. **WASM target** (Rae → WASM directly)

Rae → custom WASM emitter, or Rae → C → wasm-clang. Either way, WASM bytes that run in browsers / wasmtime / wasmer / SpiderMonkey.

**Pros**

- One artifact, runs in browser and on any WASM runtime.
- ~Native speed (within 5–15% of compiled C in good cases).
- Sandboxed; can be embedded into other apps safely.

**Cons (for UI specifically)**

- **WASM can't draw.** It has to call into the host: in a browser that means JS Canvas/WebGL/WebGPU; outside a browser, host-supplied imports. So you're always pairing WASM with another UI lib. The WASM choice doesn't *solve* the UI question.
- **WASM ↔ JS calls have overhead.** Drawing thousands of small primitives via per-call FFI is slow. Best practice: build a command buffer in WASM, hand it to JS as a typed array, let JS run the actual draw calls. That works but it's an extra layer.
- **WASM has weak interop with the DOM today.** Direct DOM access (still useful for accessibility, input, layout debug) goes through wasm-bindgen-style glue.
- We'd need a non-trivial WASM emitter, *or* go via wasm-clang on the C output (cheap but loses some Rae-aware optimization opportunities).

**Verdict:** **Defer.** For Rae's actual UI deployment story, **TypeScript transpile is strictly easier** and faster to ship. WASM is interesting later for sandboxed embedding (running untrusted Rae code in a host app — devtools, plugin system) but it's not the right answer to the "ship a UI" question.

#### Compile-target recommendation

Three targets, each with a clear job:

| Target          | When to use                                                      |
| --------------- | ---------------------------------------------------------------- |
| Live (VM)       | Local dev loop, hot reload, devtools, AI agent eval.             |
| Compiled (C)    | Production native binaries — macOS, Linux, Windows desktop. Eventually iOS / Android via native toolchains. |
| TypeScript (new)| Web deploy, devtools embedding, Electron, Capacitor-mobile.      |

WASM stays on the radar; we add it when there's a concrete embedding need.

## Part 2 — Renderer / Graphics Library

The scene-graph (`lib/ui/`) is renderer-independent. The renderer adapter
(currently `lib/ui/render.rae`) makes a small number of primitive calls.
Swapping renderers is a per-target adapter, not a rewrite of Rae UI code.

### Candidates

#### 1. **raylib** (current)

- C, MIT, single dep on OpenGL/Metal via GLFW. Window + input + 2D + 3D + audio + math.
- **2D vector**: triangulated paths, no analytic AA. MSAA helps but won't match Skia/Canvas.
- **Text**: `stb_truetype` atlas, bilinear filter. Acceptable for Roboto/Inter/Material at fixed sizes. No shaping, no subpixel.
- **iOS**: possible (community ports). Not first-class.
- **Build**: trivial.
- **Binary**: ~1.5 MB static.

Fine for the *current* example. Begins to feel cheap as soon as you want crisp circles at any size, complex-script text, or polished gradient/blur effects.

#### 2. **Skia** (Google)

- C++ (with a C API + Rust bindings), BSD. The 2D engine behind Chrome, Android, Flutter, LibreOffice.
- **2D vector**: industrial-strength. Analytic AA, GPU + CPU paths, every primitive Canvas has.
- **Text**: SkParagraph + HarfBuzz + per-platform shaper (CoreText on macOS/iOS, DirectWrite on Win, FreeType on Linux). System fonts integrated.
- **iOS**: first-class.
- **Build**: notoriously painful. Hundreds of MB of source. Custom GN-based build system. Pre-built binaries from skia-pack help but bind us to a curator.
- **Binary**: ~5–15 MB static depending on backend selection.

Best-in-class quality. Painful infrastructure cost. The Flutter / Chrome legacy of correctness is a massive advantage. The build complexity is a real ongoing tax.

#### 3. **Blend2D + HarfBuzz + FreeType (+ CoreText/DirectWrite)**

- Blend2D: zlib, ~300 KB compiled. JIT'd 2D rasterizer, similar quality to Skia's CPU path, often faster on CPU.
- HarfBuzz: MIT, ~400 KB. Industry-standard text shaping.
- FreeType: FTL/GPL2, ~600 KB. Glyph rasterizer.
- For "world-class" macOS/iOS text: skip FreeType, talk to CoreText directly.

- **2D vector**: excellent. CPU only by default (GPU path exists in Blend2D but less mature).
- **Text**: world-class once you wire HarfBuzz → FreeType (or CoreText on Apple platforms).
- **iOS**: works (it's just C++). CoreText is the natural text backend on iOS regardless.
- **Build**: each piece is small and self-contained. You're integrating ~3 libraries instead of one mega-dep.
- **Binary**: ~1.5 MB total static, *less than Skia*.

A pragmatic, almost-as-good-as-Skia stack at a fraction of the build complexity. Trade-off: you glue the pieces yourself; Skia comes pre-glued.

#### 4. **NanoVG**

- C, zlib, ~3000 LOC single file. OpenGL/Metal-backed 2D primitives + text.
- **2D vector**: very good AA, Canvas-like API, fast.
- **Text**: bakes a font atlas (better than raylib's). No shaping. Some forks add FreeType.
- **iOS**: works (it's tiny C, builds anywhere).
- **Build**: drop two files into the project.
- **Binary**: ~150 KB.

The Goldilocks option for "small, focused, looks crisp". Drops below world-class for text — for English/Material it's great; for shaped scripts no.

#### 5. **Native platform UIs** (the "let the OS draw" strategy)

- macOS: Cocoa / Core Graphics / CoreText. Free.
- iOS: UIKit / SwiftUI. Free.
- Windows: D2D / DirectWrite. Free.
- Linux: GTK or Qt or directly Pango + Cairo + FreeType.

Per-platform native bindings; each backend implements `lib/ui/render.rae`'s primitives against the host UI API. Quality is automatically best-in-class because the OS does it.

- **Build**: per-platform binding maintenance. Most expensive option in dev time, cheapest in runtime quality.
- **Binary**: ~zero (system libs).
- **Reach**: as native as you can get.

The right answer for **shipping a real app** — but not for the dev loop.

#### 6. **Browser/Web: PixiJS or native Canvas**

When the target is TypeScript transpile, the renderer is whatever the
browser provides:

- **HTML Canvas2D**: built-in, fast enough for UIs of this complexity, world-class text via the browser.
- **PixiJS**: WebGL-backed, fast, scene-graph-friendly, the choice in royalblush.
- **WebGPU**: emerging, useful for fancier effects later.

For Rae's TS target, **PixiJS is the right pick**: it pairs naturally with an ECS, accepts texture handles, has good text support via the browser, and royalblush proves the pattern.

### Renderer comparison summary

| Backend         | 2D vector | iOS text | Cross-plat text | Build pain | Binary    | License   |
| --------------- | --------- | -------- | --------------- | ---------- | --------- | --------- |
| raylib          | adequate  | atlas    | atlas           | trivial    | ~1.5 MB   | MIT       |
| Skia            | best      | world-class | world-class | high       | ~5–15 MB  | BSD       |
| Blend2D + HB + FT (+ CoreText) | excellent | world-class | world-class | medium     | ~1.5 MB   | mixed[*]  |
| NanoVG          | very good | atlas    | atlas           | trivial    | ~150 KB   | zlib      |
| Native UIs      | OS-quality | OS-quality | OS-quality   | high       | ~zero     | per-OS    |
| PixiJS (web)    | very good | OS via browser | OS via browser | low (npm) | ~200 KB | MIT       |

*[*]: HarfBuzz is MIT, FreeType is FTL/GPL2 dual — we'd ship under FTL; redistributable.*

## Key Insight: Decouple, Don't Choose Once

Rae's scene-graph (`lib/ui/`) is renderer-agnostic. `render.rae`'s primitive
surface is small (rects, rounded rects, circles, text, sprite). **Each
target gets the renderer that's right for it:**

```
                                ┌─── render.rae adapter (~300 lines per backend)
                                │
Rae UI scene-graph (lib/ui/) ───┼─── raylib       (native desktop, current)
                                ├─── PixiJS       (web/devtools, via TS transpile)
                                ├─── CoreText+CG  (iOS native — phase 4)
                                └─── Skia or NanoVG (when raylib's limits hurt)
```

The cost of adding a backend is "rewrite `render.rae`'s ~300 lines against
a new primitive API". That's a weekend of work per backend, not months.

The cost of building a Skia toolchain is *also* a weekend, but recurring
every time the build breaks. The cost of CoreText bindings is mostly
writing the Swift bridge once.

## Recommended Stack(s)

### Today → six months: stay with raylib, add TS transpile

| Layer        | Choice                                  | Why                                                     |
| ------------ | --------------------------------------- | ------------------------------------------------------- |
| Compile      | Live (VM) + Compiled (C) + **add TS**   | TS gets us web deploy + devtools embedding cheaply.     |
| Desktop renderer | raylib (with MSAA on)               | Works, simple, the gaps are visible but tolerable.      |
| Web renderer | PixiJS via TS transpile                 | Royalblush-proven. Browser fonts are free.              |
| iOS          | Defer. (Live VM in a Swift host is feasible; native deploy via TS+Capacitor works today.) | Don't pay native-iOS cost until we have a product reason. |

### When raylib's gaps start blocking: phase in NanoVG or Blend2D

- **First pain point**: jagged AA on curves at small sizes. Action: add a `nanovg` renderer adapter alongside `raylib`. Selected via `--renderer` flag at build time. ~1 weekend.
- **Second pain point**: text shaping (someone authors a Rae UI in Arabic). Action: integrate HarfBuzz + FreeType behind text rendering in the NanoVG path. ~3 days.

### When shipping a polished iOS app: native CoreText backend

- Write a Swift wrapper that consumes the Rae scene-graph and dispatches to UIKit / CoreAnimation / CoreText.
- Skip the cross-platform renderer for iOS entirely; native quality, native perf, native a11y.
- ~2 weeks for a first cut.

### What we **don't** pick

- **Skia**: heavy dep, build complexity outweighs the quality win for our scale. If we ever need it (chart-heavy apps, complex animations, blur effects with shadows on shadows), it's a swap. Not today.
- **WASM**: defer until we have a sandboxing need. TS transpile beats it for web.
- **Multi-backend by default**: keep one renderer per target; let users pick. Don't try to ship every Rae app with every renderer linked in.

## Concrete First Step

Land the viewport / safe-area / device-preset plan
(`ui-viewport-and-safe-area-plan.md` Phase 1). This is **renderer-agnostic**:
the work is in Rae source files (`lib/ui/viewport.rae`,
`lib/ui/safe_area_system.rae`) plus the scene file. None of it commits us
to raylib *or* to a successor.

The renderer choice can stay deferred until a concrete pain point names
itself. The most likely candidate to act on first is "TS transpile +
PixiJS for the web/devtools target" — independent work, complementary,
unblocks running Rae UIs in `rae-devtools-web` directly without raylib.

## Open Questions

1. **Cost of a TS transpile target.** Likely 1–2 weeks for a first version
   that compiles `lib/ui/` cleanly. Worth scoping with a small spike
   (transpile `lib/core.rae` and confirm shape of output) before committing.

2. **iOS Live VM vs. compiled.** On iOS, App Store rules historically
   blocked JIT bytecode VMs. Rae's VM is an interpreter (no JIT), so it's
   probably fine — but worth verifying once an iOS demo is on the
   roadmap. If blocked, only Compiled C / TS-via-Capacitor ship on iOS.

3. **Asset pipeline for multi-renderer.** Right now `assets/album_art/*.jpg`
   is loaded by raylib's `LoadTexture`. PixiJS uses `Texture.from(...)`.
   Native iOS uses `UIImage`. Each backend's renderer-adapter handles its
   own loading; the scene only carries a string key. That works, but the
   asset *files* may need format constraints (PNG everywhere, etc.). Easy.

4. **Text font portability.** Roboto / Material Icons Round are shipped in
   the example. On web/PixiJS, we can load them as web fonts. On native
   iOS, we could use system SF Pro instead — but then the look diverges.
   Trade-off: brand consistency vs. native feel. Probably ship our fonts
   on every platform for a consistent product.

5. **Animation primitives.** Not covered above. Royalblush has them
   (`AnimFrames`, `AnimTrigger`, `WobbleFx` etc.). The renderer-agnostic
   approach holds: animation components mutate scene values per frame;
   any renderer just draws the current frame. No change to this analysis.

## TL;DR

- **Keep Live + Compiled.** They're load-bearing.
- **Add TypeScript transpile** as a third target for web/devtools/Capacitor-mobile deploy. PixiJS is the natural renderer there.
- **Skip WASM for now.** TS does the same job for our actual use cases, with less glue.
- **Keep raylib for now on native desktop.** Move to NanoVG or Blend2D+HarfBuzz when the AA / text-shaping gaps actually block product work.
- **For polished iOS later, plan a native CoreText backend.** Not Skia, not raylib — let the OS draw.
- **Don't pick once.** The scene-graph is renderer-agnostic; each target gets the renderer that fits.
