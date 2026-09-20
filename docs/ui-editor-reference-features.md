# UI editor — feature inventory of the reference editor, and what Rae takes from it

**Status:** research note, 2026-09-21. The reference is an in-app scene editor from an
earlier, web-rendered project of ours (referred to here only as "the reference editor").
It overlays the running app, edits an ECS UI world whose scenes are JSON files of the
same `.raescene` family Rae's `lib/ui` reads, and its ideas are worth carrying over.
Nothing is copied — no code, no assets, no scene files; this note records the
FEATURES so the Rae UI editor (`examples/121_ui_editor`, docs/ui-editor-design.md)
can grow the same capabilities in its own way.

**The one rule that shapes every port:** the Rae editor's own UI is built from the same
material it edits — Rae UI components, an ECS world, authored `.raescene` files. The
reference draws its overlays and panels with an immediate-mode graphics API in
screen space; here every guide, outline and panel is a NODE in an authored chrome
scene (or a `Shape`/`Text` entity in the world), positioned by a system, drawn by the
stock render system. Where the reference needs a drawing primitive, Rae needs an
authorable component field instead (the dashed stroke below is the first example).

## 1. Mode and input

| Reference | Rae today | Port plan |
|---|---|---|
| Editor mode is a global boolean with listeners; two 22px screen-space buttons ("E" editor, "I" inspector) toggle it; a release build compiles it out. | The editor IS the app (a separate example), always on. | Not needed while the editor is its own program. When apps embed an inspector (106's debug menu is the seed), `App.editorMode` on the owning App, never a global. |
| A full-screen invisible input "blocker" node captures the pointer so gameplay never sees editor clicks; explicit pass-through to panels and dialogs. | The document is on its own layer; hit-testing walks reverse paint order, chrome first. | A `HitArea` node at the bottom of the chrome layer is the same idea in authored form, if the editor ever hosts a live app. |
| A "Game" tool (key `4`) lets the app receive input while the chrome stays visible. | — | Same, if/when the editor hosts a running app. |
| Editor settings persist across runs (which overlays are on, the tool, the sidebar tab, zoom) in local storage. | Env vars per run. | A `.rae/editor.json` next to the project, written by the app (the way 106 keeps `state.json`). |

## 2. Canvas overlays

All reference overlays are drawn in screen space in one overlay container, bottom to top:
input blocker, render preview, **safe-area/frame guides**, node outlines, hover outline,
selection marquee, axis-constraint guide, labels.

### 2a. Frame guides — DONE in Rae (this note's first port)

The reference draws three **dashed** rectangles with a hand-rolled dashed-rect routine
(each side a run of dash/gap segments; the sides **overshoot the corners** by a few
pixels so the corners read as crop marks). No labels. Each has a toolbar toggle and a
persisted default:

| Guide | Rect | Default | Dash / gap | Width | Colour | Alpha | Corner overshoot |
|---|---|---|---|---|---|---|---|
| Safe area | the `SafeArea` root's computed rect shrunk by padding + the applied insets + extra; fallback = the device's safe insets in design units | **on** | 10 / 6 | 2 | mint `#7fffd4` | 0.8 | 24 |
| Visible / simulated area | the simulated device viewport in design units | off | 8 / 6 | 4 | warm yellow `#ffd36a` | 0.7 | 30 |
| Full design area | `(0, 0, designW, designH)` | off | 8 / 6 | 3 | sky blue `#8fd3ff` | 0.6 | 19 |

Safe insets come from the platform (`env(safe-area-inset-*)`) or from the active device
preset; a `SafeAreaSystem` writes them onto every entity carrying a `SafeArea
{ enabled, apply: {l,t,r,b}, extra }` component — the same component and system shape
Rae's `lib/ui` already has.

A second, **solid** guide set lives inside the app root (so it scales with the design)
and can be left on in the running app: full design (cyan-blue), visible region
(amber), safe region (green), all width 2, alpha 0.8; plus, in screen space, the
simulated viewport edge (white) and the scaled design box (magenta) so letterbox gaps
are visible.

**Rae:** `examples/121_ui_editor/assets/scenes/chrome/Guides.raescene` — an authored
overlay scene on a `guides` layer between the document and the chrome: `FrameGuide`
(the page column — the design area the document is laid out in) and `SafeAreaGuide`
(the column inset by the device frame's insets), each a plain `Shape` node with the
new **dashed stroke** (`strokeDash` / `strokeGap` on `Shape`, painted by
`lib/ui/renderSystem/RenderPaintBox.rae` as rect segments along the four edges — pure
Rae, no C), plus a small `Text` label each. `guideSystem/GuideSystem.rae` writes their
Rects from the document's frame after every load; `G` toggles them;
`RAE_UI_EDITOR_GUIDES=0` starts hidden. Colours and rhythm follow the reference (sky
blue for the frame, mint for the safe area; the dash lengths are doubled since Rae's
design units are 3 per point for phone scenes). The safe-area guide shows only when the
frame has insets. Not ported yet: the corner overshoot (crop-mark corners — would be a
third `Shape` field), and the "visible area" guide, which only means something once
the editor simulates a device viewport smaller than the design.

### 2b. Node outlines, hover, marquee — partly in Rae

| Outline | Reference style | Rae |
|---|---|---|
| Primary selection | solid magenta-pink `#ff4fd8`, width 2.5, α 0.9 (`#ff7878` for runtime-only entities) | solid `#60c8ff`, width 7 — a code-created `Shape` entity the inspector moves. |
| Additional selections (multi-select) | solid cyan `#4fe4ff`, width 1.6, α 0.7 | no multi-select yet |
| "All outlines" toggle | every entity, cyan width 1, α 0.35 | — |
| "Button outlines" toggle | every entity with a button / `OnClick` / pointer events, orange `#ffa640`, width 2.2, α 0.9 | — |
| Hover | topmost hit under the cursor, `#28f0ff`, width 2, α 0.6 | `#ff9640`, width 3 |
| Marquee (drag-select) | `#28f0ff`, width 2, α 0.7, in design space | — |
| Axis-constraint guide | while moving with Shift/X/Y: a line from the drag origin along the locked axis, pink-red X `#ff9aa9` / light-blue Y `#9cc9ff`, with an "X axis"/"Y axis" label | — (no move yet) |
| "Hidden" label | under a selected node whose alpha is 0 (the editor bumps it to 0.5 so it can be seen) | — |
| Entity name labels toggle | white 10px `<id> <name|type>` at each rect's top-left | — |

Port note: the two Rae outlines should become authored nodes in `Guides.raescene` too
(`SelectionOutline`, `HoverOutline`), so the whole overlay set is one scene and the
inspector system only positions them — the same move the guides made.

### 2c. Render preview

With "Render" off, sprites/text/shapes are hidden and replaced by filled placeholder
rects (α 0.5) coloured by kind with a brightness ramp by draw order (greens for
sprites, blues for text, purples for shapes, greys for containers), so structure is
readable without artwork. Works with the editor closed ("preview only").
**Rae:** a render-system flag on the world (`world.renderPreview`) is enough; the
paint pass already switches per component kind.

## 3. Panels

The reference's panels are screen-space, unscaled, draggable, with per-tab persistence.

- **Left sidebar** — tabs ("Editor", "Hierarchy"), a scrollable stack of panels, an
  optional docked "create" toolbar (Create button / image / text, inserted after the
  selection and auto-selected).
- **Hierarchy** — header "Hierarchy (N)"; tree rows 24px with caret collapse, 12px
  indent per depth, label `name · Kind · file · "text preview"`; runtime-only entities
  prefixed and coloured red; selected row tinted; click selects, Shift/Ctrl/Cmd-click
  adds, Up/Down move the selection when focused; a filter prompt; two modes, "running"
  (the live world) and "raw scene data" (lists every scene file, opens one into a
  separate preview world — nested instances expand with their file name).
  **Rae:** the `T` tree panel is the seed (rows, indent, marker, click-select); missing:
  collapse carets, multi-select, the filter, kind/file columns, raw mode (Rae's project
  panel + bare-scene open already cover "list the files, open one").
- **Inspector** (right, 320px) — header toggles Active / Editor-visible / Editor-locked
  (applied to the selection and descendants); live pointer + hover readouts; a section
  per component: Rect X/Y/W/H (X/Y disabled with a note when a parent layout drives
  position), Padding, Size mode Fixed/Hug/Fill, Constraints, Align, Offset, TransformFx,
  Sprite (texture, mode, size), Text (content, style enum, "edit text styles"), Shape
  (kind, fill/stroke hex + swatch → colour picker, radius), OnClick action id, SafeArea
  (enabled + per-side on/off + read-only effective insets), Computed rect (read-only);
  a component manager (filter, add/remove with dependency locks, "addable" list,
  click-to-copy a component's JSON); multi-selection shows shared values and "—" for
  mixed, edits apply to all. Field widgets edit through a prompt (no scrubbing).
  **Rae:** the inspector panel shows components read-only. The editable form is the
  big next step and should be data-driven from compile-time field reflection over the
  component structs, not hand-written per component.
- **Toolbar** — a vertical strip docked to the inspector: Pointer / Select / Move /
  Game tools, Outlines / Buttons / Safe / Sim / System / Render / Names / MEM /
  vision-log toggles, Back, Fullscreen, zoom − / 100% / +.
- **Dialogs** — text styles, colour palettes, colour picker (top-right, own wheel and
  pointer routing).
- **Device simulator** — "reset to actual window", "safe area on/off", "guides in game
  on/off", then one button per preset: design portrait (no insets), a dozen phones
  (iPhone 15 Pro 393×852 with 59/34 insets … Android small 360×640), all portrait.
  Simulation centres a `w×h` viewport in the window and substitutes the preset's safe
  insets. **Rae:** `lib/ui/DevicePresets` already holds the same presets; the editor
  applies one automatically for phone-authored scenes (`unitScale`) and via
  `RAE_UI_EDITOR_DEVICE`; a panel with one row per preset is straightforward.

## 4. Selection

- Three tools: **Pointer** (`1`: click selects, drag pans), **Select** (`2`: drag = marquee,
  fully-contained rects), **Move** (`3`: drag moves the selection); `4` toggles Game.
- Hit test in design space sorted by draw index desc, depth desc, area asc; respects
  editor-visible / editor-locked flags.
- **Click-through cycling**: clicking again within 6px cycles to the next candidate
  underneath — the single best idea here for dense UIs.
- Shift/Ctrl/Cmd adds, Alt removes (click and marquee); click empty or `Esc` clears.
- A selection store with change listeners; hit lists logged verbosely.
**Rae:** click-select and arrow-key tree walking exist; add cycling, modifiers,
multi-select and the marquee next.

## 5. Editing operations

| Operation | Reference | Rae plan |
|---|---|---|
| Move by drag | writes `Rect.x/y`, or `Offset` when a parent layout drives position; multi-select keeps relative offsets; Shift auto-constrains to an axis, X/Y keys constrain manually | same rule — `Offset` is Rae's layout-input translation |
| Numeric edit | every component field in the inspector | reflection-driven inspector |
| Create nodes | button / image / text archetypes | archetype scenes (`chrome/*Row.raescene` are already this shape) |
| Add / remove components | with dependency checks and a confirm | ECS component tables make this direct |
| Visibility / lock | `Active`, `EditorVisible`, `EditorLocked` (the last two are editor-only components) | `Active` exists; the two editor flags as components on the document entities |
| Nudge with arrows, snapping, alignment tools, z-order, duplicate/delete, undo/redo | **none** (snapping is a design note only; no history stack) | Rae should do undo first: every edit is a component write, so a command log of (entity, component, before, after) is cheap |

Every edit bumps a world revision; panels poll it each frame to refresh — the same
generation-counter idea Rae's component tables already carry.

## 6. Data model and persistence

- World: numeric entity ids, one table per component (`rect`, `size`, `layout`,
  `padding`, `safeArea`, `align`, `offset`, `transform`, `sprite`, `text`, `shape`,
  `button`, `onClick`, `sceneInstance`, `active`, `parent`, `children`, …; runtime-only
  `computedRect`, `worldTransform`, `safeInsets`, `renderOrder`). `Rect` is authored
  top-left in parent-local design units and is the source of truth; the renderer
  mirrors it. This is Rae's `UiWorld` almost table for table.
- Scene file: `{ type: "Scene", version: 2, root, nodes: { id: { Children, <one key per
  component> } } }` with nested `SceneInstance { sceneId, params }` expanded at load
  with per-instance `overrides` (`nodeId/component/field/value`) and size `variant`s.
  Rae's format is the same lineage (plus `import`, `assets`, `unitScale`).
- **Save**: the inspector's "Save file" serializes the subtree under the selected
  scene root and POSTs it to a dev-server endpoint that writes the `.raescene`; also
  copies the JSON to the clipboard; "Load file" re-fetches and re-deserializes in place.
  No file watcher — reload is manual. **Rae:** the editor already watches and reloads
  on disk change; writing back is `Sys.writeFile` of a serialized subtree (a
  serializer is the missing lib piece — the deserializer is reflection-driven, so the
  serializer should be too).
- Design space is a fixed 1080×2280 portrait frame; "wide" mode fits the full design
  when no device is simulated.

## 7. Camera

- Zoom around the cursor on wheel (step 0.01 per notch; ×2 with Shift/Ctrl/Cmd, ×0.5
  with Alt), clamped 0.25–5; toolbar ± about the screen centre; "100%" resets.
- Pan: middle-drag always; left-drag in the Pointer tool; Alt/Cmd-drag in any tool;
  6px drag threshold; smoothed by a per-frame lerp (0.35).
- A design-space pointer readout inverts camera + layout transforms.
**Rae:** the canvas has one design-resolution transform; a camera is a scale + offset
on the document layer's root (`TransformFx` scale is not honoured by the renderer yet
— that is the lib gap to close first, and it unlocks the "fit a small document into
the chrome space by scaling" option the design-space rule currently avoids).

## 8. Keyboard

`Esc` deselect · `1`/`2`/`3` Pointer/Select/Move · `4` Game · hold `X`/`Y` axis
constraint · `Shift` auto-axis while moving · Shift/Ctrl/Cmd-click add · Alt-click
remove · Up/Down in the hierarchy move the selection (Shift additive).
Rae today: `H` chrome, `D` diagnostics, `T` tree, `P` project, `G` guides, `O` open,
arrows walk the tree, `Esc` clears.

## 9. Order of porting (suggested)

1. **Guides** — done (this note).
2. Authored selection/hover outlines in the same guides scene; hover colour per the
   reference; "all outlines" and "button outlines" toggles (a system that positions one
   authored outline node per entity of a query — the ListView row-pool pattern).
3. Click-through cycling, modifiers, multi-select, marquee.
4. Device simulator panel (one authored row per `lib/ui/DevicePresets` entry) and the
   "visible area" guide.
5. Move tool with the `Rect`/`Offset` rule, then the reflection-driven editable
   inspector, then a serializer + "Save" — with an undo log from the first write.
6. Camera (needs renderer scale), render preview, name labels.
