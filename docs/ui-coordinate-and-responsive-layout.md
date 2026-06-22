# Rae UI — Frames, Coordinates & Responsive Layout (final architecture)

Status: **architectural proposal, not implemented.** This is the authoritative
design; it supersedes and absorbs `docs/ui-viewport-and-safe-area-plan.md` and
`examples/98_mobile_ui/docs/adaptive-layout.md` (retire both into this once
accepted).

Combines: **Figma-style named frames + variants**, **Royal Blush-style
high-resolution virtual coordinates**, one shared entity/component model,
phone/tablet/desktop/web/games, **no game mode**, and **no single scaling rule
forced to solve every layout**.

### Grounding (verified against current code, prior inspection)
- Components present (`lib/ui/components.rae`): `Rect`, `Size`/`SizeAxis`
  (Fixed/Hug/Fill+min/max), `Layout` (None/Horizontal/Vertical/Grid/Stack),
  `Padding`, `Margin`, `Offset`, `Align` (per-axis), `Constraints` (min/max
  W/H+has-flags), `OverflowPolicy`, `SafeArea`(`enabled`,`apply:Insets`,
  `extra:Insets`)/`SafeInsets`, `Active`, `TransformFx` (scale/rot/pivot/anchor),
  `LayerRoot`/`LayerRef`, `PageRoot`, `Sprite.scaleMode`(Fit/Fill/Stretch),
  derived `ComputedRect`/`WorldTransform`/`MeasuredSize`.
- Hierarchy: `Children{ids:List(EntityId)}` on parent = **authoritative order**;
  `Parent{parent}` = **derived** upward cache (`components.rae:531-580`).
- Loader is already a dynamic string switch: `applyComponentByName(world,
  entity, name, doc, val)` deserializes one component blob onto one entity
  (`registry.rae:674+`). JSON layer iterates arbitrary keys, looks up fields,
  handles present/absent (`json.rae`, `scene_loader.rae`). `JsonDoc` is a
  resident flat pool.
- `componentGet` returns **typed structs** (no `RaeAny` for components);
  reparent = list ops on `Children`/`Parent`.
- Coordinate reality today: layout extent = disconnected globals
  `screenWidth=600/screenHeight=1300` (`layout.rae:29-30`); `setLayoutScreenSize`
  is a no-op (cross-module global write); transform is pure translation; input
  compares raw `getMouseX/Y` to world rects; no `FLAG_WINDOW_HIGHDPI`, no DPR.
- Live VM / C-backend limits that constrain the data model: recursive
  `List(Self)` structs segfault compiled → flat pools (`json.rae:1-3`); no
  `StringMap` inside structs on Live VM (`registry.rae:176`); `mod` struct-field
  `List` mutation doesn't survive return on Live (`json.rae:283-288`);
  `List(struct)` by-index corrupts compiled → parallel arrays /
  `rae_ext_rae_buf_get` (`registry.rae:602-604`); no first-class function refs →
  dispatch stays an if/else switch.

---

## The core coordinate decision (read first)

A named-frame model collides head-on with one question: **if the phone frame is
authored at 1080×2280 and the desktop frame at 2560×1440, does `200` mean the
same size in both?** There is no way to dodge it, so decide it explicitly.

You cannot have **(i)** one shared unit, **(ii)** phone frame = 1080 wide, and
**(iii)** desktop frame = 2560 wide, simultaneously. Under one shared unit the
ratio of frame widths is locked to the ratio of the devices' logical widths
(× the unit). Phone ≈ 360 pt, a 1080p desktop ≈ 1920 pt → 5.33×. So:

| Model | phone frame | desktop frame | `200` same size across frames? | clean integers? |
|---|---|---|---|---|
| **A. One unit, phone=1080** *(recommended)* | 1080×2280 du | **5760×3240 du** (1080p) / 7680×4320 (1440p) | **yes** | yes |
| B. One unit, desktop=2560 | 480×1014 du | 2560×1440 du | yes | yes, but phone is low-res/tiny |
| C. Per-frame unit | 1080×2280 du | 2560×1440 du | **no** (button changes size on copy) | yes |

