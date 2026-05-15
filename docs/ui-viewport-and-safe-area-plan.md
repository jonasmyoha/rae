# Rae UI — Viewport, Safe Area, and Device Simulation Plan

## Objectives

- Give the Rae UI library a clean separation between **design coordinates**, **viewport coordinates**, and the **OS window**, so a `.raescene` written for "an iPhone-sized canvas" can render on a desktop window of arbitrary size.
- Make **safe areas authorable** as components on a scene node (one entity per safe region), rather than baked into hand-tuned padding constants per app.
- Allow the desktop preview to **simulate a specific phone** — `iPhone 15 Pro`, `Pixel 7`, etc. — with the correct viewport size and notch/home-bar insets, switched by a single config or env var.
- Lay groundwork for a future **editor view**: a portrait phone window centred inside a larger desktop window, with debug UI in the surrounding margin and a 2D camera (zoom/pan) over the phone for tooling.

This is the structured port of the patterns observed in
`own_repos/royalblush_codex/royalblush-pixi`, applied to Rae's existing
ECS-driven UI runtime under `lib/ui/`.

## Current Baseline

- `examples/98_mobile_ui/config.rae` hardcodes `screenW = 600`, `screenH = 1079` and uses them directly for the OS window size, the layout root extent, and every internal dimension. Aspect ratio (≈0.556) does not match an iPhone (≈0.461).
- `lib/ui/layout.rae:30-31` carries module-level `screenWidth: Float = 600.0` / `screenHeight: Float = 1300.0` globals used as the parent-content extent for layout roots. No notion of "design coords" distinct from "window pixels".
- `SafeArea` exists as a type in `lib/ui/components.rae` and is listed in `registry.rae:registeredComponentNames()` — but no system reads it, no `.raescene` uses it, and `LayoutSystem` does not fold safe insets into effective padding. Padding around the iPhone notch is currently hand-tuned in `examples/98_mobile_ui/config.rae:outerPad`.
- No render-target indirection — the example renders directly to the back buffer at the OS window's framebuffer size. No editor camera. No simulator UI.
- `rae/docs/royal-blush-ruics-ecs-reference.md` already describes the
  intended `SafeArea` + `SafeAreaSystem` shape (lines 222 and 1102) but
  the implementation hasn't been done.

## Reference: Royalblush Pixi Patterns

Source-of-truth references (read-only — used here as a study target):

- **`utils/layoutSimulation.ts`** — static table of `DevicePreset` (id, label, w, h, safe{Top,Bottom,Left,Right}), plus a singleton `LayoutSimulationState` (`{enabled, safeEnabled, width, height, safeTop, safeBottom, safeLeft, safeRight, presetId}`) with subscriber API.
- **`utils/layout.ts:computeLayoutContext`** — per-frame derivation of a `LayoutContext` carrying three coordinate systems (design / view / screen) plus the scale and offsets between them, plus the active safe insets.
- **`ui/ruics/types.ts:SafeArea`** — `{ enabled: bool, apply: {l,t,r,b: bool}, extra?: number }`. `SafeInsets` — `{l, t, r, b}` numbers written by the runtime.
- **`ui/ruics/systems/SafeAreaSystem.ts`** — each frame, for every entity with a `SafeArea` component, write a `SafeInsets` derived from the active `LayoutContext` (scaled by `1/layout.scale` so insets stay in design units).
- **`ui/ruics/systems/LayoutSystem.ts:getEffectivePadding`** — base `Padding` + (per-edge `SafeArea.apply` × `SafeInsets`) + `SafeArea.extra`.
- **`public/assets/layout/main-menu.raescene`** — authored `"SafeArea"` entity with `Size: Fill/Fill`, `Padding: 0`, `SafeArea: { enabled, apply.{l,t,r,b}=true, extra: 0 }`, whose `Children` are the actual app roots.
- **`ui/DeviceSimulatorPanel.ts`** — buttons that call `enableLayoutSimulation(preset)`; everything else (layout, safe area, scene) reacts to the state change via the subscriber.
- **`ui/editor/UIEditorController.ts`** — `cameraScale` (0.25..5) and `cameraOffset` outside the layout pass, with `zoomAt(screenX, screenY, scale)` doing the around-cursor math via "unzoom screen point → design point".

## Proposed Architecture

### 1. Three Coordinate Systems

