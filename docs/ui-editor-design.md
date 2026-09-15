# UI Editor — a `.raescene` viewer that grows into an editor

**Status:** design, 2026-09-15. Implementation is queued as rae QUEUE #995–#1000
(one task per section below, difficulty 4 each). Example number **121_ui_editor**,
devtools tab **UI** (owned by `.raepack category: "UI"`, next to 104/105/106).

## 1. What it is

An example that is more than an example: an ECS / `.raescene` app whose job is to
open ANY `.raescene` file and show it as the UI system renders it — first as a
passive watcher (open one file, letterboxed 9:16, re-render when the file changes on
disk), then as a read-only inspector (hover/click a node, see its id, components and
rect), and only later as an editor that writes scenes back. It is the dogfooding
surface for the whole `lib/ui` scene format: every component `lib/ui` can author must
load and draw here, and whatever it cannot load must be a visible diagnostic, never
a crash or a silent hole.

The old, bit-rotted external viewer (raylib era: a hard-coded list of 28 scene
paths, one fixed top scene, a `Camera2D` zoom as a fit rule, a texture manifest) is
inspiration only. Nothing from it — and none of its trademarked scene files — is
copied; its ideas that survive are: a registry of every scene in a directory with
the top scene picked by id, recursive `SceneInstance` resolution, a key→path
texture manifest, and a design-resolution + fit rule (now native:
`Gpu2d.setDesignResolution`).

## 2. Inputs

- **The scene path** is the first program argument: `rae run --project
  examples/121_ui_editor examples/121_ui_editor/Main.rae -- path/to/scene.raescene`.
  Rae has no program-argument API today (the generated `main(argc, argv)` discards
  `argv` and `rae run` passes nothing through) — #995 adds `Sys.argCount()` /
  `Sys.argAt(i)` and the `--` passthrough. `RAE_UI_EDITOR_SCENE=<path>` is the env
  fallback (headless gates use it). No argument → the shipped sample
  `assets/samples/MainMenu.raescene`.
- **Sub-scenes** referenced by `SceneInstance { sceneId }` resolve to
  `<scene dir>/<sceneId>.raescene`, loaded lazily into one `SceneRegistry`
  (recursively). Missing → red sentinel block + diagnostic, the page still mounts.
- **Theme:** `<scene dir>/theme.raescene` (version 3) if present, else the library
  default theme. An unresolved token (`"spaceL"`, `"surface"`) is a diagnostic and
  falls back to the default value, never a crash.
- **Textures:** `Sprite.textureKey` → the first hit in `<scene dir>/`,
  `<scene dir>/../textures/`, `<scene dir>/../images/`, then
  `$RAE_UI_EDITOR_ASSETS/` (`<key>.png`). A miss draws a labelled placeholder
  (key text on a hatched box), not nothing.
- **Window:** default 9:16, `540x960` logical; `RAE_UI_EDITOR_WINDOW=WxH` overrides;
  resizable. **Design resolution** = the root node's authored `Rect` when it has one,
  else `1080x1920`, `RAE_UI_EDITOR_DESIGN=WxH` overrides; fit = contain (letterbox),
  so a phone scene stays a phone in any window.

## 3. Architecture (ECS, one `UiWorld`, two layers)

One `UiWorld`; the app's own chrome and the viewed document are two page roots on
two layers, so the document's layout can never push the chrome around and the
chrome can be hidden for a pure passive view (`H` key).

- **document layer** — the loaded scene, mounted with `mountPage` (one call, exactly
  like a 106 page) under a `Viewport` node that carries the design resolution and
  the letterbox. Remounted whole on reload.
- **chrome layer** — authored in the app's OWN `.raescene`
  (`assets/scenes/editor.raescene`, `chrome/*.raescene` sub-scenes): a top bar (file
  name, design size, node count, diagnostics count, `watching` dot), a status line,
  later the inspector/tree side panel. The chrome is data; the app has no
  `createEntity` for layout.

Resources on `App` (no globals): `document: SceneDocument { path, mtime, scene,
registry, theme, pageRoot, designW, designH, diagnostics: List(SceneDiagnostic) }`,
`assets: TextureSearch`, `inspector: InspectorState`, `chrome: ChromeState`.