**Recommendation: A — one shared design unit; frames are device-sized
rectangles in that unit.** This is exactly **Figma's** model (one canvas unit;
an iPhone frame is 393×852 and a Desktop frame is 1440×1024 in the *same* px —
a 200-px button is identical in both), lifted to high resolution. The honest
consequence: **the desktop frame is authored at ~5760×3240 du, not 2560×1440.**
Bigger numbers, but clean integers, and a shared button/sidebar/safe-inset has
one consistent physical size everywhere. Option C (per-frame unit) gives you the
literal 2560×1440 desktop numbers but **breaks cross-frame size identity** — the
Figma virtue you're asking to copy. (This stays listed as an open decision in
§Final-11, but the rest of this doc assumes A.)

---

## 1. Coordinate semantics (Q2)

Three strictly separated layers; designers and `.raescene` only touch layer 1.

| Layer | Unit | Set by |
|---|---|---|
| **Design units** `du` | authored, clean integers | the `.raescene` |
| **Logical points** `pt` | the OS window | the platform |
| **Framebuffer px** | DPR / Retina | the display |

One fixed global constant: **`unitScale` = design units per logical point,
default `3`** (1 du = one physical pixel of a 3× reference phone — the Royal
Blush intuition made exact). Derived, never authored:
`renderScale = dpr / unitScale` (du→px, rasterization only);
`area_du = windowPt × unitScale` (the real window expressed in du).

- **`200` has the same visual meaning in every frame** — one unit, period.
- **Frames merely use different available areas** (and carry different authoring
  *reference* rectangles); they do not rescale the unit.
- **Shared components keep consistent sizes** automatically (same unit).
- **Copying a button phone→desktop** keeps its du size → identical physical
  size; you only change layout *relationships* (parent, Fill vs Fixed), not the
  number.
- **Framebuffer/DPR stays separate**: only `renderScale` touches pixels.
- **UHD/5K affect only sharpness or available workspace, never authored values**
  (worked below).

A frame's `viewport` is a **reference rectangle** (editor canvas + headless
fallback + basis for local `ScaleToFit`), **not a runtime canvas**: the frame's
root lays out into the *actual* `area_du`, with Fill/Hug/Constraints absorbing
the difference between the reference and the real window.

### Worked examples (unitScale = 3)

| Display | window pt | DPR | framebuffer px | `area_du` | renderScale |
|---|---|---|---|---|---|
| iPhone 15 Pro | 393×852 | 3 | 1179×2556 | 1179×2556 | 1.000 |
| 1080p desktop | 1920×1080 | 1 | 1920×1080 | 5760×3240 | 0.333 |
| UHD 3840×2160 @200% | 1920×1080 | 2 | 3840×2160 | 5760×3240 | 0.667 |
| UHD 3840×2160 @100% | 3840×2160 | 1 | 3840×2160 | 11520×6480 | 0.333 |
| Apple 5K 5120×2880 (Retina) | 2560×1440 | 2 | 5120×2880 | 7680×4320 | 0.667 |

The decisive rows: **1080p-at-100% and UHD-at-200% produce an identical
5760×3240 du layout** — only `renderScale` differs (0.333 vs 0.667). DPR is
fully divorced from scene coordinates: same layout, sharper pixels. **UHD-at-100%
vs UHD-at-200% differ in *workspace*** (11520 vs 5760 du wide) because the user
chose more logical space, not more pixels — also correctly captured. 5K Retina
lays out like a 1440p desktop (7680×4320 du) and rasterizes at 5120×2880.

---

## 2. Frame architecture (Q1)

**Recommendation: named frames are primary; per-node overrides are secondary
sugar.** Frames own *structure and composition* (root, hierarchy, visibility,
broad overrides); inline per-node overrides handle *small local diffs* and
desugar into frame overrides at load, so there is exactly **one** resolution
path.