Adopt the royalblush vocabulary, made explicit in a new `lib/ui/viewport.rae`:

| Name              | Meaning                                             | Drives                                |
| ----------------- | --------------------------------------------------- | ------------------------------------- |
| **Design space**  | Authoring units a `.raescene` speaks in.            | All node `Rect.w/h`, font sizes, gaps |
| **Viewport space**| What the layout's root sees as its available extent.| Layout pass (`LayoutSystem`)          |
| **Screen space**  | Actual OS window framebuffer.                       | `BeginDrawing`, editor UI, blits      |

In the no-simulation case, `viewport == screen` and `design == viewport` — single coordinate system, layout works as today.

In the simulation case, `viewport` is the simulated device's logical resolution, the `design` is rendered to a `viewport`-sized render texture, and that texture is centred inside the actual `screen`.

```rae
type Viewport {
  designWidth: Float       # authoring grid; layout root content size
  designHeight: Float
  viewWidth: Float         # simulated device or actual window
  viewHeight: Float
  screenWidth: Float       # actual OS window framebuffer
  screenHeight: Float
  scale: Float             # design → viewport (cover-fit)
  pillarboxOffsetX: Float  # design centring inside viewport
  pillarboxOffsetY: Float
  simOffsetX: Float        # viewport centring inside screen
  simOffsetY: Float
  safeTop: Float           # in design units
  safeBottom: Float
  safeLeft: Float
  safeRight: Float
  simulated: Bool
  presetId: String         # for diagnostics; "" when no preset
}
```

Single global instance lives next to the world. `viewportSystem(viewport, world)` runs once per frame *before* `layoutSystem` to refresh the derived numbers from the chosen preset (or from the live window if simulation is off).

`LayoutSystem`'s module-level `screenWidth`/`screenHeight` in `lib/ui/layout.rae:30-31` go away; the layout root reads its extent from the viewport instead.

### 2. `SafeArea` as an Authored Entity

Implement `lib/ui/safe_area_system.rae`. Roughly 25 lines:

```rae
func safeAreaSystem(world: mod UiWorld, viewport: view Viewport) {
  # For every entity that opts in via the SafeArea component, write a
  # SafeInsets in design units. Layout reads SafeInsets via the
  # effective-padding accessor.
  let entities: view List(Int) => world.alive
  for entity in entities {
    if not componentHas(world.safeAreas, entity) { continue }
    let insets: SafeInsets = {
      l: viewport.safeLeft
      t: viewport.safeTop
      r: viewport.safeRight
      b: viewport.safeBottom
    }
    componentSet(world.safeInsets, entity, insets)
  }
}
```

Update `entityPadding()` in `lib/ui/layout.rae`:

```rae
func entityPadding(world: view UiWorld, entity: Int) ret Insets {
  let base: Insets = ...           # as today
  if not componentHas(world.safeAreas, entity) { ret base }
  let safe: SafeArea = componentGet(world.safeAreas, entity)
  if not safe.enabled { ret base }
  let insets: SafeInsets = componentGet(world.safeInsets, entity)
  ret Insets {
    l: base.l + (if safe.apply.l then insets.l else 0.0) + safe.extra
    t: base.t + (if safe.apply.t then insets.t else 0.0) + safe.extra
    r: base.r + (if safe.apply.r then insets.r else 0.0) + safe.extra
    b: base.b + (if safe.apply.b then insets.b else 0.0) + safe.extra
  }
}
```

Authored usage in a `.raescene`:

```json
"SafeArea": {
  "Rect": { "x": 0, "y": 0, "w": 0, "h": 0 },
  "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fill"} },
  "Padding": { "l": 0, "t": 0, "r": 0, "b": 0 },
  "SafeArea": {
    "enabled": true,
    "apply": { "l": true, "t": true, "r": true, "b": true },
    "extra": 0
  },
  "Children": ["TopBar", "Hero", "TitleRow", "Controls", ...]
}
```

The current `outerPad: 20.0` hack in `config.rae` can shrink to zero (or stay as a non-safe inner gutter) — the iPhone notch reserve is now structural.

### 3. Device Preset Table

`examples/98_mobile_ui/device_presets.rae` (or `lib/ui/device_presets.rae`):

