# Rae WebGPU 2D UI Renderer — Design

**Status:** Design / pre-implementation (2026-06-28)
**Target:** Compiled (C backend) + Web (WASM), via WebGPU-everywhere. Live VM is
frozen (`docs/live-vm-status.md`) and is a non-goal here.
**Supersedes the "phase in NanoVG/Blend2D" hedge** in
`docs/ui-rendering-tech-stack-comparison.md` — see the decision below.

---

## 1. Goal

A small, fully-owned, GPU-driven 2D vector renderer that draws **high-quality,
analytically-antialiased UI** — rounded rects, borders, circles, pills, lines,
gradients, shadows — composites **crisp MSDF text** in the same pass, and can
**embed 3D content into the UI** (and run **fullscreen 3D**) by compositing
offscreen render targets. Particles and custom per-widget shaders are stretch
goals that fall out of the same architecture.

It is **data-driven and ECS-based**: the renderer consumes a flat list of draw
primitives produced from the existing `lib/ui` ECS (`UiWorld`,
`ComponentTable(T)`), never an immediate-mode call graph.

This is the linchpin of **raylib deprecation** (`lib/ui` still imports raylib in
9 files). The new renderer replaces raylib *as a UI renderer* and the SDL3 CPU
framebuffer blit path as the *production* 2D path.

---

## 2. Decision: build our own