Why frames over scattered per-node `Responsive` (last draft's model): a
phone→desktop change is a *whole-composition* change (stack→split, bar→rail,
hidden→visible, tools appear). Grouping all of a frame's diffs under the frame
(a) matches the Figma mental model, (b) lets the editor switch frames and see a
coherent composition, (c) keeps hierarchy recomposition in one readable place.
Per-node-only overrides scatter that across the tree.

Why frames are **not** separate scenes / duplicated entities: entities are
shared and keep identity (§4); a frame is a *presentation* of the shared graph,
stored as data and overlaid at runtime — no duplication, no whole-scene copies.

A **frame** declares: `viewport` (reference rectangle, §1), `when` (activation,
§3), `root` (which entity is the active root), and `nodes` (a map of nodeId →
override bag: `Parent`, `order`, `Active`, and any component overrides). It is
mechanically the same overlay used everywhere — replay `applyComponentByName`
with the override bag — just grouped by frame.

---

## 3. Frame selection (Q3)

**Conditions are in logical points** (device-independent; not du which is 3×,
not framebuffer px which is DPR-dependent). The brief's worry about numeric
breakpoints misbehaving under authoring scale is resolved by breaking on pt.

```json
"PhonePortrait": { "when": { "maxWidthPt": 599 } },
"TabletPortrait":{ "when": { "minWidthPt": 600, "maxWidthPt": 1023, "orientation": "portrait" } },
"Desktop":       { "when": { "minWidthPt": 1024 }, "default": true }
```

Inputs — **core now:** `minWidthPt`/`maxWidthPt`, `minHeightPt`/`maxHeightPt`,
`aspect` (min/max h:w), `orientation`. **Now, cheap:** `inputType`
(touch|pointer), `windowMode` (named class). **Deferred:** true physical size.
Runtime **state** (e.g. `canvasOpen`) is *not* a frame selector — it's a
within-frame condition (§4), so frame choice stays a pure function of the
window.

Determinism: evaluate every frame's `when`; among matches pick the **most
specific** (most bounded predicate), ties broken by **declared order**; if none
match, the `default` frame. **Editor force-select** overrides everything
(`forcedFrameId`), and the simulator sets it per preset.

---

## 4. Shared identity & state (Q4)

Entities are created **once**, keyed by `nodeId` (`world.nodeIds`), and are
**never destroyed or recreated** on a frame switch. Therefore `ScrollState`,
`AnimState`, `HoverScale.current`, selection, and entity IDs all persist across
phone↔desktop automatically.

Composition of base + frame override:
- The base `nodes` block holds each entity's **authored** components (the
  base bag). On load, snapshot it (keep the `JsonDoc` resident — already the
  case — so the base bag is always re-derivable).
- On frame change: for every entity, **reset to base, then overlay the active
  frame's override bag** by replaying `applyComponentByName` per component in
  the bag. Overlay writes *only* the components named in the bag.
- **Restore is trivial and exact:** runtime-state components (`ScrollState`,
  `AnimState`, …) are never present in override bags, so the reset+overlay pass
  never touches them. A component the old frame overrode but the new frame
  doesn't returns to its base value because we reset-to-base first. One small
  new helper, `removeComponentByName`, handles a component that exists *only*
  in an override (added, not overridden) when its frame deactivates.

---

## 5. Hierarchy derivation (Q5)

Extend the existing authoritative(`Children`)/derived(`Parent`) split by one
level — **authored vs active**:

- **Authored** (immutable after load): the base parent/child as written (kept in
  `AuthoredChildren`, or read from the resident `JsonDoc`). Never mutated.
