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
- **The scene root** (#1008, docs/ui-scene-format.md): every scene reference —
  `import`, `SceneInstance.sceneId`, a ListView's row scenes — is a package path
  without extension (`Theme`, `shared/GameAssets`, `cards/HeroCard`) resolved as
  `<root>/<path>.raescene`. The root is the opened scene's directory unless
  `--scene-root <dir>` (or `RAE_UI_EDITOR_ROOT`) says otherwise — the way `rae run`
  takes the entry file's folder as the project unless `--project` is given.
  Sub-scenes load lazily into one `SceneRegistry` (recursively). Missing → red
  sentinel block + diagnostic, the page still mounts.
- **Theme and assets** come ONLY through the scene's `"import": [...]` list (and
  its own inline token / `assets` blocks) — nothing is read by convention; the
  old `<dir>/theme.raescene` auto-load is gone, and a scene that still relies on
  it gets a diagnostic naming the `"import": ["Theme"]` line it needs. An
  unresolved token (`"spaceL"`, `"surface"`) is a diagnostic and falls back to
  the default value, never a crash.
- **Textures:** `Sprite.textureKey` → the scene's (and imports') `assets.textures`
  `map` entry, else `<dir>/<key>.png` over its `dirs`; `$RAE_UI_EDITOR_ASSETS/<key>.png`
  is a viewer-only override tried first. A miss draws a labelled placeholder
  (key text on a hatched box), not nothing.
- **Fonts:** `assets.fonts.text` / `.icons` (a `.mtsdf.json`; the sibling atlas is
  `.png` or `.raw`), else the viewer's fallback Roboto + Material atlases;
  `RAE_UI_EDITOR_FONT` / `RAE_UI_EDITOR_ICON_FONT` override.
- **Window:** default 9:16, `540x960` logical; `RAE_UI_EDITOR_WINDOW=WxH` overrides;
  resizable. **Design resolution** = the root node's authored `Rect` when it has one,
  else `1080x1920`, `RAE_UI_EDITOR_DESIGN=WxH` overrides; fit = contain (letterbox),
  so a phone scene stays a phone in any window. The design resolution the canvas
  fits is the LARGER of the document's design and the chrome's own `1080x1920`,
  per axis: the chrome shares the canvas, so following a small document (a
  `993x130` row scene) shrank it into the letterboxed strip and took the Open
  button with it. A document smaller than the design keeps its authored size and
  sits centered in it — a translation, never a scale.
