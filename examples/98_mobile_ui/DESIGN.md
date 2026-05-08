# 98_mobile_ui — Design

A scene-driven UI runtime for Rae, built for mobile-shaped layouts. The
target reference is the music player mockup in `references/music_player.webp`
and the two cropped screens in `screens/music_player_screen_*.png`.

The starting model is **RUICS**, the Royal Blush UI system documented at
`rae/docs/royal-blush-ruics-ecs-reference.md`. RUICS is an ECS-backed
scene graph with JSON `.raescene` files,
authored components, and a small set of layout/transform/render systems.
This design carries that model into Rae nearly as-is, with a few
deliberate adaptations called out below.

The user has flagged that *most of the work belongs in the stdlib* —
the example app itself should be small. So this doc plans both the new
`lib/ui/` modules and the per-screen scene files, but leans most of the
volume into the library.

## Goals (this round)

1. Render **music_player_screen_2.png** — the "Charcoal" album view —
   from a `.raescene` file, with no hand-laid-out raylib draw calls in
   the example.
2. Then render **music_player_screen_1.png** — the "Now Playing" view —
   reusing the shared bottom tab bar as a `SceneInstance`.
3. The example app (`examples/98_mobile_ui/main.rae`) is a thin shell:
   open a window, load the scene, run the system pipeline each frame.
4. Most code lives in `lib/ui/*.rae`. Future examples (workout app,
   etc.) should be able to consume the same library without changes.

## Non-goals (this round)

- Animations, smoke FX, wobble. Author them in components, but don't
  wire the runtime systems yet.
- Virtualised lists / `DataRequest`. The track list in screen 2 is
  short enough to render the whole list every frame; a real `ListView`
  + virtualisation comes later.
- Editor / inspector tooling. Stable `NodeId` is preserved, but no
  visual editor.
- Audio. The mini-player UI exists; the actual playback engine doesn't.

## Why scene-driven and ECS

Two questions worth answering up-front, since neither is obvious for a
language that already has structs and `raylib.drawRectangle`:

**Why ECS instead of a tree of structs?** A tree of structs locks you
into one shape per "widget kind" — `Button`, `Slider`, `ListItem` etc.
ECS lets a node *become* a button by gaining `OnClick` and `HitArea`,
without changing its struct type. The same node can drop those
components later and become something else. That's what makes the
authored format generic: a node's meaning is the *combination* of
components on it, not its class.

**Why a scene file format instead of building the tree in code?** The
moment two screens share a tab bar, you want it as a reusable unit
with a stable id. The moment a list has 20 items with the same shape,
you want one item template instantiated 20× with different bound data.
Both are awkward in code-only UI; both are natural with a scene file
plus `SceneInstance` and overrides.

## High-level architecture

Five layers, mirroring RUICS:

```
┌─────────────────────────────────────────────┐
│ 1. ECS storage                              │   lib/ui/ecs.rae
│    Entity = Int; ComponentTable(T); World   │
├─────────────────────────────────────────────┤
│ 2. Scene format + loader                    │   lib/ui/scene.rae
│    .raescene JSON → ECS components          │   lib/ui/scene_loader.rae
├─────────────────────────────────────────────┤
│ 3. Layout + transform                       │   lib/ui/layout.rae
│    measure → compute → world transform      │   lib/ui/transform.rae
├─────────────────────────────────────────────┤
│ 4. Render                                   │   lib/ui/render.rae
│    raylib painter (immediate-mode)          │
├─────────────────────────────────────────────┤
│ 5. Behaviours (text binding, input,         │   lib/ui/text_binding.rae
│    scene instance, list view, …)            │   lib/ui/input.rae
└─────────────────────────────────────────────┘
```

The example app (`main.rae`) owns the `World`, calls
`loadScene(...)` once at startup, and then per frame runs the system
pipeline. That's it — the rest is library + scene files.

## Adaptations vs. Royal Blush

A few places the Rae version deliberately differs:

| Royal Blush | Rae adaptation |
|---|---|
| Sparse-set component table keyed by entity id | Same shape, but as a Rae generic `ComponentTable(T)` (Rae's monomorphisation handles it). |
| `UiWorld` stores tables in a `Map<Type, ComponentTable>` | Rae has no untyped maps over types. World holds each table as a **named field**: `transforms: ComponentTable(TransformFx)`, `rects: ComponentTable(Rect)`, etc. The set of components is fixed at compile time. |
| Pixi render backend, retained-mode, with handle objects | Raylib is immediate-mode. We don't keep render handles. `RenderSystem` paints the scene directly each frame from `WorldTransform` + `Sprite`/`Text`/`Shape`. No `RenderSyncSystem` — that whole layer collapses. |
| TypeScript + dynamic JSON ⇄ object mapping | Rae has flat-struct JSON helpers (`rae_json_extract_int/float/string/bool`) but no generic JSON tree. We add a small JSON-tree parser to `lib/json.rae`, then drive component deserialisation off a per-component-name table. |
| Component registry discovered at runtime via reflection | Rae has no reflection. The registry is a plain `func registerCoreComponents(reg: mod ComponentRegistry)` that lists every component name + its deserialiser + applier. New components ⇒ one line in that function. |
| Generic `query(...tables)` taking varargs | Rae lacks variadic generics. Provide query helpers per shape (`queryWith2(world, &world.rects, &world.layouts)` etc.) — the music player uses ≤4 components per query. Easy to extend later. |

The architectural skeleton — authored components, scene instances,
overrides, two-phase layout, derived `ComputedRect` and `WorldTransform`,
text wrap by node width, active filtering — all lift over unchanged.

## Scene file format

Identical to RUICS at the JSON level, so existing tooling and the
reference doc apply directly:

```json
{
  "type": "Scene",
  "version": 2,
  "sceneId": "music-player-album",
  "root": "AlbumRoot",
  "nodes": {
    "AlbumRoot": {
      "Rect":     { "x": 0, "y": 0, "w": 600, "h": 1300 },
      "Size":     { "w": { "mode": "Fill" }, "h": { "mode": "Fill" } },
      "Layout":   { "type": "Vertical", "gap": 16 },
      "Padding":  { "l": 24, "t": 24, "r": 24, "b": 12 },
      "Children": ["TopBar", "AlbumHero", "ActionRow", "PlayShuffleRow",
                   "TrackList", "MiniPlayer", "TabBar"]
    },
    "TopBar": {
      "Layout":   { "type": "Horizontal", "gap": 12, "alignCross": "Center" },
      "Size":     { "w": { "mode": "Fill" }, "h": { "mode": "Fixed" } },
      "Rect":     { "x": 0, "y": 0, "w": 0, "h": 36 },
      "Children": ["BackButton", "TopBarSpacer", "SearchButton"]
    },
    ...
    "TabBar": {
      "SceneInstance": {
        "sceneId": "nav-tabs",
        "params": {
          "overrides": [
            { "nodeId": "TabLibrary", "component": "Active",
              "field": "value", "value": true }
          ]
        }
      }
    }
  }
}
```

Validation rules from RUICS apply verbatim: `type=="Scene"`,
`version==2`, `root` exists, every `Children[i]` exists.

### Why JSON and not a Rae-native form

A Rae-native scene file would let the compiler type-check component
data at parse time, which is real value. But:

- it forks from the reference doc the user pointed at,
- it can't be hot-reloaded without recompiling the whole module,
- it can't be patched by an editor without touching the AST,
- it kills the override mechanism (which is structural by design).

We pay the runtime parse cost and the per-component deserialiser
plumbing in exchange for keeping the scene model as data, not code.

### Adding a JSON tree parser

`lib/json.rae` (delivered in queue task #40) — recursive-descent JSON
parser with a flat-pool layout. Initially designed with a recursive
`JsonValue` containing `List(JsonValue)`, but the C backend currently
segfaults on that shape (Live works), so the parser stores all values
in three flat lists owned by a `JsonDoc`:

```rae
enum JsonKind { Null, Bool, Number, String, Array, Object }

type JsonField {
  key: String
  valueIdx: Int          # index into JsonDoc.values
}

type JsonValue {
  kind: JsonKind
  asBool: Bool
  asNumber: Float
  asString: String
  rangeStart: Int        # for Array: range in JsonDoc.children
  rangeLen: Int          # for Object: range in JsonDoc.fields
}

type JsonDoc {
  values: List(JsonValue)
  children: List(Int)    # array element value-indices, contiguously
  fields: List(JsonField) # object fields, contiguously
  rootIdx: Int
  ok: Bool
  errorPos: Int
}

func parseJson(source: String) ret JsonDoc
func jsonRoot(doc: view JsonDoc) ret JsonValue
func jsonField(doc: view JsonDoc, this: view JsonValue, key: String) ret Int  # -1 = missing
func jsonValueAt(doc: view JsonDoc, idx: Int) ret JsonValue
func jsonArrayLen(this: view JsonValue) ret Int
func jsonArrayAt(doc: view JsonDoc, this: view JsonValue, idx: Int) ret JsonValue
func jsonObjectLen(this: view JsonValue) ret Int
func jsonObjectKeyAt(doc: view JsonDoc, this: view JsonValue, idx: Int) ret String
func jsonObjectValueAt(doc: view JsonDoc, this: view JsonValue, idx: Int) ret JsonValue
func jsonInt(this: view JsonValue, fallback: Int) ret Int
func jsonFloat(this: view JsonValue, fallback: Float) ret Float
func jsonString(this: view JsonValue, fallback: String) ret String
func jsonBool(this: view JsonValue, fallback: Bool) ret Bool
```

Two Rae-specific gotchas hit during implementation, worth flagging
because they'll bite later phases:

1. **Live VM doesn't propagate `mod struct.listField`.** Mutating a
   list that's a *field* of a `mod` struct works inside the callee
   but is invisible to the caller after return. Workaround: pass
   each list as its own `mod List(...)` parameter. The parser does
   this through `parseValue(p, vals: ..., kids: ..., fields: ...)`
   instead of `parseValue(p, doc: doc)`.
2. **Compiled C backend can't `&` a function-call rvalue passed as
   `view T`.** Bind `jsonRoot(doc)` (or any `ret JsonValue`) to a
   local first before passing to a `view JsonValue` helper. The
   tests do this consistently.

Both are real Rae bugs that should land in the queue once #40 is
done, but neither blocks the parser shipping.

## Component inventory for the music player

The minimum component set needed to render both screens:

**Authored (persisted in `.raescene`):**

| Component | Fields | Used for |
|---|---|---|
| `Rect` | x, y, w, h | seed rectangle |
| `Size` | w: SizeAxis, h: SizeAxis (Fixed/Hug/Fill, min, max) | sizing policy |
| `Layout` | type (None/Horizontal/Vertical/Stack), gap, alignMain, alignCross | container mode |
| `Padding` | l, t, r, b | inner padding |
| `Margin` | l, t, r, b | reserved, not yet honoured |
| `Align` | x, y (Start/Center/End) | child alignment |
| `Offset` | x, y | post-layout shift |
| `Constraints` | minW, maxW, minH, maxH | size clamping |
| `OverflowPolicy` | mode | clip / scaleToFitY (none for now) |
| `Sprite` | textureKey, tint, scaleMode, nineSlice | image draw |
| `Text` | text, styleId, wrapWidthMode | text draw |
| `Shape` | kind (Rect/RoundedRect/Circle), fill, stroke, strokeWidth, radius | vector panel |
| `Shadow` | blur | drop shadow (raylib stub OK) |
| `TransformFx` | scaleX, scaleY, rotation, alpha, visible, pivot, anchor | post-layout transform |
| `Opacity` | value | alpha multiplier |
| `Active` | value | runtime visibility/participation |
| `Name` | label | tooling |
| `PrimaryType` | typeName | semantic |
| `NodeId` | id | stable id |
| `Button` | role | semantic marker |
| `OnClick` | actionId, actionIdDouble, actionIdTriple | action metadata |
| `PointerEvents` | enabled, blockChildren | hit testing |
| `HitArea` | kind, radius | hit shape |
| `MaskShape` | kind, sourceNodeId, radius | clip child to shape |
| `SceneInstance` | sceneId, params | mount another scene |
| `TextBinding` | key, format, prefix, suffix | text from runtime data |
| `ImageSourceResolver` | mode (Fixed/Random/Hash/Direct), textureKeys, seed | texture selection |
| `Children` | List(String) | hierarchy ordering |

**Derived (runtime only, never in the file):**

| Component | Computed by |
|---|---|
| `MeasuredSize` | `LayoutSystem.measure` |
| `ComputedRect` | `LayoutSystem.compute` |
| `WorldTransform` | `TransformSystem` |
| `RuntimeOffset` | future Wobble/BackgroundPan |
| `LayoutScale` | `OverflowPolicy.scaleToFitY` |
| `SafeInsets` | `SafeAreaSystem` |
| `ParentLink` | `HierarchySystem` (see below) |

Listed components are roughly two thirds of RUICS's full inventory.
Drop `Carousel`, `BackgroundPan`, `SmokeFx`, `AnimFrames`/`AnimTrigger`,
`WobbleFx`, `DataRequest`, `ListView` for now — the music player
doesn't need any of them.

## Module split (`lib/ui/`)

```
lib/ui/
  ecs.rae                # Entity, ComponentTable(T), World
  components.rae         # All authored + derived component types
  registry.rae           # Component name → (deserialiser, applier)
  scene.rae              # .raescene JSON → in-memory Scene (untyped tree)
  scene_loader.rae       # Scene → ECS entities + components
  scene_instance.rae     # SceneInstance mount, params, overrides
  hierarchy.rae          # ParentLink + Children traversal helpers
  layout.rae             # measureSubtree + computeSubtree
  transform.rae          # TransformSystem
  text_measure.rae       # Hug-text measurement (raylib measureText)
  text_binding.rae       # TextBinding resolver protocol
  image_source.rae       # ImageSourceResolver (Direct mode is enough)
  masking.rae            # MaskShape stub for Circle / RoundedRect
  input.rae              # Pointer events, hit testing, click dispatch
  render.rae             # Immediate-mode raylib painter
```

And one new shared module:

```
lib/json.rae             # JsonValue tree + parseJson
```

Each file is small and focused. Per CLAUDE.md, target <1000 LOC each.
The biggest file is likely `layout.rae` (~400 LOC for two-phase layout
with row/column/stack and Hug/Fill resolution).

## System pipeline (per frame)

```rae
loop not windowShouldClose() {
  inputSystem(world)             # gather pointer state, dispatch OnClick
  textBindingSystem(world)       # rewrite Text.text from runtime keys
  textMeasureSystem(world)       # write MeasuredSize for hug-text nodes
  imageSourceSystem(world)       # resolve ImageSourceResolver → Sprite.textureKey
  safeAreaSystem(world)          # write SafeInsets
  layoutSystem(world)            # measure → compute → ComputedRect
  transformSystem(world)         # ComputedRect + parent → WorldTransform

  beginDrawing()
  clearBackground(color: bgColor)
  renderSystem(world)            # paint by world transform z-order
  endDrawing()
}
```

The pipeline matches RUICS conceptually but is simplified by raylib's
immediate-mode model: there's no `RenderSyncSystem`/handle creation,
since we paint each frame from scratch.

## Music player screen 2 — concrete plan

Screen 2 (the album view) is the build target for phase 1. Here's the
node decomposition:

```
AlbumRoot               Vertical, gap 16, padding 24
├── TopBar              Horizontal, alignCross Center
│   ├── BackButton          Sprite "icon-back"
│   ├── TopBarSpacer        Size w Fill
│   └── SearchButton        Sprite "icon-search"
├── AlbumHero           Horizontal, gap 16, alignCross Center
│   ├── AlbumArt            Sprite, 96×96, Shape RoundedRect mask r=12
│   └── AlbumTitleGroup     Vertical, gap 4
│       ├── AlbumMeta           Text "Album · 8 songs · 2012"
│       ├── AlbumTitle          Text "Charcoal", styleId "h1"
│       └── AlbumArtist         Text "Brambles", styleId "subtitle"
├── ActionRow           Horizontal, gap 12
│   ├── AddPlaylistBtn      Sprite "icon-add-playlist"
│   ├── DownloadBtn         Sprite "icon-download"
│   └── MoreBtn             Sprite "icon-more"
├── PlayShuffleRow      Horizontal, gap 12
│   ├── PlayBtn             Shape RoundedRect black,
│   │                       Layout Horizontal alignCross Center,
│   │                       OnClick "music.play"
│   │   ├── PlayIcon            Sprite "icon-play"
│   │   └── PlayLabel           Text "Play", styleId "button-on-dark"
│   └── ShuffleBtn          Shape RoundedRect outlined,
│                           OnClick "music.shuffle"
│       ├── ShuffleIcon         Sprite "icon-shuffle"
│       └── ShuffleLabel        Text "Shuffle", styleId "button-on-light"
├── TrackList           Vertical, gap 8
│   ├── Track1              SceneInstance "track-row" with overrides
│   ├── Track2              SceneInstance "track-row" …
│   ├── Track3              SceneInstance "track-row" …
│   ├── Track4              SceneInstance "track-row" …
│   └── Track5              SceneInstance "track-row" …
├── MiniPlayer          SceneInstance "mini-player" with overrides
└── TabBar              SceneInstance "nav-tabs" Active=Library
```

Five `.raescene` files in this round:

```
scenes/music-player-album.raescene       # screen 2
scenes/music-player-now-playing.raescene  # screen 1 (phase 2)
scenes/track-row.raescene                 # one track row, scaffolded
scenes/mini-player.raescene               # mini player pill
scenes/nav-tabs.raescene                  # bottom tab bar
```

`track-row.raescene` carries the standard fields:

```json
{
  "type": "Scene", "version": 2, "sceneId": "track-row",
  "root": "Row",
  "nodes": {
    "Row": {
      "Layout": { "type": "Horizontal", "gap": 12, "alignCross": "Center" },
      "Children": ["TrackNumber", "TrackIcon", "TrackTitleGroup", "TrackOverflow"]
    },
    "TrackNumber":      { "Text": { "text": "01", "styleId": "track-number" } },
    "TrackIcon":        { "Sprite": { "textureKey": "icon-eq" } },
    "TrackTitleGroup":  { "Layout": { "type": "Vertical", "gap": 2 },
                          "Children": ["TrackTitle", "TrackArtist"] },
    "TrackTitle":       { "Text": { "text": "—", "styleId": "track-title" } },
    "TrackArtist":      { "Text": { "text": "—", "styleId": "track-artist" } },
    "TrackOverflow":    { "Sprite": { "textureKey": "icon-more-h" } }
  }
}
```

Then each `Track1..5` in the album scene gets `SceneInstance` with
overrides for `TrackNumber.Text.text`, `TrackTitle.Text.text`, and
`TrackArtist.Text.text`. That's the override mechanism doing exactly
what RUICS's list bindings do, just written by hand for five rows.

When `lib/ui/list_view.rae` lands later, those five overrides become
one `ListView` component with a 5-record data source. Same scene
file, less verbose authoring.

### Assets

`examples/98_mobile_ui/assets/`:

- icons (PNG, ~48×48) — back, search, add-playlist, download, more,
  more-h, play, shuffle, eq, heart, pause, home, search-tab, library,
  hotlist
- album art placeholder (PNG, 256×256) — `Charcoal` cover

About 14 small icons. Easiest path: hand-pick from a permissively
licensed icon set (e.g. Lucide or Heroicons MIT) and ship them
alongside. Alternatively, draw them as `Shape` primitives — works for
the ones that are clearly geometric (back chevron, search circle,
shuffle X-arrow), less well for the playlist-add and download icons.

### Text styles

`lib/ui/text_styles.rae` (or just on `World` for now): a registry
keyed by `styleId` returning `(font slot, font size, color, line spacing,
weight)`. Five styles cover both screens:

| styleId | size | weight | color |
|---|---|---|---|
| `h1` | 28 | bold | text-primary |
| `subtitle` | 14 | regular | text-muted (underlined for "Brambles" link) |
| `body` | 14 | regular | text-primary |
| `track-number` | 13 | regular | text-muted |
| `track-title` | 16 | medium | text-primary |
| `track-artist` | 13 | regular | text-muted |
| `button-on-dark` | 16 | medium | white |
| `button-on-light` | 16 | medium | text-primary |

Roboto is already shipped from `97_tetris3d/Roboto-Regular.ttf` — copy
into `assets/` here, or factor it out to a stdlib-friendly location.

## Music player screen 1 — what's added

Screen 1 ("Now Playing") is mostly a different composition of the
same primitives, plus one new component:

- Hero album art at large size + reflective shadow
- A waveform/progress visualiser (a row of vertical bars representing
  audio amplitude) — needs a new `WaveformBars` component or a
  `Shape: VerticalBars` variant. Cheapest option for the demo:
  `Shape RoundedRect` arranged via `Layout Horizontal` with
  per-bar overrides. Real waveforms wait.
- A circular play button — `Shape Circle` with a centred play icon
  inside via `Layout Stack`.

It reuses the same `nav-tabs` scene instance with a different `Active`
override (`TabHome` instead of `TabLibrary`).

The new bits in stdlib for screen 1: nothing essential. Both screens
should be reachable from the same library after phase 1.

## Phase plan

| Phase | What lands | Stops gracefully if cut here? |
|---|---|---|
| 0 | This design doc, music player screen PNGs in place. | Yes (already cut). |
| 1a | `lib/json.rae` JSON tree parser + tests. | Yes — useful on its own. |
| 1b | `lib/ui/ecs.rae`, `lib/ui/components.rae`, `lib/ui/registry.rae`. World boots, can `add`/`get` components by hand. | Yes — useful as a pure-Rae ECS demo. |
| 1c | `lib/ui/scene.rae` + `lib/ui/scene_loader.rae`: parse a `.raescene`, instantiate the entities, no layout yet. Just dump the entity list. | Yes — proves the loader. |
| 1d | `lib/ui/layout.rae` + `lib/ui/transform.rae`. Renders rectangles only via `lib/ui/render.rae`. | Yes — the album scene shows as a tower of grey boxes, but it's positioned correctly. |
| 1e | `Sprite` + `Text` + `Shape` painting in `render.rae`. Plus `lib/ui/text_measure.rae` for hug-text. | At this point screen 2 looks ~right without scene instances yet — track rows hand-laid. |
| 1f | `lib/ui/scene_instance.rae` + override application. Screen 2 wired up properly with shared `nav-tabs` and instanced track rows. | First commit-ready milestone. |
| 1g | `lib/ui/input.rae` + click dispatch. Tabs become tappable. | Screen 2 ships as a static-state demo. |
| 2 | Screen 1 (Now Playing) added as a second `.raescene`. Tab navigation switches between them. | Two-screen demo. |
| 3 | (Future) `ListView`, `DataRequest`, animations. Workout app screens. | Out of scope for this round. |

Each of 1a–1g is a small commit. 1d is the first user-visible
milestone — boxes on the screen in roughly the right places.

## What `examples/98_mobile_ui/main.rae` looks like

The whole example app fits in ~30 lines. The point is that the
heavy lifting lives in `lib/ui/`, not here:

```rae
import raylib

func main() {
  setConfigFlags(flags: 4)
  initWindow(width: 600, height: 1300, title: "Music Player")
  setTargetFPS(fps: 60)
  initHudFont()                                  # see 97_tetris3d

  let world: UiWorld = createUiWorld()
  registerCoreComponents(reg: world.registry)
  loadScene(world: world, sceneId: "music-player-album")

  loop not windowShouldClose() {
    inputSystem(world)
    textBindingSystem(world)
    textMeasureSystem(world)
    imageSourceSystem(world)
    safeAreaSystem(world)
    layoutSystem(world)
    transformSystem(world)

    beginDrawing()
    clearBackground(color: { r: 248, g: 248, b: 250, a: 255 })
    renderSystem(world)
    endDrawing()
  }

  unloadHudFont()
  closeWindow()
}
```

Nothing music-player-specific in code. Switching to screen 1 is a
one-line change to `loadScene(world, sceneId: "music-player-now-playing")`.

## Open questions

1. **Texture loader**: where do raylib `Texture` handles live? Probably
   on the `World` as `textures: List(TextureSlot)` (parallel to
   `Sprite.textureKey`), loaded lazily from `assets/`. That's a
   `lib/ui/textures.rae` module — added in phase 1e.

2. **Hot-reload**: `.raescene` files are pure data. Watching them and
   re-loading on change should be straightforward — `prepareLayoutRootForLoad`
   in RUICS is the model. Not in phase 1, but it's free if we keep the
   scene loader idempotent.

3. **Coordinate space**: raylib uses pixel coordinates with origin
   top-left. Authored scenes assume the same. No translation needed
   between the two — keep it that way.

4. **HiDPI**: the cropped reference PNGs are around 535×1037 — that's
   already roughly logical-pixel size. We render at 1× for now and
   let raylib's window scale handle Retina.

5. **List virtualisation cutover**: the design treats the five track
   rows as five hand-instanced `SceneInstance`s for now. That's fine
   up to maybe 50 rows. Once `ListView` lands, the album scene can
   replace the five `Track1..5` entries with one `TrackList` node
   carrying `ListView { itemSceneId: "track-row", ... }`. Same scene
   file shape, less authoring.

6. **Live target**: the live VM has worked for raylib so far in
   `97_tetris3d`. The new `lib/ui/` modules use only generics + structs,
   nothing that should challenge it. Still, plan to test live + compiled
   in parallel from phase 1d onward.

## Why this gets us closer to Rae's stated goals

The reference doc closes with the line "Make scene instancing
first-class, not an afterthought." `97_tetris3d` already nudged Rae
toward a Bevy-flavoured component architecture; this round is the
next step — components stop being fields on a `World` struct hand-coded
in a single example, and start being a library that any Rae app can
opt into. The scene file format keeps UI authoring data-driven, which
the language has been waving toward (`design.md`'s "easy for AI agents
to parse, generate, refactor") but not yet earned.

The biggest debt this round will leave is the per-component-name
deserialiser registration in `lib/ui/registry.rae` — it's manual today
and a real reflection-or-codegen story would erase it. That's a
language-level conversation, not a UI-system one, and it can come
after the music player ships.

## Cross-references

- `rae/docs/royal-blush-ruics-ecs-reference.md` — the source document
  this design tracks. Read it first if you're touching the layout or
  scene-instance code.
- `examples/97_tetris3d/DESIGN.md` — the previous round's
  component/system layout; the same pattern, but per-example rather
  than stdlib.
- `examples/98_mobile_ui/screens/music_player_screen_*.png` — the
  visual targets.
- `lib/raylib.rae` — already has `loadFontInto`, `drawTextWithFont`,
  `getMonitorWidth/Height`, `setWindowSize/Position`, plus the standard
  shape and texture API. Sufficient for phase 1.