- **Active** (derived on frame/state change): rebuild `world.childrens` + the
  `Parent` cache:
  1. `active = clone(authored)` (or clone of the frame's own `root` subtree).
  2. Apply the active frame's directives in **declared order**: `Parent: "X"`
     detaches the node from its current active parent and appends under `X`
     (insertion index = optional `order`, else append). Last writer wins.
  3. **Cycle detection:** a node may not become its own ancestor → reject that
     directive with a diagnostic, keep the prior parent. Detached-and-unattached
     → fall back to authored parent.
- Layout/transform/input read the **active** tables unchanged — nothing below
  the derivation layer knows frames exist.

Supports alternate parent, alternate child order, **frame-only entities** (only
wired into the frames that use them; inactive elsewhere via no active parent +
`Active:false`), shared entities, cycle detection, **stable z-order** (z from
`LayerRoot.order` + child order, both deterministic), and **hot reload**
(reload doc → re-derive; `nodeId` identity preserves runtime state).

---

## 6. Games in the same system (Q6)

No game mode. A board is an ordinary fixed-size subtree inside any frame:

```json
"GameBoard": {
  "Rect": { "w": 1080, "h": 1600 },
  "Aspect": { "ratio": 0.675 },
  "ScaleToFit": { "mode": "Contain" },
  "Layout": { "kind": "None" }
}
```

- `Layout.kind = None` (**exists**) → children placed absolutely in stable du
  art-direction coordinates with `Rect`/`Offset`/`Align` (**all exist**).
- `Aspect { ratio }` (**new, minimal**) → the container sizes to that aspect
  inside its responsive slot (letterbox/pillarbox internally).
- `ScaleToFit { mode: Contain|Cover }` (**new, minimal**) → the board's whole du
  coordinate space scales as one unit to fit/fill its slot. This is the **only**
  place a uniform scale exists, and it is **local and opt-in** — the
  game-engine "fixed virtual canvas + contain/cover" available anywhere without
  a global mode.
- Already-present support: `Constraints`, `Align`, `Offset`, `LayerRoot`/
  `LayerRef` (HUD layers), `SafeArea` (HUD insets), `Sprite.scaleMode` (extend
  enum with `Cover`/`Contain` for media). Overlays/dialogs = a top `LayerRoot`
  + `Stack`.

So `GameBoard` stays a pixel-stable composition (fixed du, `Aspect`+`ScaleToFit`)
while `LibraryPanel`/`Navigation`/`CanvasArea`/dialogs around it reflow per
frame — one scene, no switch.

---

## 7. Figma comparison (Q7)

| Figma | Rae equivalent | Copy? |
|---|---|---|
| Frames (device rectangles, one canvas unit) | Named frames in one shared du unit | **Yes** — core idea |
| Auto Layout (dir/gap/padding/hug/fill) | `Layout` + `Size` (already present) | **Yes** — already have it |
| Constraints (pin/stretch/center/scale) | `Align`+`Constraints`+`Offset` | **Yes** |
| Reusable components | Shared entities by `nodeId` | **Yes** |
| Variants / alternate compositions | Frames + per-node overrides | **Yes** (data overlay, not a property graph) |
| Component *properties*/boolean/instance-swap | — | **No** — too dynamic for the ECS; use data overlays |
| Per-frame independent unit | — | **No** — Figma itself shares one unit; we match that (one du) |
| Vector/boolean ops, infinite nesting | — | **No** — out of scope for a runtime |

Conceptually copy frames + Auto Layout + constraints + shared components +
variants. Drop Figma's live property graph and any per-frame unit.

## 8. Game-engine comparison (Q8)

The classic "fixed virtual canvas + contain/cover + stable animation coords +
direct framebuffer + DPR-independent design coords" is preserved **locally**:
- Fixed virtual canvas = a `Layout.kind=None` subtree in fixed du.
- contain/cover = `ScaleToFit{Contain|Cover}` on that subtree.
- stable animation coords = the subtree's du never change with the window.
- direct framebuffer + DPR-independent = the global du↔px split (`renderScale`).
The difference from a typical engine: this canvas is **nested inside a
responsive frame**, not the whole screen — so the same primitives give you a
fullscreen game *or* a board beside a sidebar, with no global mode.

---

## 9. Proposed `.raescene` schema (Q9)

```json
{
  "scene": {
    "type": "Scene", "version": 3,
    "unitScale": 3,
    "frames": {
      "PhonePortrait": {
        "viewport": { "w": 1080, "h": 2280 },
        "when": { "maxWidthPt": 599 },
        "root": "SafeRoot",
        "nodes": {
          "SafeRoot":    { "Children": ["PhoneStack"] },
          "PhoneStack":  { "Layout": { "kind": "Stack" }, "Children": ["LibraryPanel", "CanvasArea", "Navigation"] },
          "LibraryPanel":{ "Parent": "PhoneStack", "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fill"} } },
          "CanvasArea":  { "Parent": "PhoneStack", "Active": { "value": false } },
          "Navigation":  { "Parent": "PhoneStack", "order": 99,
                           "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fixed"} }, "Rect": { "h": 240 },
                           "Layout": { "kind": "Horizontal", "alignMain": "SpaceBetween" } }
        }
      },
      "Desktop": {
        "viewport": { "w": 5760, "h": 3240 },
        "when": { "minWidthPt": 1024 }, "default": true,
        "nodes": {
          "SafeRoot":    { "Children": ["DesktopSplit"] },
          "DesktopSplit":{ "Layout": { "kind": "Horizontal" }, "Children": ["Navigation", "LibraryPanel", "CanvasArea"] },
          "Navigation":  { "Parent": "DesktopSplit", "order": 0,
                           "Size": { "w": {"mode":"Fixed"}, "h": {"mode":"Fill"} }, "Rect": { "w": 660 },
                           "Layout": { "kind": "Vertical", "alignMain": "Start" } },
          "LibraryPanel":{ "Parent": "DesktopSplit", "order": 1,
                           "Size": { "w": {"mode":"Fixed"}, "h": {"mode":"Fill"} }, "Rect": { "w": 1080 } },
          "CanvasArea":  { "Parent": "DesktopSplit", "order": 2, "Active": { "value": true },
                           "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fill"} } },
          "DesktopTools":{ "Parent": "Navigation", "Active": { "value": true } }
        }
      }
    },
    "states": { "canvasOpen": { "nodes": { "CanvasArea": { "Active": { "value": true } } } } },
    "nodes": {
      "SafeRoot":   { "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fill"} },
                      "SafeArea": { "enabled": true, "apply": { "l":1,"t":1,"r":1,"b":1 } } },
      "DesktopSplit": { "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fill"} } },
      "PhoneStack":   { "Size": { "w": {"mode":"Fill"}, "h": {"mode":"Fill"} } },
      "Navigation": { "Sprite": { "textureKey": "ui.navbg" } },
      "LibraryPanel": { "Sprite": { "textureKey": "ui.panel" }, "ScrollState": { "y": 0 } },
      "CanvasArea": { "Children": ["GameBoard"] },
      "GameBoard":  { "Rect": { "w": 1080, "h": 1600 }, "Aspect": { "ratio": 0.675 },
                      "ScaleToFit": { "mode": "Contain" }, "Layout": { "kind": "None" },
                      "Children": ["BoardBg", "CardSlotA", "CardSlotB"] },
      "BoardBg":  { "Rect": { "x": 0, "y": 0, "w": 1080, "h": 1600 }, "Sprite": { "textureKey": "game.board" } },
      "CardSlotA":{ "Rect": { "x": 120, "y": 900, "w": 360, "h": 504 } },
      "CardSlotB":{ "Rect": { "x": 600, "y": 900, "w": 360, "h": 504 } },
      "DesktopTools": { "Layout": { "kind": "Vertical" }, "Sprite": { "textureKey": "ui.tools" } }
    }
  }
}
```

Top-level `nodes` = shared entities + base components (the single source of
identity and of state-bearing components like `ScrollState`). `frames` =
per-frame composition (root + reparenting + visibility + overrides). `states` =
within-frame runtime overlays. `GameBoard` is a fixed-aspect contained subtree
of `CanvasArea` in both frames — stable composition, responsive surroundings.

---

## 10. Runtime pipeline (Q10)

```
per frame:
  1. read window pt + DPR
  2. area_du = pt × unitScale ; renderScale = dpr / unitScale
  3. select active frame (pt-based `when`, or forcedFrameId)        ── on change only
  4. derive active components (reset-to-base + overlay frame bag)   ── on frame/state change
  5. evaluate runtime state conditions (canvasOpen…) → overlay      ── on state change
  6. derive active hierarchy (authored + frame directives, cycle-checked) ── on frame change
  7. layout over active tree, root extent = area_du                ── every frame, dirty-gated
  8. apply local Aspect/ScaleToFit/camera containers               ── every frame (with layout)
  9. transform (du; local scale only inside ScaleToFit subtrees)   ── every frame, dirty-gated
 10. safeArea fold (insets in du) ; render: du×renderScale, pixel-snap, HIGHDPI ── every frame
 11. input: pointer pt×unitScale → du; invert local ScaleToFit transforms      ── every frame
```

**Recompute only on frame/state change (steps 3–6):** frame selection, active
component overlay, active hierarchy, visibility. These are gated on
window-crossing-a-breakpoint or a state flag flip — not per frame.
**Every frame (7–11):** layout/transform/render/input, already generation/dirty
gated so a static scene does no layout work. This keeps the expensive
derivation rare and the steady state cheap.

---

## 11. Compiler implications (Q11)

**No compiler changes, no new syntax. Scene-loader + new ECS components +
hierarchy-derivation infrastructure only.** Enablers already exist: dynamic
`applyComponentByName`, JSON nested-map/optional-field decoding, typed
`componentGet`, list-based hierarchy.

Respect the verified limits (all already worked around elsewhere):
- Keep `frames`/`states`/override data in the **resident flat-pool `JsonDoc`**,
  not recursive structs.
- **No `StringMap` in structs on Live VM** → frame/condition tables as parallel
  lists or read straight from the `JsonDoc`.
- **`mod` struct-field `List` mutation doesn't survive return on Live** → when
  rebuilding active child lists, plumb `mod List` directly (mirror
  `appendEntityList`).
- **`List(struct)` by-index corrupts compiled** → parallel arrays /
  `rae_ext_rae_buf_get` for any new list-of-struct.
- **No function refs** → conditions are data evaluated by one interpreter;
  dispatch stays if/else; add `removeComponentByName` beside
  `applyComponentByName`.
- Replace the `600×1300` globals + `setLayoutScreenSize` no-op by **passing the
  layout extent (a `Viewport`) into the layout root as a parameter** — the clean
  fix that also sidesteps the cross-module global-write limit.
- Live/Compiled parity holds (all existing patterns); derivation is change-gated.

Minimal **new components**: `Aspect{ratio}`, `ScaleToFit{mode}`; a `Viewport`
runtime struct; a `DevicePreset` table; a `FrameDef`/`Condition` data model
(parallel-list form). `Parent`/`order` in override bags are loader directives,
not stored components.

---

## 12. `examples/98_mobile_ui` demonstration (Q12)

One scene file (the §9 schema), showing: `PhonePortrait` + `Desktop` frames;
`LibraryPanel` fullscreen on phone → 1080 du (360 pt) sidebar on desktop;
`Navigation` bottom bar → 660 du (220 pt) left rail (reparent + layout flip);
`CanvasArea` hidden on phone until `canvasOpen` toggles it as a full-screen page
→ Fills beside the sidebar on desktop; `DesktopTools` frame-only entity visible
only on desktop; `SafeRoot` safe-area container; `GameBoard` fixed-aspect
contained media/board inside `CanvasArea` in both frames; same `nodeId`
identities throughout (scroll/anim survive switching); **simulator** sets
`forcedFrameId` per device preset and **live resize** re-runs frame selection
(steps 1–6) on `IsWindowResized`.

Worked: iPhone 15 Pro → `PhonePortrait`, area 1179×2556 du, `LibraryPanel` Fills,
`Navigation` 1179×240 du bottom bar. 1080p desktop → `Desktop`, area 5760×3240
du, `Navigation` 660 + `LibraryPanel` 1080 + `CanvasArea` 4020 du across;
`GameBoard` 1080×1600 du contained inside `CanvasArea`, scaled as one unit.

---

## Final answer

**1. Recommended architecture.** One scene = shared entities (top-level `nodes`,
the source of identity + state) + **named frames** (composition, reparenting,
visibility, overrides, each with a reference viewport and pt-based `when`) +
within-frame `states`. One shared design unit (du, `unitScale=3`). Runtime
selects a frame, resets-to-base and overlays the frame (and active states),
derives an active hierarchy non-destructively, then runs the existing
layout/transform/render in du with DPR applied only at rasterization. Games are
fixed-aspect `ScaleToFit` subtrees inside any frame.

**2. Better than one globally scaled `1080×2280` canvas.** A single scaled canvas
makes desktop a uniformly enlarged phone — it cannot turn a fullscreen panel
into a sidebar, reveal desktop-only tools, or reparent a bottom bar into a rail.
Frames change *composition*, not just scale.

**3. Better than pure logical-point responsive layout.** Pure points give one
fluid layout but no clean high-res integers and no first-class notion of
*distinct* phone vs desktop compositions; you'd rebuild structure with ad-hoc
conditionals. Frames make the alternate compositions explicit, editable, and
Figma-like, while still using Fill/Hug/Constraints *inside* each frame.

**4. Top-level `.raescene` schema.** `scene{ type, version:3, unitScale,
frames{<Name>{viewport, when, default?, root, nodes{<id>{Parent,order,Active,
<overrides>}}}}, states{<name>{nodes{...}}}, nodes{<id>{<base components>,
Children}} }`.

**5. Frame/override model.** Frames primary (structure); per-node `Responsive`
optional sugar desugaring to frame overrides. Overlay = reset-to-base + replay
`applyComponentByName` with the active bag; `removeComponentByName` for
override-only components. One resolution path.

**6. Hierarchy derivation.** Authored (immutable) → active (derived) `Children`/
`Parent`: clone authored, apply frame `Parent`/`order` directives in declared
order, cycle-checked, z-order from `LayerRoot.order`+child order. Reversible;
identity stable.

**7. Coordinate semantics.** Author in du (clean integers). `unitScale=3`
converts du↔pt; DPR converts pt↔px. `200 du` is one size everywhere; frames vary
*area*, not unit; `renderScale=dpr/unitScale` is the only pixel-facing scalar;
local `ScaleToFit` is the only uniform scale.

**8. Minimal new components.** `Aspect{ratio}`, `ScaleToFit{mode:Contain|Cover}`;
runtime `Viewport`; `DevicePreset` table; `FrameDef`/`Condition` data (parallel
lists). Extend `Sprite.scaleMode` with Cover/Contain. (`SafeArea`, `Active`,
`Constraints`, `Align`, `LayerRoot`, `Layout.kind=None` already exist.)

**9. Compiler vs UI-library.** UI-library + scene-loader + ECS data only; **no
language changes**. Add `removeComponentByName`; pass layout extent as a param
(kills the `600×1300` globals). Respect flat-pool/no-StringMap-in-struct/
explicit-`mod List`/parallel-array limits.

**10. Staged plan.**
- **S1 Coordinate spine:** `Viewport`+`unitScale`+DPR, `FLAG_WINDOW_HIGHDPI`,
  extent-as-param, pixel-snapped DPR render, pointer×unitScale, rescale the
  example into du. *(no frames yet)*
- **S2 Safe areas + device presets + simulator** (`forcedFrameId`).
- **S3 Frames (selection + per-frame component overlay, no reparent):** frame
  table, pt-based `when`, reset-to-base+overlay, `Active`/`Size`/`Layout`
  overrides, `removeComponentByName`.
- **S4 Active-hierarchy derivation:** authored vs active, reparent + `order`,
  cycle detection; the phone-stack↔desktop-split demo.
- **S5 Within-frame `states`** (phone canvas toggle) + frame-only entities.
- **S6 Art direction:** `Aspect`, `ScaleToFit`, `Sprite` cover/contain, HUD
  layers; the `GameBoard` sample.
- **S7 (deferred):** world-space camera, `inputType`/physical-size conditions,
  per-node `Responsive` sugar.

**11. Open decisions for Jonas.**
- **The unit decision (§"core coordinate decision"):** lock **A — one shared
  unit (`unitScale=3`), desktop frame authored ~5760×3240 du** (recommended,
  Figma-faithful, consistent sizes)? Or accept **C — per-frame unit** so desktop
  reads the literal 2560×1440 but a shared button changes size between frames?
- **Frame granularity:** are `PhonePortrait/TabletPortrait/TabletLandscape/
  Desktop` the right named set, or should frames be fully author-defined names
  with only `when` deciding activation? (Recommend author-defined names.)
- **Reparent scope:** may a frame reparent across `LayerRoot`/`PageRoot`
  boundaries? (Recommend no initially.)
- **Frame-only heavy subtrees:** always-instantiated + `Active` toggle, or lazy
  build to avoid constructing large desktop-only trees on phone?
- **`royalblush-rae` alignment:** should the game port share this du space
  (`unitScale=3`, 1080-wide phone reference) so its `1080×2280` art lines up 1:1?
- **Multiple simultaneously-matching frames:** most-specific-then-declared-order
  (recommended) vs explicit numeric priority.