- **A `.raepack` as the PROJECT** (#82531251, `projectSystem/`): the same inputs
  (argument, `RAE_UI_EDITOR_SCENE`, later the picker) accept a `.raepack`. Its
  directory becomes the scene ROOT — the `rae run --project` semantics: every
  package path, `import`, sub-scene, texture dir and font resolves from it —
  or, when the pack DECLARES where its scenes live (`scenes: { root:
  "assets/scenes" }`, the way `sources` declares where its code lives), that
  directory under the pack; 106 declares it, so `Theme` / `TrackRow` resolve in
  the editor exactly as in the app (`--scene-root` / `RAE_UI_EDITOR_ROOT` still
  win, as an explicit override).
  Every `*.raescene` under it (recursively; dot-dirs, `build`, `node_modules`
  skipped; sorted) is a scene of the project, identified by its package path
  (`assets/scenes/Album`); the first one mounts, or the one named by
  `--scene <package/Path>` / `RAE_UI_EDITOR_PROJECT_SCENE`. The chrome's project
  panel (`P` hides it; open by default when a project is loaded) lists them as
  tappable rows: a tap is the watcher's reload with the new path, so the
  selection, scroll positions and the watch list follow; the root does not
  move. `RAE_UI_EDITOR_TEST_OPEN=<index>` is the headless tap. The pack is used
  for its LOCATION (and its `name` in the panel title) — no dependency
  resolution. DECIDED: **no `chdir`** — the process working directory is never
  moved behind the app's back (hidden global state); everything keys off
  `document.root`, and `dir()` stays the app's own. If a later tool genuinely
  needs the cwd moved it gets an explicit `Sys.setWorkingDirectory` call, never
  an implicit one. Note that a project whose scenes get their theme from app
  code rather than an `import` (106 today) mounts with the #1008 "add
  `import`" diagnostic — that is the rule doing its job, not the editor's.
- **Opening a file at runtime** (#44411932): the argument/env inputs above are the
  boot path; a running editor also opens files through the OS "open file" panel.
  That panel is a GENERIC binding in the platform layer, not an editor feature —
  `lib/Sdl3.rae` `openFileDialog(filters, defaultLocation)` / `pollFileDialogResult()`
  on SDL3's `SDL_ShowOpenFileDialog`. It is async (request now, the chosen path
  arrives on a later poll — the same one-shot shape as the `windowResized` flag),
  knows nothing about scenes or packs, and NEVER changes the process working
  directory. Headless/tests drive it without a panel: `RAE_SDL_FILE_DIALOG_RESULT`
  resolves the next request to that path (empty => cancel), and a
  `RAE_SDL_HEADLESS_MS` run never opens a real panel.
- **The picker** (#51700883): `O`, or the chrome's `Open (O)` pill (an authored
  `OnClick` `file.open` row in `Editor.raescene`, not a code-built widget),
  requests `Sdl3.openFileDialog(filters: ["raepack", "raescene"], defaultLocation:
  document.root)`; the frame loop polls `pollFileDialogResult()` and a non-empty
  path goes through **the ONE opener** — `documentOpenPath`, the same function the
  boot argument / env and the project-panel rows use (`.raepack` → project open,
  `.raescene` → scene open) — so the picker adds no second code path. Cancel
  leaves the document untouched; a file that fails to parse keeps the last good
  page with the error in the chrome (the reload policy). The `--scene-root` /
  `RAE_UI_EDITOR_ROOT` override describes the file named on the command line, so
  it applies to the boot open only — a file picked later gets its own root.
  `RAE_UI_EDITOR_TEST_OPEN_FILE=1` requests the dialog after the first frame for
  the headless gate, which answers it with `RAE_SDL_FILE_DIALOG_RESULT`.
  So "Inputs" is: argument, env, project row, picker — one resolver.

## 3. Architecture (ECS, one `UiWorld`, two layers)

One `UiWorld`; the app's own chrome and the viewed document are two page roots on
two layers, so the document's layout can never push the chrome around and the
chrome can be hidden for a pure passive view (`H` key).

- **document layer** — the loaded scene, mounted with `mountPage` (one call, exactly
  like a 106 page) under a `Viewport` node that carries the design resolution and
  the letterbox. Remounted whole on reload.
- **chrome layer** — authored in the app's OWN `.raescene`
  (`assets/scenes/Editor.raescene`, `chrome/*.raescene` sub-scenes): a top bar (file
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
| `watchSystem/` | `fileWatchSystem` | poll `Sys.fileMtime` of the scene, its sub-scenes and imports every 250 ms (only while the window is visible); changed → `documentLoadSystem` remount, scroll preserved |
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
| Size | w/h `{mode min max}` Fixed / Fill / Hug | yes | yes — Fixed, Fill, and Hug on a Text leaf hugs its glyphs (#1004: a pre-layout pass writes the font measure into MeasuredText, read by measureNode) | Coverage `HugText` |
| Layout | type Horizontal / Vertical / Grid / Stack / None, gap, alignMain (incl. SpaceBetween), alignCross, columns, rowGap, columnGap | yes | yes | CardStrip, Coverage `SpriteRow` (grid) |
| Padding, Margin | l t r b (space tokens) or a padding preset name | yes | yes | all |
| SafeArea | enabled, apply, extra | yes | yes (app viewport insets are 0 in the editor) | Coverage |
| Align | x, y, hasX, hasY | yes | yes | Coverage `HugText` |
| Offset | delta | yes | yes | Coverage |
| Constraints | minW maxW minH maxH + has* | yes | yes | Coverage `WrappedText` |
| Overflow | mode None / Clip / ScaleToFitY | yes | Clip (rounded when the node has a radius) | Coverage `Screen` |
| ExtentAnchor | h | yes | yes (roots) | Coverage |
| Aspect, ScaleToFit | ratio; mode | yes | yes (fit system) | Coverage `GradientBox` |
| TransformFx | scaleX scaleY rotation alpha visible pivot anchor | yes | scale about the pivot (#1000), rotation about the pivot (#1003: boxes, images and glyphs turn; children follow), alpha, visible; `anchor` is accepted but has no layout meaning yet | CardStrip, Coverage `ScaledBox` |
| Sprite | textureKey (`mat:` glyph or image), tint, tintSlot, scaleMode Fit / Fill / Stretch, tileScale, nineSlice, hasNineSlice | yes | key, tint, scaleMode; nineSlice (borders keep their pixel thickness, authoring any border implies the flag) and tileScale (repeat) since #1003 | MainMenu `Panel`, Coverage `NineSlice` |
| Text | text, styleId, wrapWidthMode None / NodeWidth, styleOverride | yes | yes | Coverage `WrappedText` |
| TextShadow | color, offset, softness | yes | yes | Coverage `Heading` |
| Shape | kind Rect / RoundedRect / Circle, fill, stroke (token, `{r,g,b,a}` or `#hex`), strokeWidth, radius | yes | yes | all |
| Shadow | blur | yes | yes — a soft drop shadow (three growing translucent layers, #1000) | Coverage `ShadowBox` |
| Opacity | value | yes | yes, inherited down the tree | Coverage `FadedBox` |
| GradientFill | from, to, angle | yes | yes | Coverage `GradientBox` |
| CornerRadius | radius | yes | yes | Coverage |
| MaskShape | kind Circle / RoundedRect, sourceNodeId, radius | yes | yes — own box or `sourceNodeId`; since #1003 the image and text pipelines round a clip like the box pipeline, so the mask is exact for every primitive | Coverage `CircleMask`, `SourceMasked` |
| BackdropImage | textureKey | yes | image when registered, glass fallback otherwise | Coverage |
| HoverScale | restScale hoverScale speed current target | yes | yes (interactive) | Coverage `ShadowBox` |
| HitArea, OnClick, Button, PointerEvents, ActionBinding | kind radius; actionId actionIdDouble actionIdTriple maxDelayMs; role; enabled cursor blockChildren; actionId role | yes | input only (no pixels) | MainMenu, Coverage |
| Active, EditorVisible, EditorLocked, DebugAnchor, RuntimeOnly | value | yes | Active hides; the rest are flags | Coverage |
| Name, PrimaryType, NodeId, SceneScope, HeroWidget, WidgetId, WidgetState | strings / flags | yes | metadata (the inspector shows them) | Coverage |
| LayerRoot, LayerRef, LayerTag, PageRoot | id order; (layer is runtime); id; id | yes | paint order / page bookkeeping | Coverage `Footer` |
| ScrollState | y velocity | yes | yes (lists) | Settings |
| SceneInstance | sceneId, params.overrides[] (Text, Sprite, OnClick, Active, Rect w/h, Padding l/t/r/b) | yes | yes | CardStrip, Coverage `CardsRow` |
| ListView | itemSceneId itemKeyField itemHeight itemGap visibleItemCount overscanRows bindings[] loadingSceneId emptySceneId errorSceneId | yes | yes (the editor seeds placeholder rows) | Settings, Coverage `ListBlock` |
| TextBinding, DataRequest, DataDependency, RefreshRegion, ImageSourceResolver | as declared | yes | data bindings — no editor system feeds them; `ImageSourceResolver.textureKeys` decodes from its JSON array (#1007; Coverage's `IconSprite` carries `["logo"]`) | Coverage |
| ContainerStyle | a theme container name | yes (an unknown name is an unknownToken diagnostic, #1000) | yes | Coverage `TextBlock`, CoverageCard |
| ProfileStat, AvatarSource, ListSource, SearchField, HistoryList, AnchorBottom, NavTab, AlbumHeader, ProgressBar, BottomSheet, PlaybackIcon, PlaybackCover, LikedHeart, TrackText | 106's app bindings | yes | app systems (106) drive them; inert in the editor | Coverage `MetaRow` |
| BackgroundPan, Carousel, SmokeFx, WobbleFx | as declared (+ `Carousel.autoAdvanceSec`, #1005) | yes | yes — lib/ui/animationSystem/EffectSystems.rae (#1005): sine / two-sine runtime offsets, a swipe-or-timer paged container with `previewScale`, puffs recycled from the node's authored children | Coverage `BackdropBox`, `IconSprite`, `CardsRow` |
| AnimFrames, AnimTrigger | baseKey frames textureKeys fps loopForever onEndTextureKey (+ `frameCount`, #1005); event actionId restartOnTrigger | yes — the `frames` / `textureKeys` List fields decode from their JSON arrays (#1007; Coverage's `NineSlice` authors `frames: [0, 1, 2]`), or author `baseKey` + `frameCount` for `<baseKey><i>` keys | yes (#1005): frames advance at fps into Sprite.textureKey, loop or stop on onEndTextureKey; a trigger (actionId, or click/hover on the node) starts / restarts | Coverage `NineSlice` |

`typeName` of a `List(T)` field is spelled in full since #1006, and a scene's
`List(Int)` / `List(String)` field is populated from its JSON array since #1007.
`componentGet` / `componentDataAt` now return an independent deep copy of such a
component; use `componentView` in read-only hot paths to avoid that copy.
The effect systems landed with #1005. Rotation,
nine-slice, tiling and rounded clips for every pipeline landed with #1003.

## 6. Not in the first versions

Writing scenes back (the "editor" half), a file picker, drag-to-move, undo. The
inspector is read-only until the DAW/editor capability design
(`ui-ecs-refactor-status.md` §2.7) lands; the app is shaped so those are new
systems, not rewrites.