Systems (folder-per-system, registered on the `lib/ui` `Pipeline` `Schedule` after
the library systems, like 106's `FramePipeline.rae`):

| folder | system | does |
|---|---|---|
| `documentSystem/` | `documentLoadSystem` | parse + register + mount; on failure keep the last good page and show the parse error in the chrome |
| `watchSystem/` | `fileWatchSystem` | poll `Sys.fileMtime` of the scene, its sub-scenes and theme every 250 ms (only while the window is visible); changed → `documentLoadSystem` remount, scroll preserved |
| `diagnosticsSystem/` | `diagnosticsSystem` | owns `List(SceneDiagnostic)` (unknown component, runtime-only component, unknown token, missing sub-scene, missing texture, parse error); writes the chrome's counter + list (a `ListView` — the `lib/ui` list system, dogfooded) |
| `inspectorSystem/` | `inspectorSystem` | hover → highlight rect; click → select; overlay with node id, component names (`componentNamesFor`), computed rect; arrow keys walk the tree; `Esc` clears |
| `viewportSystem/` | `viewportSystem` | design resolution + letterbox from the document; window resize → re-fit |

Rendering is the stock `lib/ui` render system; the highlight is a `Shape` entity on
a third `overlay` layer driven by `inspectorSystem`, not a special draw path.

## 4. Loader policy (the library change that makes a viewer possible)

`applyComponentByName` today calls `exit(1)` on a component it does not know. That
is right for an app's own scenes and wrong for a tool that opens other people's:
game-specific components (`CardView`, `LevelDef`, `DragToSnapBack` in the reference
files) must load as a **per-node diagnostic**, the node still mounts with what it
has. #996 adds a `ScenePolicy { unknownComponents: fail | report }` on the world
(the default stays `fail`; the editor sets `report`) and a `SceneDiagnostics`
resource the registry appends to. The same policy covers runtime-only tables,
unknown theme tokens and `#rrggbb[aa]` colours (the reference format's colour
spelling — `deserRgba` accepts only tokens and `{r,g,b,a}` today; hex becomes a
third accepted spelling for everyone, not an editor special case).

## 5. Coverage rule and support matrix

"Everything found in `lib/ui` must be supported" is a test, not a promise. #1000 ships
`examples/121_ui_editor/assets/samples/Coverage.raescene` (+ `CoverageCard`,
`CoverageRow`), which authors EVERY name `registeredComponentNames(world)` returns —
78 at the time of writing — with every authored field, loaded under the `report`
policy; the example gate asserts `[ui-editor] mounted Coverage: N nodes, 0
diagnostics`. Every sample is rendered headlessly, asserted non-blank and compared
with its checked-in reference (`examples/121_ui_editor/references/*.png`, regenerated
by `make -C examples/121_ui_editor references`, compared by
`compiler/tools/assert_bmp_diff.py` — under 0.1% of pixels may differ; a reference
rendered at another DPR is skipped, not failed).

**Loads** means the component decodes with no diagnostic. **Draws** means the
renderer honours it on screen today. The samples column names the scene that shows
it (`Coverage` unless a nicer example exists).

| component | authored fields | loads | draws | shown in |
|---|---|---|---|---|
| Rect | x y w h | yes | yes | all |
| Size | w/h `{mode min max}` Fixed / Fill / Hug | yes | Fixed, Fill; **Hug on a Text leaf measures as its authored Rect** (the layout has no font — follow-up) | Coverage `HugText` |
| Layout | type Horizontal / Vertical / Grid / Stack / None, gap, alignMain (incl. SpaceBetween), alignCross, columns, rowGap, columnGap | yes | yes | CardStrip, Coverage `SpriteRow` (grid) |
| Padding, Margin | l t r b (space tokens) or a padding preset name | yes | yes | all |
| SafeArea | enabled, apply, extra | yes | yes (app viewport insets are 0 in the editor) | Coverage |
| Align | x, y, hasX, hasY | yes | yes | Coverage `HugText` |
| Offset | delta | yes | yes | Coverage |
| Constraints | minW maxW minH maxH + has* | yes | yes | Coverage `WrappedText` |
| Overflow | mode None / Clip / ScaleToFitY | yes | Clip (rounded when the node has a radius) | Coverage `Screen` |
| ExtentAnchor | h | yes | yes (roots) | Coverage |
| Aspect, ScaleToFit | ratio; mode | yes | yes (fit system) | Coverage `GradientBox` |
| TransformFx | scaleX scaleY rotation alpha visible pivot anchor | yes | scale about the pivot (#1000), alpha, visible; **rotation is carried by the transform but not drawn** (gpu2d has no rotated quads — follow-up) | CardStrip, Coverage `ScaledBox` |
| Sprite | textureKey (`mat:` glyph or image), tint, tintSlot, scaleMode Fit / Fill / Stretch, tileScale, nineSlice, hasNineSlice | yes | key, tint, scaleMode; **nineSlice and tileScale are not drawn** (gpu2d images have no sub-rect / tiling — follow-up) | MainMenu, Coverage |
| Text | text, styleId, wrapWidthMode None / NodeWidth, styleOverride | yes | yes | Coverage `WrappedText` |
| TextShadow | color, offset, softness | yes | yes | Coverage `Heading` |
| Shape | kind Rect / RoundedRect / Circle, fill, stroke (token, `{r,g,b,a}` or `#hex`), strokeWidth, radius | yes | yes | all |
| Shadow | blur | yes | yes — a soft drop shadow (three growing translucent layers, #1000) | Coverage `ShadowBox` |
| Opacity | value | yes | yes, inherited down the tree | Coverage `FadedBox` |
| GradientFill | from, to, angle | yes | yes | Coverage `GradientBox` |
| CornerRadius | radius | yes | yes | Coverage |
| MaskShape | kind Circle / RoundedRect, sourceNodeId, radius | yes | own box: yes (#1000; a masked sprite rounds through its own quad); **`sourceNodeId`: axis-aligned scissor only** (gpu2d rounds clips for boxes, not images/text) | Coverage `CircleMask`, `SourceMasked` |
| BackdropImage | textureKey | yes | image when registered, glass fallback otherwise | Coverage |
| HoverScale | restScale hoverScale speed current target | yes | yes (interactive) | Coverage `ShadowBox` |
| HitArea, OnClick, Button, PointerEvents, ActionBinding | kind radius; actionId actionIdDouble actionIdTriple maxDelayMs; role; enabled cursor blockChildren; actionId role | yes | input only (no pixels) | MainMenu, Coverage |
| Active, EditorVisible, EditorLocked, DebugAnchor, RuntimeOnly | value | yes | Active hides; the rest are flags | Coverage |
| Name, PrimaryType, NodeId, SceneScope, HeroWidget, WidgetId, WidgetState | strings / flags | yes | metadata (the inspector shows them) | Coverage |
| LayerRoot, LayerRef, LayerTag, PageRoot | id order; (layer is runtime); id; id | yes | paint order / page bookkeeping | Coverage `Footer` |
| ScrollState | y velocity | yes | yes (lists) | Settings |
| SceneInstance | sceneId, params.overrides[] (Text, Sprite, OnClick, Active, Rect w/h, Padding l/t/r/b) | yes | yes | CardStrip, Coverage `CardsRow` |
| ListView | itemSceneId itemKeyField itemHeight itemGap visibleItemCount overscanRows bindings[] loadingSceneId emptySceneId errorSceneId | yes | yes (the editor seeds placeholder rows) | Settings, Coverage `ListBlock` |
| TextBinding, DataRequest, DataDependency, RefreshRegion, ImageSourceResolver | as declared | yes | data bindings — no editor system feeds them; **`ImageSourceResolver.textureKeys` (a List field) does not decode** (compiler reflection reports a List field as its element type — follow-up) | Coverage |
| ContainerStyle | a theme container name | yes (an unknown name is an unknownToken diagnostic, #1000) | yes | Coverage `TextBlock`, CoverageCard |
| ProfileStat, AvatarSource, CoverGrid, SearchField, SearchResults, HistoryList, AnchorBottom, NavTab, AlbumHeader, TrackList, ProgressBar, BottomSheet, PlaybackIcon, PlaybackCover, LikedHeart, TrackText | 106's app bindings | yes | app systems (106) drive them; inert in the editor | Coverage `MetaRow` |
| BackgroundPan, Carousel, SmokeFx, WobbleFx | as declared | yes | **no runtime system yet** (declared for the format) — follow-up | Coverage |
| AnimFrames, AnimTrigger | baseKey frames textureKeys fps loopForever onEndTextureKey; event actionId restartOnTrigger | yes (scalars); **`frames` / `textureKeys` List fields do not decode** (same reflection gap) | **no runtime system yet** — follow-up | Coverage `NineSlice` |

Follow-ups queued from this matrix: gpu2d rotated quads + image sub-rects/tiling
(TransformFx rotation, Sprite nineSlice/tileScale) and rounded clips for images/text
(sourceNodeId masks); a font-aware Hug measure for Text leaves; the animation
systems (AnimFrames/AnimTrigger/WobbleFx/BackgroundPan/SmokeFx/Carousel); and the
compiler's `typeName` of a `List(T)` field.

## 6. Not in the first versions

Writing scenes back (the "editor" half), a file picker, drag-to-move, undo. The
inspector is read-only until the DAW/editor capability design
(`ui-ecs-refactor-status.md` §2.7) lands; the app is shaped so those are new
systems, not rewrites.