```rae
type DevicePreset {
  id: String
  label: String
  width: Float
  height: Float
  safeTop: Float
  safeBottom: Float
  safeLeft: Float
  safeRight: Float
}

func devicePresets() ret List(DevicePreset) {
  let list: List(DevicePreset) = createList(DevicePreset)(initialCap: 16)
  # Ported verbatim from royalblush utils/layoutSimulation.ts.
  list.add(value: { id: "iphone-15-pro",     label: "iPhone 15 Pro (393×852)",     width: 393.0, height: 852.0, safeTop: 59.0, safeBottom: 34.0, safeLeft: 0.0, safeRight: 0.0 })
  list.add(value: { id: "iphone-15-pro-max", label: "iPhone 15 Pro Max (430×932)", width: 430.0, height: 932.0, safeTop: 59.0, safeBottom: 34.0, safeLeft: 0.0, safeRight: 0.0 })
  list.add(value: { id: "iphone-14",         label: "iPhone 14/13 (390×844)",      width: 390.0, height: 844.0, safeTop: 59.0, safeBottom: 34.0, safeLeft: 0.0, safeRight: 0.0 })
  list.add(value: { id: "iphone-13-mini",    label: "iPhone 13 mini (375×812)",    width: 375.0, height: 812.0, safeTop: 50.0, safeBottom: 34.0, safeLeft: 0.0, safeRight: 0.0 })
  list.add(value: { id: "iphone-se-2",       label: "iPhone SE (375×667)",         width: 375.0, height: 667.0, safeTop: 20.0, safeBottom:  0.0, safeLeft: 0.0, safeRight: 0.0 })
  list.add(value: { id: "pixel-7",           label: "Pixel 7 (412×915)",           width: 412.0, height: 915.0, safeTop:  0.0, safeBottom:  0.0, safeLeft: 0.0, safeRight: 0.0 })
  list.add(value: { id: "galaxy-s23",        label: "Galaxy S23 (360×780)",        width: 360.0, height: 780.0, safeTop:  0.0, safeBottom:  0.0, safeLeft: 0.0, safeRight: 0.0 })
  # ...
  ret list
}
```

Switching is one call: `viewport.setPreset("iphone-15-pro")`. In phase 1 it's driven by a `RAE_UI_DEVICE` env var so the snapshot tooling can iterate over presets without UI.

### 4. Phone-In-Window Rendering

When `viewport.simulated == false`, render is unchanged: layout/transform/render the world to the back buffer.

When `viewport.simulated == true`:

1. Allocate (lazily, on size change) a `RenderTexture` sized `viewport.viewWidth × viewport.viewHeight`.
2. `BeginTextureMode(rt)` → `clearBackground(bg)` → `renderSystem(world)` → `EndTextureMode()`.
3. `BeginDrawing()` → clear actual screen → `DrawTextureEx(rt.texture, simOffsetX, simOffsetY, scale=1)` → draw editor / debug UI in screen coords → `EndDrawing()`.

Adds maybe 40 lines to `examples/98_mobile_ui/main.rae` and is independent of every other layout change. Raylib's `LoadRenderTexture` / `BeginTextureMode` already exist; bindings need a one-time pass.

A simple device frame around the phone — rounded rect, dark grey, maybe a faked status bar — is purely a draw call on the screen layer, optional polish.

### 5. Editor Camera (zoom/pan) — Future Phase

Wrap step 4's final `DrawTextureEx` in a `Camera2D` (raylib has `BeginMode2D` / `EndMode2D`). The camera has `zoom` (0.25..5) and `target` (pan position). Wheel zooms around cursor; middle-button drag pans.

The crucial detail (from `UIEditorController.zoomAt`): convert screen-pointer coords back to design-space coords by **inverting the camera transform first**, then the sim/pillarbox offsets, then dividing by `scale`. That keeps "click landed on this widget" working at any zoom.

Not needed for the basic iPhone-aspect ask; lands when we want a real editor view.

## Migration Path

### Phase 1 — Viewport + SafeArea + Preset (one-day work)