`docs/ui-rendering-tech-stack-comparison.md` and
`docs/tech-stack-and-dependencies.md` (§"2D rendering — Rae-owned batched
renderer") already lean this way. Confirming it:

- **Skia** — rejected. Hundreds of MB of source, GN build system, binds us to a
  curator. Quality win not worth it at our scale.
- **Blend2D (+ HarfBuzz + FreeType)** — rejected *for this product*. It is
  **CPU-first**; its GPU path is immature. It is a C dependency that **fights
  WebGPU-everywhere and the web target**, and would *block* the GPU/3D-in-UI/
  fullscreen-3D goals while buying a fast CPU 2D path we don't need.
- **NanoVG** — closest in spirit (GPU, SDF-ish), but atlas-based text and an
  OpenGL/own-GL heritage; we'd still be adapting it to WebGPU. At that point we
  own most of the surface anyway.
- **Our own SDF-quad renderer** — **chosen.** GPU-driven, *higher* quality than
  Blend2D's raster AA (analytic SDF AA needs no MSAA, crisp at any zoom), tiny,
  fully controllable, reuses our WGSL + compute infra, and is the **only** path
  that satisfies WebGPU-everywhere + web without a heavy C dep. The cost is the
  WebGPU *render* binding — which we must build regardless for 3D-in-UI and
  fullscreen 3D.

Honest hardest parts (none are research problems — all careful engineering):
1. The dual-backend WebGPU **render** binding (wgpu-native C + browser JS behind
   one Rae API).
2. Nested rounded **clipping** + correct alpha **ordering**.
3. Uniform/storage **buffer layout alignment** portability (std140-ish).

---

## 3. What exists today vs. what's new

**Exists:**
- `lib/gpu.rae` — generic WGSL **compute** dispatcher (storage/uniform buffers,
  kernels, dispatch, readback). Reusable for compute-driven particles.
- `lib/webgpu.rae` — raytracer compute + blit-to-screen.
- `lib/sdf_text.rae` — MSDF/MTSDF text, **CPU** blit into a packed-0xRRGGBB
  `Buffer(Int)`. The atlas + median-of-3 logic ports directly to a GPU shader.
- `lib/sdl3.rae` — window, input, `updatePixels`/`present` (CPU framebuffer).
- `lib/ui/*` — mature ECS: `UiWorld`, generic `ComponentTable(T)`, pages, scroll,
  layout, theme, transform, hero, animation. **Backend-agnostic except the 9
  files that import raylib** (components, input, render, text_measure, textures,
  hero, event_loop, msdf, debug).

**New (the project):**
- A Rae-facing WebGPU **render** API (extern, dual-backend): render pipelines,
  vertex/instance buffers, bind groups, samplers, textures, render passes,
  swapchain/surface config, blend/stencil/scissor state, render-to-texture.
- A batched 2D renderer + WGSL shader set over that API.
- A draw-list extractor that walks `UiWorld` → primitive instances.
- A compositor that orders 2D passes and 3D render targets onto the swapchain.

---

## 4. Core technique — instanced quads + analytic SDF AA

**No tessellation.** Every UI element is *one quad* (two triangles, or a single
fullscreen-style quad expanded in the vertex shader) carrying per-instance data:

```
struct Primitive {       // per-instance, std140-aligned (16-byte rules!)
  rect:        vec4<f32>,  // x, y, w, h  (in UI px, pre-projection)
  cornerR:     vec4<f32>,  // per-corner radius: TL, TR, BR, BL
  fill:        vec4<f32>,  // premultiplied RGBA
  border:      vec4<f32>,  // premultiplied RGBA
  params:      vec4<f32>,  // borderWidth, shadowSoftness, kind, clipId
  uv:          vec4<f32>,  // atlas/texture sub-rect (text & images)
}
```

The fragment shader evaluates a **rounded-box signed distance function** and
antialiases **analytically** with screen-space derivatives:

```wgsl
let d = sdRoundedBox(localPos, halfSize, cornerR);
let aa = fwidth(d);                  // ~1px screen-space band
let cov = 1.0 - smoothstep(-aa, aa, d);
```

One rounded-box pipeline thus gives **filled rect, rounded rect, border/stroke,
circle (r = halfSize), pill, and drop shadow (wider AA band)** — ~80% of a
mobile UI from a single shader, crisp at any DPI with **no MSAA**.

Pipeline families (kept deliberately small):
1. **Box** — the SDF rounded-box uber-shader (fills, borders, shadows, gradients
   via a second color + axis in `params`).
2. **Text** — MSDF glyph quads sampling the atlas texture (median-of-3 + analytic
   AA). Shares projection + blend state with Box, so text composites in-order.
3. **Image** — textured quads (icons, photos, *3D render-target output*).
4. **Custom** *(Tier 2)* — a widget supplies its own fragment shader on a quad.

---

## 5. Frame / render graph

```
UiWorld (ECS)
  └─ extract()  ──►  DrawList { boxes[], glyphs[], images[], clips[] }   (CPU, per frame)
                         │  sort by (layer, clip, pipeline) — painter's order
                         ▼
                     upload instance buffers (one map/write per pipeline)
                         ▼
   ┌─────────────────── Compositor ───────────────────┐
   │  pass A: 3D scene → offscreen render target(s)    │  (depth, optional MSAA)
   │  pass B: 2D UI    → swapchain (or offscreen)      │  (Box, Image[=3D RT], Text)
   │  pass C: overlays / fullscreen quad               │
   └────────────────────────────────────────────────┘
                         ▼
                     surface.present()
```

- **2D layer needs no depth buffer.** A stable painter's-order draw + alpha
  blending is enough (UI is 2.5D). Group by pipeline to minimize state switches.
- **Static UI caching** (Tier 2): render unchanged subtrees once into an
  offscreen target, reuse until invalidated.
- **This compositor is a node in a shared render graph.** The future 3D renderer
  (`docs/webgpu-3d-renderer.md`) generalizes this fixed compositor into a
  data-driven render graph; the 2D UI is its final node, drawing onto the
  swapchain after 3D producers. Design the compositor so it slots in as a node,
  not a hardcoded sequence.
- **Color space.** Blend in **linear light** (`docs/color-management-plan.md`),
  premultiplied alpha. The UI composites **after** any 3D tonemap + color-grade,
  in display-referred space (sRGB / Display-P3); the swapchain format / sRGB-view
  handles the encode. When there is no 3D layer, the UI still works in linear and
  outputs display-referred — same path, fewer nodes.

---

## 6. Subsystems & key issues (in rough cost order)

1. **WebGPU render binding (dual backend) — the bulk of the work.** WGSL is a
   single shared shader source (the WebGPU-everywhere win). The *host* resource
   API must be bound twice: wgpu-native (C, Compiled/desktop) and browser WebGPU
   (JS, web), behind one Rae API. Resources are opaque handles (Int ids, like
   `lib/gpu.rae` already does). Scope per tier in §8.
   **Anticipate the 3D renderer:** even though Tier 0 only needs a single
   swapchain pass, design the binding so it can express **depth buffers, MSAA,
   multiple render targets (MRT), and HDR (`rgba16f`) formats** —
   `docs/webgpu-3d-renderer.md` extends this exact binding rather than forking a
   second one. Don't bind those features yet; just don't paint the API into a
   no-depth/LDR corner.
2. **Buffer layout alignment.** std140-style 16-byte alignment differs subtly
   between backends and fails *silently*. Keep instance/uniform structs tiny,
   explicit, and `vec4`-padded. Add a layout-assertion test.
3. **Clipping.** Scissor rect for axis-aligned clips (scroll containers — cheap,
   the common case). Per-instance **SDF clip** (a `clipId` indexing a small
   uniform array of rounded-rect clip regions) for rounded/nested clips (a
   rounded card containing a scroll list). Stencil reserved for arbitrary paths.
4. **Alpha ordering.** Painter's order from the extract sort; premultiplied
   alpha everywhere to make compositing associative.
5. **Text.** MSDF atlas → GPU texture + sampler; reuse `sdf_text` measurement
   and atlas loading, swap the CPU blit for the Text pipeline. Latin/Cyrillic is
   free; complex-script shaping (HarfBuzz) stays deferred per
   `docs/tech-stack-and-dependencies.md` §Text.
6. **Coordinate system / DPI / resize.** Orthographic projection in a uniform,
   top-left origin y-down, retina/mobile DPI scale, pixel snapping for crisp 1px
   borders, swapchain resize + (browser) context-loss handling. Align with
   `docs/coordinate-system.md`, `docs/ui-coordinate-and-responsive-layout.md`,
   `docs/ui-viewport-and-safe-area-plan.md`.
7. **Render loop.** Reuse the hybrid busy/idle policy from
   `docs/ui-render-loop-performance.md` (busy-render while animating, block when
   idle). New animation sources MUST feed the `animating` flag
   (`project_event_loop_animating_gate`).

---

## 7. 3D-in-UI and fullscreen 3D

The render-target capability (§6.1) is exactly what unlocks this:

- **3D embedded in UI** — the 3D scene renders into an **offscreen render
  target** (own passes, depth buffer, optional MSAA); that texture is composited
  into the 2D UI as an **Image** quad. With SDF clipping you get 3D content
  inside a **rounded card**.
- **Fullscreen 3D** — same target sized to the window drawn as a fullscreen quad
  (or render straight to the swapchain), with UI as an **overlay** pass on top.
- So **UI and 3D are separate render passes sharing one swapchain**; the
  compositor orders them. The existing raytracer GPU path is the first 3D
  producer to plug in here.

---

## 8. Particles & effects (stretch)

- **Particles** — instanced quads (point sprites). Simulation either CPU or, for
  scale, a **compute pass** via the existing `lib/gpu.rae` (we already have
  compute!), output buffer consumed as instance data by a particle pipeline.
- **Blur / glow / shadows-on-shadows** — separable blur in offscreen passes.
- **Gradients** — encoded in the Box shader (`fill` + second color + axis).

---

## 9. ECS & data-driven integration

The renderer is a **consumer of ECS data**, never an immediate-mode API:

- Drawable state lives in `ComponentTable(T)` components on `UiWorld` entities
  (already how `lib/ui` works): transform, style (fill/border/radius/shadow),
  text, image-handle, clip, layer.
- A pure **`extract` system** walks alive entities → a flat `DrawList` of
  instances. This is the only per-frame O(n) hot path; **no nested per-entity
  scans** (`CLAUDE.md` §UI rendering loops — keep it O(n), sparse-set lookups).
- The renderer backend becomes a **draw-list sink**, so `lib/ui` stops importing
  raylib: introduce a thin `lib/ui/draw.rae` interface (submit box/glyph/image/
  clip) with a WebGPU implementation; the raylib path can remain alongside until
  examples are migrated (nothing removed — Live-VM-freeze discipline).

---

## 10. Examples roadmap (deliverables)

Numbered to sit alongside existing examples (#100_font_example, #53 text-over-3D):

1. **`text_webgpu`** — MSDF text via the new Text pipeline (the GPU sibling of
   `100_font_example` / `53`). Proves: render binding, atlas texture, analytic
   text AA, projection/DPI.
2. **`vector_2d`** — 2D vector graphics showcase: rounded rects, borders,
   circles, pills, gradients, shadows, lines — the Box uber-shader stress test.
3. **`ui_basic`** — simple data-driven UI (a panel with buttons, a list, a
   toggle), rendered only (minimal interaction). Proves: ECS extract → DrawList,
   clipping, layering, text+box compositing.
4. **`mobile_ui_webgpu`** — full mobile UI, **ported from
   `examples/98_mobile_ui`** onto the new renderer (nav tabs, scrollable cards,
   screens/routing). The real apples-to-apples comparison vs. the raylib version.
5. *(stretch)* **`ui_3d_embed`** — the raytracer (or a spinning mesh) inside a
   rounded UI card + a fullscreen toggle. Proves render targets / compositor.

All target Compiled (and Web where the example is web-appropriate).

---

## 11. Phasing

- **Tier 0 — BASE renderer.** Render binding (pipeline, instance buffer, bind
  group, sampler, texture, single render pass to swapchain, blend, scissor),
  ortho uniform, Box uber-shader (rounded AA rect + border + shadow), Text
  pipeline, DPI/resize. → Examples 1–2, much of 3.
- **Tier 1 — composition.** Offscreen render targets, Image pipeline, 3D-embed
  compositor, fullscreen 3D, SDF clip (rounded/nested). → Examples 3–5.
- **Tier 2 — polish/effects.** Stencil arbitrary clip, gradients, blur/shadow
  passes, compute-driven particles, per-widget custom shaders, static-subtree
  caching.

`lib/ui` raylib removal happens *after* Example 4 proves parity, module-by-module,
suite green at each step.

---

## 12. Compiler & language gaps — fix, don't work around

**Policy (per the maintainer):** when this work surfaces a compiler or language
limitation, **fix the language/compiler** rather than contort the Rae code
around it. Each gap gets a QUEUE item and a fix; this section is the running log.
No silent workarounds — if a workaround is unavoidable as a stopgap, it is
documented here with a linked QUEUE item to remove it.

Known gaps likely to be hit by a GPU render binding + ECS renderer (seeded from
prior findings; expand as they surface):

- **Opaque/handle FFI ergonomics.** The render API will pass many GPU resource
  handles. `lib/gpu.rae` uses bare `Int` ids today. Decide whether the language
  needs a first-class opaque-handle/newtype story or whether Int ids stay — and
  fix extern/typing if the Int-id pattern proves unsafe.
- **`extern` must bind on both backends.** An extern only in the C runtime fails
  on web/other backend (`project_extern_binding_two_backends`). The render API
  must be defined for wgpu-native *and* browser WebGPU together.
- **List(Struct) ownership / deep-copy** (`project_list_struct_ownership`) —
  `List(T).add` doesn't deep-copy inner `String`/heap fields. The DrawList and
  component tables are `List`-heavy; fix the deep-copy semantics rather than
  hand-cloning.
- **`own String` param + replace-alias double-free**
  (`feedback_own_string_param_and_replace_alias`) — fix the underlying codegen.
- **`mod String` / `mod` heap write-back is a latent no-op**
  (`project_mod_param_writeback`) — numeric `mod` works; heap `mod` doesn't.
- **`opt is some` not lowered in C backend** (`project_opt_is_some_codegen_gap`)
  — fix the lowering (currently must use `is not none`).
- *(Live VM gaps are out of scope — VM is frozen, `docs/live-vm-status.md`.)*

New gaps get appended here with: symptom, minimal repro, the *fix* (not the
workaround), and a QUEUE id.

---

## 13. Open questions

- Handle/resource lifetime: explicit `destroy` vs. arena/scope-tied GPU resources?
- One uber-pipeline with a `kind` switch vs. a few specialized pipelines — bench
  pipeline-switch cost vs. branch cost on mobile GPUs.
- Clip representation ceiling: how many simultaneous SDF clip regions before we
  must fall back to stencil?
- Web vs. native surface/swapchain config differences — enumerate early.
- Text shaping boundary: when (if) HarfBuzz enters for international UI.