1. Add `lib/ui/viewport.rae` (the `Viewport` struct + `viewportSystem`).
2. Add `lib/ui/safe_area_system.rae` (`safeAreaSystem`).
3. Update `LayoutSystem.entityPadding()` to fold `SafeInsets`.
4. Update `lib/ui/layout.rae` to read root extent from a `view Viewport` instead of the module-level `screenWidth/Height` globals.
5. Add `examples/98_mobile_ui/device_presets.rae` and an env-var driven selection in `main.rae`.
6. Rewrite `examples/98_mobile_ui/assets/scenes/music-player-now-playing.raescene` to wrap its children in a `"SafeArea"` entity (`Fill/Fill`, `SafeArea: enabled+all-edges`).
7. Adjust `config.rae` constants: `outerPad` shrinks (its job is now just an extra gutter, not notch padding); other dimensions rescale to the chosen design size — keep the example at 393×852 logical points so we get the iPhone aspect "for free".

Phase 1 deliverable: the example renders at iPhone-15-Pro aspect, with safe-area padding applied automatically from the preset, and switching presets via `RAE_UI_DEVICE=pixel-7` reflows the entire UI without code changes.

### Phase 2 — Render-to-texture phone-in-window

8. Add raylib bindings for `LoadRenderTexture`, `BeginTextureMode`, `EndTextureMode`, `UnloadRenderTexture` if missing.
9. Switch `main.rae`'s draw loop to render-into-texture when `viewport.simulated`.
10. Centre the texture on the screen; optional device frame around it.

Phase 2 deliverable: the desktop window can be any size; the simulated phone stays its preset's size and sits centred inside, with margin around it for future debug UI.

### Phase 3 — Editor camera

11. Wrap step 9's `DrawTextureEx` in a `Camera2D`. Add wheel-zoom-around-cursor and middle-drag-pan.
12. Add inverse-transform helpers so the existing pointer-input system (`lib/ui/input.rae`) still hits the right entities at non-1x zoom.

Phase 3 deliverable: dev can zoom in on a single button while the simulator keeps the iPhone proportions; debug overlays land in the margin around the phone.

## What We're Explicitly Not Doing

- **No multi-window simulator UI in Rae itself.** Picking the preset is an env var (phase 1) and later an in-app debug toggle (phase 2/3). A multi-pane editor with a tree, an inspector, and a properties panel is out of scope here — `rae-devtools-web` is the right home for that.
- **No CSS `env(safe-area-inset-*)` fallback path.** Royalblush uses it for real-iPhone Safari deployment; raylib + native phones use different APIs and we'll add it when the example actually ships on a device.
- **No tablet / landscape preset support.** The `Viewport.scale` already does cover-fit so landscape works mathematically, but `SafeAreaSystem` assumes portrait-edge semantics. Landscape gets a follow-up when we have a landscape example.
- **No automatic asset rescaling.** Bitmap assets (icons, album art) are loaded at fixed resolution. raylib's bilinear filter handles small zoom/pan; when an asset is consistently off it's a manual re-export. A "supply 1x/2x/3x and pick by `viewport.scale`" path is possible but premature.

## Open Questions

- **Where does `Viewport` live?** Per-world (every `UiWorld` carries one) or per-app (a single global the world reads via `view`)? Per-app feels right because preset-switching should reflow *all* worlds consistently, but per-world is easier to test in isolation. Lean per-app.
- **Cover-fit vs. exact-fit between design and viewport.** Royalblush uses cover (`Math.max(viewW/designW, viewH/designH)`) so portrait designs always fill the phone width; this can cut off vertical content. For Rae's "design *is* the viewport" plan (393×852 ≡ design), `scale == 1` and the question is moot — but the moment someone authors at 1080×2280 and ships to a 393×852 phone, we have to pick. Defer until that case exists.
- **Component-name collision: `SafeArea` is both a component type and an authored entity name in royalblush scenes.** That's a convention, not enforced. We should document it as a convention but not require the entity's name to be literally `"SafeArea"`.
- **Live VM (bytecode) target.** This plan is written assuming the Compiled (C backend) target, since that's where the example runs today. Live target needs a parallel implementation of render-to-texture; raylib supports it identically, so the bindings carry over, but the VM bindings file (`vm_raylib.c`) needs the same handful of new natives.

## Recommended First Concrete Step

Phase 1, items 1–7, landing as one PR. The viewport + safe-area work is mechanical (well-defined per the doc and reference impl). The scene-file rewrite is small. The win is immediate: switching to iPhone aspect, with correct notch padding, becomes a single env var.

Phase 2 and 3 are deferred until there's a clear product reason (e.g. starting the workout-app demo, or needing zoom for live editing).
