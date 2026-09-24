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
- **Window:** the editor is a DESKTOP app: it opens landscape, `1400x900` logical
  (`RAE_UI_EDITOR_WINDOW=WxH` overrides), resizable, and it works on a phone-sized
  window too (the panels minimise from the top bar; they always START open). The
  chrome is authored in logical POINTS: the canvas design resolution is the
  window's logical size (physical pixels / DPR), re-applied on every resize, so a
  76pt top bar is 76pt on any display and the chrome never scales with the
  document. **The document's frame** = the root node's authored `Rect` when it has
  one, else `1080x1920`, `RAE_UI_EDITOR_DESIGN=WxH` overrides (and the device frame
  below for a phone-authored scene); the camera FITS the frame into the chrome's
  `EditArea` on load and zooms/pans it after, so a phone scene stays a phone at any
  window size. The design-space rule that used to letterbox the chrome into the
  document's space is gone. **Device frame:** a scene that declares `unitScale` (design
  units per point — every 106 scene says `3`) is authored for a phone, and the
  editor lays it out in the SAME frame the app does (106's `Viewport.rae`): the
  extent is the active `ui/DevicePresets` preset in design units (iPhone 15 Pro:
  393x852 pt x 3 = `1179x2556`), the page is a centered 360pt column (`1080`),
  and the preset's safe-area insets (59 pt x 3 = `177` top) reach the page
  through its own `SafeArea { apply }` nodes via `safeAreaSystem`. Without it a
  106 page mounted 25% shorter, 9% wider and flush to the top — "the same scene,
  totally different sizes". `RAE_UI_EDITOR_DEVICE=<preset id|none>` picks the
  preset (default `iphone-15-pro`, 106's own default); a scene without
  `unitScale` gets no frame and is unchanged. The status line reports the frame
  (`1179 x 2556 · column 1080 · safe 177`). Not simulated: what the APP composes
  around a page — its dock, mini player, bound data, downloaded images.
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
- **Guides:** a dashed outline around the frame (the page column — the design area the
  document is laid out in) and one around the safe area inside it, each labelled —
  the reference editor's overlays (docs/ui-editor-reference-features.md §2a). They
  are an AUTHORED overlay scene, `assets/scenes/chrome/Guides.raescene`, on a
  `guides` layer between the document and the chrome: plain `Shape` nodes with the
  dashed stroke (`Shape.strokeDash` / `strokeGap`, painted by the stock render system),
  positioned from the document's frame by `guideSystem/` after every load. `G`
  toggles the camera-bearing frame subtree; `RAE_UI_EDITOR_GUIDES=0` starts it
  hidden without hiding the screen-space selection chrome. The safe-area guide shows
  only when the frame has insets.
- **Camera** (#99444501, `cameraSystem/`): the wheel zooms the DOCUMENT about the
  cursor (0.25x–5x, 10% per notch), a middle-button drag pans it, and the chrome's
  zoom pill (`−` / `100%` / `+`, authored `OnClick` rows in the top bar) steps ±10%
  about the canvas centre — its label click resets to 100%. The camera is a scale
  + offset written as a `TransformFx` (scale about the origin) and a
  `RuntimeOffset` on the document page root and the guides root, the same
  transform-pass inputs a scroll uses; the chrome is untouched. `lib/ui` now
  honours an INHERITED world scale everywhere it reads a rect — paint box (own
  scale grows about the pivot, inherited scale only sizes), glyph size and
  placement, clip and mask rects, hit-testing — so the stock systems do the rest.
  `RAE_UI_EDITOR_ZOOM=<factor>` boots zoomed (the headless gate's hook).
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
- **The picker** (#51700883): `O`, or the chrome's `Open...` pill (the `O` key does the same) (an authored
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
  (`assets/scenes/Editor.raescene`, `chrome/*.raescene` sub-scenes), the desktop
  editor layout the reference editor uses (docs/ui-editor-reference-features.md §3):
  a 76pt **top bar** (`Panels` toggle, file name, zoom pill, `Open...`, node count,
  diagnostics pill, `Inspector` toggle), a **Body** row of `LeftPanel` (328pt:
  Project / Hierarchy / Diagnostics sections as clipped ListViews, `chrome/*Row`
  rows) · `EditArea` (Fill — the space the camera fits the document into) ·
  `RightPanel` (384pt: the `chrome/Inspector` instance, always shown, "nothing
  selected" is a state), and a 42pt **status bar** (frame size, status line). The
  top-bar toggles fire `panel.left` / `panel.right` and flip the panels' `Active`;
  `H` hides the whole chrome. The chrome is data; the app has no `createEntity`
  for layout. Pointer input reaches the document only inside `EditArea`, so a
  panel over the document never selects through.
- **The chrome is driven through components, never node ids** (#32499842, the
  106 shape): every label carries `TextBinding { key: "editor.<name>" }`
  (`fileName`, `nodeCount`, `diagnosticsCount`, `designSize`, `statusLine`,
  `zoomLabel`, `projectTitle`, `frameLabel`, `safeAreaLabel`,
  `inspector.nodeId|type|components|rect|parents`) and `chromeSystem/`'s
  `editorTextSystem` writes them from the `EditorStatus` resource on `App`; the
  panels carry `EditorPanel { panel }` and the pills that minimise them
  `PanelToggle { panel }` (`panelToggleSystem` flips every panel of the kind a
  fired pill names; the key handlers call `setEditorPanelOpen` by kind); the
  space the document is fitted into is tagged `EditArea`; the three lists carry
  `EditorList { source: hierarchy|project|diagnostics }` and one list system
  each fills their `ListViewData`; the guide shapes and labels carry `GuideRect
  { kind }`; the count pill `DiagnosticsBadge`. The components are lib tables
  (`lib/ui/BindingComponents.rae`, the "registry is the world" rule), so the
  loader deserialises them by reflection like any other; no editor system
  contains a `chrome/...` string except the page id and the row-scene package
  ids.
- **Outlines are authored too** (#12306774): the inspector's hover and selection
  outlines are `HoverOutline` / `SelectionOutline` Shape nodes in `Guides.raescene`
  tagged `OutlineFor { role }` (the reference's styles: hover `#28f0ff` w2 α0.6,
  selection `#ff4fd8` w2.5 α0.9); the inspector only positions them (Rect size +
  RuntimeOffset from the target's world transform, compare-before-write — a moved
  outline is a frame change so the transform pass runs). The reference's
  "Outlines" and "Buttons" toggles are `OutlinePool { matches: all|buttons,
  itemSceneId }` nodes: `outlineSystem/` keeps one mounted instance of the
  authored outline scene (`chrome/NodeOutline`, `chrome/ButtonOutline`) per
  matched document entity, the ListView row-pool shape; `N` / `B` toggle them
  (EditorPanel kinds `outlines` / `buttons`), `RAE_UI_EDITOR_OUTLINES=nodes|
  buttons|both` boots with them on. Since the guides root's child `FrameSpace`
  carries the `Camera2D`, the outlines are its screen-space siblings. The app
  creates no entity of its own any more except the layer roots and pool
  instances of authored scenes.

Resources on `App` (no globals): `document: SceneDocument { path, mtime, scene,
registry, theme, pageRoot, designW, designH, diagnostics: List(SceneDiagnostic) }`,
`assets: TextureSearch`, `inspector: InspectorState`, `chrome: ChromeState`.

Systems (folder-per-system, registered on the `lib/ui` `Pipeline` `Schedule` after
the library systems, like 106's `FramePipeline.rae`):

| folder | system | does |
|---|---|---|
| `documentSystem/` | `documentLoadSystem` | parse + register + mount; on failure keep the last good page and show the parse error in the chrome |
| `documentSystem/DocumentSave.rae` | `saveDocument`, `saveDocumentTo` | Cmd/Ctrl+S writes the document page back to its file through `lib/ui/SceneWriter` (+ `SceneEncode`): a component that still equals a fresh decode of its authored text is written VERBATIM (theme tokens, hex colours, the author's field and node order survive), a changed or added one is encoded in the grammar (the inverse fixups: Rect {x,y,w,h}, Layout `type`, flattened insets, palette slots, Text styleOverride, DataDependency keys, SceneInstance overrides, ListView bindings), the node's pending app components as they were read, the headers from the parsed file; canonical 2-space layout, one node per block. `RAE_UI_EDITOR_TEST_SAVE=<path>` is the headless hook. Fixture 884 round-trips every 121 sample and every 106 scene (load → save → load, every authored table equal in machine form, pending components verbatim) and an edited Rect |
| (bundles) | `BundleRefs` on a node | a node expanded from `bundles` (docs/ui-scene-format.md §9) carries the loader-written `BundleRefs { names, inherited, overridden }`; the inspector's components line appends `bundles: … [inherited: …] overrides: …` — the components the node took whole from a bundle versus the ones it authored a field over. The per-field diff on save waits for the save path |
| `EditorSystems.rae` | `applyEditorComponents`, `sweepEditorEntities` | the editor's aggregate glue (docs/ecs-systems-own-their-tables.md §3.2): the chrome / guides / inspector / outline-pool systems OWN their authored components (`chromeSystem/EditorComponents.rae`, no longer lib tables); after every mount the pending outbox is drained into them, and each frame the dead entities are cleared from them before the world recycles the indices |
| (lib outbox) | `world.pendingComponents` | an authored component no lib table matches is an APP's own (docs/ecs-systems-own-their-tables.md §4): the editor never reports it — it stays on the node as authored, the inspector lists it under "not a lib component", the diagnostics list shows it as an `appComponent` INFO row (the red pill counts only real diagnostics), and the mount/reload log says `N app components kept` |
| `watchSystem/` | `fileWatchSystem` | poll `Sys.fileMtime` of the OPENED document, its sub-scenes and imports every 250 ms (only while the window is visible); changed → `documentLoadSystem` remount, scroll preserved. This is for the document, which may live in another project; the editor's own chrome scenes are NOT watched here — under `rae watch` a chrome edit restarts the editor like any `.rae`/`.raescene` edit (docs/hot-reload-plan.md) |
| `diagnosticsSystem/` | `diagnosticsSystem` | owns `List(SceneDiagnostic)` (unknown component, runtime-only component, unknown token, missing sub-scene, missing texture, parse error); writes the chrome's counter + list (a `ListView` — the `lib/ui` list system, dogfooded) |
| `inspectorSystem/` | `inspectorSystem` | hover → highlight rect; click → select (see *Canvas selection* below); overlay with node id, component names (`componentNamesFor`), computed rect; arrow keys walk the tree; `Esc` clears |
| `viewportSystem/` | `viewportSystem` | design resolution + letterbox from the document; window resize → re-fit |

Rendering is the stock `lib/ui` render system; the highlight is a `Shape` entity on
a third `overlay` layer driven by `inspectorSystem`, not a special draw path.

### Chrome and sample visual tokens

The visual pass replaces heavy outlined panels and equally weighted actions with
charcoal elevations, a large title, a single teal primary pill, and quieter
secondary actions. The orbital illustration, progress card, card artwork,
settings rows and all chrome remain authored scene nodes. No widget is drawn
from application code and no C renderer code was added.

- **Sample palette:** background `#0C121B`, surface `#18222C`, raised surface `#24333D`,
  primary text `#EEF4F3`, secondary text `#9DACB5`, accent `#48B498` (72,180,152).
  Surface gradients stay close in value; the accent gradient runs from
  `#69C7AC` to the base teal. Colour is a hierarchy cue, not a border on every box.
- **Chrome palette:** deep green background `#021210`, sidebar `#041A18`,
  raised inspector `#0A3838`, primary text `#E6FAFA`, secondary `#9FC1BD`,
  mint `#78E6DC` and amber selected outline `#FFA84C`. The top bar is flat. The open action uses a dark teal fill and a stronger
  mint edge; secondary pills have a near-black teal fill and a quieter edge.
  Amber means selection: the selected document bounds use solid amber, hierarchy
  and project selection markers use soft amber `#FFD69E`, and optional clickable
  bounds use translucent amber. Hover and passive guides remain mint, so hovering
  a different node cannot be mistaken for changing the selection.
- **Sample type scale:** display 128, title 92, h2 42, body 34, secondary 28,
  caption 26, eyebrow 24 design units. Display leading is 1.08; prose is
  1.25–1.30. Compact card and onboarding headings author local overrides.
  The main-menu action labels use `OpticalAlign` to centre visible glyph bounds
  on both axes, rather than centring the font line box with its descender space. Chrome uses authored `OpticalAlign`,
  colour and 17–24pt size overrides, independent of the loaded document's styles.
- **Desktop readability:** chrome uses 19pt controls, tree rows and inspector
  details, 22pt file names, 24pt inspector headings and 17pt section/status text.
  These are authored logical sizes, equivalent to roughly 150% text scaling from
  a compact desktop UI; controls grow more gently, to 44pt pills and 38pt list
  rows. Wider 328/384pt sidebars accommodate the type. The canvas retains its
  independent document zoom; larger chrome never changes scene design units.
- **Spacing:** 8 / 16 / 24 / 40 / 64, with 64-unit sample gutters and 32-unit
  card padding. The main menu pairs its secondary actions and gives progress its
  own card. Settings keeps 144-unit rows with 16-unit separation.
- **Rounding:** sample cards 48, hero sheets 56, compact stat tiles 40;
  button radius is exactly half the button's height. Desktop chrome pills are
  44pt high with a 22pt radius; the inspector surface has a 20pt radius.
  Gradient nodes explicitly author `CornerRadius`, which is the gradient
  renderer's rounding input. Rounded-box ring nodes retain their authored stroke.
- **Shadows:** `Shadow { blur: 24, layers: 12, opacity: 0.32 }` gives a subtle
  layered edge. The pure-Rae renderer bounds layers to 1–16 and opacity to 0–1;
  existing scenes retain the original three layers and full opacity by default.
  Each layer is a draw, so this is used on selected large surfaces, not every row.
- **Status:** `DiagnosticsBadge.clearFill` and `errorFill` are authored colours;
  the existing ECS system only chooses between them. Defaults preserve previous
  scenes. Coverage authors these fields and the new Shadow fields, while retaining
  its component inventory, animation, list, mask and nested-scene exercises.

The watch-reload twin is regenerated from MainMenu with only the title changed
and one intentionally unknown `Telemetry` component. All chrome ids, bindings,
actions and the sample `PlayButton` remain stable. The split/import and inline
asset examples retain their distinct packaging models.

### Button roles and theme ownership

`assets/scenes/Theme.raescene` owns editor-prefixed colour roles, appended to
its owned theme on startup and every document load. The prefix keeps these roles
separate from a document's ordinary `accent`/`surface` palette. Custom palette
names now resolve through the generic component decoder and shape/sprite painter,
not only the ten standard palette names.

- **Primary:** Open has `Shape` with `editorButtonPrimary` fill, the stronger
  `editorButtonEdge` stroke and a restrained coloured halo. No bar-wide gradient.
- **Secondary:** Panels and Inspector use `editorButtonSecondary` and
  `editorButtonQuietEdge`. Their halo appears only while hovered.
- **Circular control:** Zoom minus/plus have equal 44pt dimensions, a 22pt
  rounded-box radius, no padding and centred glyphs. They use the same outline
  vocabulary and hover halo. Geometry belongs to these entities, not the palette.
- **Selection:** amber remains distinct from mint hover and primary actions.

`Shadow` now accepts `color` with `hasColor: true`, `centered` and `hoverOnly`. A centred coloured
shadow is a glow; `hoverOnly` reads the existing ECS `Interaction.hovered` state.
The existing defaults remain black, downward-offset and always visible. This is
bounded translucent geometry in Rae, not a Gaussian CSS blur: at most 16 layers,
with the editor using 12 only on its few controls. Coverage authors both an
always-visible colour glow and a hover-only one. It does not add C rendering or
per-button drawing code. The theme supplies shared colours; authored components
supply the role, border width, radius, glow amount and interaction condition.

CSS inset-shadow and animated filter/scale transitions are not emulated here.
The stronger single outline supplies a clean edge without stacked inset rings.
A future toggle may use existing `WidgetState`/`StateStyle` for its state; these
editor controls retain their existing actions and do not introduce a pretend
speaker setting.

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
| Shadow | blur, layers, opacity, color, hasColor, centered, hoverOnly | yes | yes — bounded translucent layers; defaults preserve the original three-layer shadow | Coverage `ShadowBox` |
| Opacity | value | yes | yes, inherited down the tree | Coverage `FadedBox` |
| GradientFill | from, to, fromSlot, toSlot, angle | yes | yes — literals or live palette slots | Coverage `GradientBox` |
| CornerRadius | radius | yes | yes | Coverage |
| MaskShape | kind Circle / RoundedRect, sourceNodeId, radius | yes | yes — own box or `sourceNodeId`; since #1003 the image and text pipelines round a clip like the box pipeline, so the mask is exact for every primitive | Coverage `CircleMask`, `SourceMasked` |
| BackdropImage | textureKey | yes | image when registered, glass fallback otherwise | Coverage |
| HoverScale | restScale hoverScale pressedScale speed current target | yes | yes (interactive) | Coverage `ShadowBox` |
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

## Typing into the Rect group (#44133406)

The Rect component is the inspector's one hand-written widget, and it is now
four real inputs — X, Y, W, H — plus a W/H lock, instead of a summary line
(`inspectorSystem/RectEdit.rae`, on top of RectWidget's model).

| key / gesture | effect |
|---|---|
| click a row | focus it; the whole value is SELECTED, so the first key replaces it (`[128]|`) |
| digits, `.`, `-` | type into the draft (`-` only first, one `.`) |
| Backspace | delete the last character |
| Tab | commit this field, move to the next (x → y → w → h → x) |
| Enter, or any other click | commit to EVERY selected entity, stop editing |
| Esc | drop the draft, stop — nothing is written |

Three rules the design turns on:

- **An untyped field commits nothing.** A MIXED field shows the mixed marker
  until something is typed; tabbing through it, or focusing it and pressing
  Enter, must not flatten a selection that disagrees into one value.
- **A blur commits to the selection it was typed for.** A click elsewhere is
  handled at the very top of the frame, before that click can change the
  selection — otherwise the draft would land on whatever the click selected.
- **The lock keeps each entity's OWN ratio.** Locking two differently-shaped
  nodes and typing a width scales each by its own proportion (Card 300×460 →
  w 150 gives h 230; CardCost 60×40 → w 120 gives h 80), never forcing them to
  one shape. An entity with a zero on either axis has no ratio to keep and
  takes the typed value alone. X and Y ignore the lock.

**One question decides whether keys are text or commands:**
`keyboardCaptured(inspectorSystem)`, asked ONCE at the start of the frame and
true while either filter or a Rect field has focus. While it is, the bare
panel shortcuts in App.rae, `T`, `U`, `/`, `F`, the arrows and the
deselecting Esc all stand down — and because it is asked at the start, the
key that ENDS an edit (Enter, Esc) is spent on the edit alone. This replaced
several single-flag checks and fixed latent bugs from the filter work, where
typing `t` or `u` into the tree filter toggled the panel or selected a parent,
and Esc in a filter also threw the selection away.

`RAE_UI_EDITOR_TEST_SET_RECT` takes a keystroke script (`focus:x type:5 tab
type:7 enter`, `lock w=150`, `esc`) and drives the SAME functions the keys
call. The original `h=64` form still works as shorthand for focus-type-Enter.

## The side panels are the same width

`LeftPanel` and `RightPanel` are both **384** wide. They were 328 and 384,
which pushed the `EditArea` — and so the document centred inside it — 28pt
left of the window centre. Measured on a 2800px render: the chrome background
ran 654px on the left against 766px on the right; it is 766/766 now.

They were matched to the WIDER of the two rather than the narrower, which also
relieved the hierarchy rows: `Name - Kind "preview"` was clipping at the old
panel's ~290pt inner width and fits at ~346pt.

Keep them equal. A fixed-width pair is what makes the canvas centred, so
changing one without the other silently re-introduces the offset — and the
reference screenshots, which frame the whole window, will shift by exactly
half the difference.

## One wheel notch, one consumer

Scrolling the hierarchy used to zoom the canvas at the same time. Two
systems read the device's per-frame wheel delta (`Gpu2d.wheelMove()`) on
their own: the lib `updateScrollRoot` scrolled any `ScrollRoot` the pointer
was inside, and the editor camera zoomed on any wheel at all. Each was fine on
its own; together they broadcast one notch to both.

The fix is in the lib, not in the editor: the wheel is **routed**.
`updateUiInput` (lib/ui/inputSystem) calls `routeUiWheel` once per frame,
which stores the delta on `UiInput.wheel` and the ONE entity that takes it on
`UiInput.wheelTarget`. The target is the nearest wheel consumer on the path to
the topmost node under the pointer. A consumer is a `ScrollRoot` (its window
is its parent's box) or a node tagged with the new lib component
`WheelTarget {}` (lib/ui/InputComponents.rae). The walk follows reverse paint
order, like the click hit test. Two rules keep it honest:

- a subtree that paints or takes clicks under the pointer **blocks** what
  is painted below it, so a popup that consumes nothing swallows the notch
  instead of letting it fall through;
- a clipping node (`Overflow: Clip`) hides its subtree outside its box. Tree
  rows scrolled out of the panel cannot catch the wheel over the panel next
  to them.

Systems read their share with `uiWheelFor(input, entityId)`, which is the delta
if the wheel was routed to `entityId` and 0 otherwise. `updateScrollRoot` uses
it for the root it steps. The editor authors `"WheelTarget": {}` on its
`EditArea`, and `cameraInputSystem` zooms by
`uiWheelFor(input, editAreaEntityId(...))`. So over the hierarchy the list
scrolls, over the canvas it zooms, and over the inspector's fields nothing
happens. With the chrome hidden, the canvas is the whole window and every
notch zooms. An overlay drawn outside the world (the debug overlay) drops the
notch, just as it drops the press.

Tests: `compiler/tests/cases/910_ui_wheel_routing` runs the router on a
hand-built world (list / overlay / clipped row / canvas / popup / idle). Each
of the rules above was sabotage-checked against it. The editor gate replays
`RAE_UI_EDITOR_TEST_WHEEL="TreePanel:-3 EditArea:2 InsWidgets:-1"`
(WheelScript.rae) through `routeUiWheel` and then every wheel reader a frame
runs. It fails with exactly the reported bug if the camera goes back to
reading the raw delta.

The 3D cameras came next. A world camera behind the UI is the FALLBACK
consumer. It takes `uiWheelUnclaimed(input)`, which is the notch only when no UI
consumed it (`wheelTarget` is none) and the pointer is not over UI that paints
or takes clicks. The router records the second half as `UiInput.wheelBlocked`,
so a notch over a panel's plain background or a button reaches nobody.
`updateCameraRig` no longer reads the device: it takes a `wheel:` argument.
111, 112 and 114 pass `uiWheelUnclaimed(input:)`, which fixes a live double
consume: each has a scrolling panel, and a notch over the panel's list used to
scroll it AND zoom the camera, because the camera was gated only by "a button
is hovered or pressed". 118 and 119 have no UI in the window and pass
`Gpu2d.wheelMove()`, as the only consumer. Fixture `914_ui_wheel_unclaimed`
covers a panel over a world view (list, panel background, button, clipped row,
3D view, idle). Removing the "over UI" half makes the camera zoom over the
panel background and the button, and the fixture catches it.

Still reading the device, on purpose: 109, 110 and 115, whose hand-rolled
cameras run in windows with no UI world, and 106's own scroll code, which uses
its own input system and has no world camera. Each is the only wheel reader in
its app. Route it through `uiWheelFor`/`uiWheelUnclaimed` the day it gains a
second one.

## Hover glows fade, then the editor sleeps

Every top-bar button glows under the pointer. The icon buttons and the quiet
pills use a `Shadow` with `hoverOnly: true`, which is invisible at rest.
`Open...` is the primary button, so its glow is always on (opacity 0.22), and
it used to look the same whether hovered or not. `Shadow` now has a second
opacity, `hoverOpacity`. A value above 0 keeps the shadow at `opacity` at rest
and brightens it to `hoverOpacity` under the pointer. `Open...` authors
`"hoverOpacity": 0.6`. This is a scene parameter, so no button code knows
about it.

The change between rest and hover is animated. `buttonHoverFadeUpdate`
(lib/ui/buttonSystem/ButtonHoverFade.rae) keeps a runtime `HoverFade { amount }`
on every hover-reactive Shadow, in the ButtonSystem's own table next to
`Interaction`. It moves `amount` linearly toward 1 while the node is hovered
and toward 0 when it is not, taking 0.15 s for a full fade. The painter mixes
the rest and hover opacity by `amount`. An app that never runs the system
gets the old instant switch.

The system returns true only while a fade is still moving, and it snaps the
last float step so it cannot ask for a frame it would not show. The editor
feeds that into the same `animating` flag as the effect systems. It renders
every frame for about 0.15 s after the pointer enters or leaves, then drops
back to its blocking event wait and renders nothing until the next event.

Tests: `compiler/tests/cases/911_ui_hover_fade` checks the fade: 3 frames in,
3 frames out, `animating` false on the settled frame, plain shadows
untouched, and the instant fallback. The 121 gate hovers `OpenPill` through
`RAE_UI_EDITOR_TEST_HOVER` (HoverScript.rae), which calls the same
`buttonInteractionMark` the input system uses, and requires the frame to
differ from the un-hovered MainMenu shot. Removing `hoverOpacity` makes the two
frames identical.

## What a hierarchy row says (#89663819)

A row reads `Name - Kind`, plus a value preview where one exists, so a node
can be identified WITHOUT selecting it — the thing that makes a long tree
navigable:

```
Screen - Shape
Title - Text  "Orbit Dr..."
ParentMount - Shape
```

`Kind` is the authored `PrimaryType` when there is one, else derived from the
components the node actually carries, most specific first (a node with a
ListView is a List whatever else it has; a bare container is a Node). The
PREVIEW is deliberately only two cases — a Text node's string and a scene
instance's file — because those are where a node's identity lives in its data
rather than its name; a preview of nothing is noise in a narrow panel. It
truncates at 8 characters: the row already spends its width on `Name - Kind`,
and a longer preview is clipped by the panel edge mid-word, which reads as
broken rather than truncated. A node carrying bundles appends `+<bundle>`.

**The header carries the node count and the filter** — `Hierarchy (40)`, or
`Hierarchy (11)  filter: stat` — so the panel says how much of the tree you
are looking at without selecting anything.

**The filter keeps ANCESTORS of a match.** Hiding a non-matching parent would
cut its matching descendants out of the tree entirely, so a node survives when
anything beneath it matches. It matches against the whole row label, so
filtering `orbit` finds `Title` by its TEXT, not just nodes named Orbit\*.
It has its own focus key, `F`, mutually exclusive with the component list's
`/` — two filters, two panels, one keyboard.

**Mounted scene roots name their source file.** Every mounted node carries the
runtime-only `SceneSource { sceneId, nodeId, root }`. The root flag makes the
hierarchy preview the file only on the row where that scene is mounted, while
the source node id lets every descendant resolve its authored fields. This is
separate from authored `SceneScope` and never appears in saved `.raescene` text.
`BundleRefs` remains the independent `+<bundle>` provenance.

## The selected-row plate (#38090472)

A selected row is a rounded gradient plate, not a glyph: `chrome/TreeRow`
carries a transparent `GradientFill` until a row binds its `fromSlot` and
`toSlot`. Unselected rows therefore stay transparent, while live theme changes
re-resolve both ends of a selected row's gradient.

**TWO token pairs, not one.** `editorRowActiveFrom/To` mark the node the
inspector describes and the arrow keys walk from;
`editorRowSelectedFrom/To` mark the rest.
With many rows selected a single flat colour cannot say which row is which —
the question a one-row selection never has to answer, and the reason the task
asked for this to "look right with MANY rows selected". Both are palette
entries, so a theme change carries them and nothing is hardcoded.

The `>` and `-` markers are gone, replaced by the plate. **`*` stays**: a
COLLAPSED row hiding a selected descendant is not itself selected, so it has
no plate to speak for it (#72481430). The marker column remains even when
empty so the label keeps its indent.

`references/TreeSelection.png` pins this as a screenshot diff of a three-row
selection — a log line cannot check a colour.

## Collapsing the hierarchy (#72481430)

A row with children carries an authored vector chevron: two rounded Shape
strokes form the expanded or collapsed icon, coloured by
`editorTreeChevron`. A leaf carries neither icon. Each icon is its own hit
target with `treeToggle:<nodeId>`, handled before the row's `inspect:` action.
The row also authors six one-level guide cells. Bindings activate the first N
for depth N, so each level contributes both 12 du of indentation and a subtle
`editorTreeGuide` vertical rule without code-drawn chrome.

**Collapsed state is keyed on NODE ID**, never an entity or a row index. The
tree rebuilds on every selection change and a reload recreates every entity,
so either of those would lose the state on the next keystroke; a node id
survives both. Nothing clears the list on reload — that IS the persistence.

Two consequences fall out of collapsing in `collectTreeRows` rather than
filtering afterwards:

- **A Shift-range spans only VISIBLE rows.** `treeEntities` is built by the
  same walk, so a collapsed subtree is simply not in it. With `LogoRing`
  expanded, `Title` → Shift+`Panel` is twelve rows; collapsed, it is four.
  Nothing in the range code changed.
- **Collapsing KEEPS hidden descendants selected.** Dropping them would
  silently shrink a set the user built. The collapsed parent then shows `*`
  to say it is hiding part of the selection — a third marker beside `>` for
  the active node and `-` for the other selected rows.

The hierarchy now reveals its active row when the active entity changes.
`lastRevealed` records that entity; refreshing the panel or clicking the same
active row preserves manual scrolling. A filtered or collapsed-away target is
retried when it becomes visible, and document reload resets the remembered
entity. Reveal cancels momentum and clamps to the full data extent.

The authored `TreeViewport` is the fixed 284-du clip (seven 38-du rows with
3-du gaps). Its child `TreeList` owns `ScrollRoot` / `ScrollState` and moves
through the existing virtual-list runtime offset. The clip must belong to the
stationary parent: clipping the moving list itself moved its window offscreen.
Wheel and drag use the shared scroll physics. The Footer regression reveals
row 39 at -1353 du, keeps the selected row visible, and captures the rendered
strip; a separate refresh check preserves a manually chosen -82-du offset.

## The headless hooks resync the tree (#85867026)

`RAE_UI_EDITOR_TEST_*_CLICKS` make a selection AFTER `inspectorAfterLoad` has
already built the hierarchy rows against the EMPTY selection. Every row cell
that carries selection — the `>` / `-` marker, a highlight plate's theme token
— was therefore bound to nothing in every headless screenshot, and the loop's
own list/layout pass is gated on `changed`, which a scripted hook never sets.
So `logInspectorPanel` now resyncs the TREE panel as well as the inspector,
and the boot path runs `listViewSystem` + the scheduled systems once more
after the hooks, so re-bound rows are laid out before the first frame.

That blind spot produced a false bug report: a `Shape.fillSlot` ListView row
binding was "silently inert" on the hierarchy's virtual-mode list. It is not.
Fixture `902_listview_virtual_shape_binding` pins the lib behaviour headlessly
— a virtual list delivers the binding to the row scene's ROOT and to a CHILD,
on the first bind and on a same-key rebind where only that cell changes — and
with the harness fixed, the same binding and an existing token turn a
three-row selection into 121,076 changed pixels in the hierarchy strip. The
one thing that could still be silent, a slot NAME the palette does not have,
is not silent either: `paletteColor` paints it magenta.

## Select parent, and the identity block (#23728694)

**Select parent walks the whole SET up the tree.** The click-through gesture
walks ONE node up the hit stack under the cursor; this is the operation a
gesture cannot express, so it is an explicit affordance: a `select parent` row
in the inspector and the `U` key.

Two decisions:

- **Siblings collapse.** Two selected children of one Button become that one
  Button, not the same node twice — the result is deduplicated, or "select
  parent" on a set of siblings would return a selection full of duplicates.
- **A node already at the scene root contributes ITSELF.** Dropping it would
  silently shrink the selection whenever it contained a top-level node, and a
  Select parent that sometimes deselects is worse than one that sometimes does
  nothing. If nothing in the selection can move up, the operation reports no
  change and the selection is untouched.

**The identity block** heads the component list at n=1 (an entity id, a name
and a parent are per-entity facts, and the header already names a
multi-selection):

```
identity: entityId=#10 gen 0  node=PlayButton  name=PlayButton  type=(none)
          parent=Panel #9   children=PlayIcon #11, PlayLabel #12
```

That is what turns the panel from a property sheet into an ECS debugger: these
are the ids diagnostics print, so they can be matched up by eye. The child list
truncates at eight with a `+N more`, so a container with fifty children does
not push the panel off screen.

## Adding and removing a component (#57156455)

A component is not only a bag of editable fields — it is present or absent on
an entity. `inspectorSystem/ComponentEdit.rae` changes that by NAME, walking
every `ComponentTable` of the world and matching `typeName(table)`, so nothing
names a component and a new one becomes add/removable the day it joins
`UiWorld`. Adding needs a default value, which is `T.default()` inside a tiny
generic helper — `T` is inferred from the `ComponentTable(any)` binding, the
same trick the decoder uses.

Over a selection the two are deliberately **asymmetric**:

| | applies to |
|---|---|
| add | every selected entity that does NOT have it |
| remove | every selected entity that DOES have it |

Add skips an entity that already carries the component rather than resetting
it — "add" must never be destructive on a node someone already configured,
which is the one way this could quietly lose authored work. Both report how
many entities actually changed (`removed Shape from 1` of a 2-node selection),
because that count is the only way to tell a real edit from a click on a
component only some of the selection had.

The actions are authored controls in `chrome/ComponentRow`, not words in the
value column. A component header ends in a red `x` pill; its first press turns
amber and asks for confirmation, and its second press removes. Add candidates
end in teal `+` pills. Their hover glow and scale, plus the smaller authored
`HoverScale.pressedScale`, come from ordinary UI components, so the pooled
ListView rows need no code-drawn affordance. Rect input rows retain their
whole-row actions; component add/remove actions use the dedicated child hit
target.

**The filter.** The add list is every component the world knows — ~79 rows —
so it is unusable without narrowing, which is what makes the filter part of
this feature rather than a nicety. It matches case-insensitively on a
substring, so `tran` finds `TransformFx` without knowing where the capitals
fall, and it narrows BOTH the present-component list and the add list.

There is no text-input widget in the editor yet (that is the per-field input
task), so the filter is typed with a **focus mode**: `/` focuses it, letters
type, Backspace deletes, Esc clears and unfocuses. Focus is not optional
here — the editor binds BARE letters as panel shortcuts (`H`, `D`, `G`, `N`,
`B`, `P`), so without a mode, typing a filter would toggle half the chrome.
`App.rae` gates exactly that block on `filterFocused`; everything below it
carries a modifier and so cannot be typed text.

The unfiltered add section starts collapsed and its `+` / `-` header toggles
it. Typing a filter opens matching candidates automatically. The component
list itself is a virtual ListView and an authored `ScrollRoot`: a Hug inner
list moves inside a separate clipped viewport, so the clip remains fixed as
the list scrolls. `inspectorUpdate` advances the shared drag/wheel physics and
suppresses a component action on the release frame of a drag.

## What DRIVES a value, inline with it (#21438052)

Rae's Rect is authored in a `.raescene` and then routinely overwritten by the
layout system, so a bare number in the inspector cannot say which won. Every
generated field row now carries where its value came from
(`inspectorSystem/FieldSource.rae`):

| tag | meaning |
|---|---|
| *(none)* | authored, and the live value still equals what the author wrote |
| `· from <x>` | the authored spelling decodes to this typed value (for example, a theme token or hex colour) |
| `· changed from <x>` | the live value differs from a fresh decode of what the author wrote |
| `· computed` | the component is absent from this node's authored bag — nothing in the file put it here |

`Name: label=PlayButton · computed` is the loader's own doing; `gap=16 · from
spaceS` is a theme token resolved to a number, and a changed gap would read
`gap=24 · changed from spaceS`. Hex colours use the same rule: an authored
`accent` or `#00000000` string is a resolution when it produces the current
`RgbaColor`, not an overwrite.

The inspector owns one reference `UiWorld` freshly decoded with the active
theme on each successful document load. It resolves the complete registered
sub-scene graph once, then uses each node's runtime `SceneSource` to compare
against the correct file and source node. Changing the selection therefore
does not mount another world or repeat token resolution. The save path uses
the same fresh-decode contract for preserving authored spellings.

**What Rae can and cannot answer.** WHICH SYSTEM wrote a value is *not*
available: `ComponentTable` carries a table-level `generation` and per-entity
`denseStamps`, but those record WHEN a component last changed, not WHO changed
it, and no system declares what it writes. So the general annotation is
provenance against the parsed scene, which is exact. RectWidget's `layout` /
`fill` / `align` tags remain the bespoke exception — they are INFERRED from
configuration (the entity's Size mode, a parent carrying a Layout, its own
Align), per-component knowledge only a hand-written widget can have.

Three limits, deliberate:

- **A mixed field makes no claim.** With the selection disagreeing there is no
  single authored value to have been overridden.
- **The picker is not compared.** An enum renders `kind=roundedRect
  [rect|roundedRect|circle]`; comparing that decorated text with the author's
  `"RoundedRect"` would call every enum overridden, so the comparison uses the
  undecorated value and is case-insensitive (the scene spells enums
  PascalCase).
- **A node without `SceneSource` gets no tag at all.** Loaded document and
  sub-scene nodes carry it; runtime-created nodes do not. The module stays
  silent rather than claiming `computed` about a node with no authored source.
  Nested authored values compare through their freshly decoded typed Rae value;
  when unchanged they stay quiet.

## Component widgets are generated from reflection (#24323144)

No widget is written per component. `inspectorSystem/ComponentWidget.rae`
walks every `ComponentTable` of `UiWorld` — `loop let table: view
ComponentTable(any) in fields(world)` — picks the one whose `typeName(table)`
is the component asked for, and hands it to a generic helper whose `T` is
**inferred from that binding**. Inside the helper `T` is a real type, so
`componentView` yields a `view T` and `fields(value)` renders its fields.
Add a component to `UiWorld` and it has a widget; nothing here names one.

What a field row shows, all from reflection: the field's **name**, its value in
Rae's own spelling (`"{f}"` — `SizeAxis { mode: fixed, min: -1, max: -1 }`),
and for an **enum** the current member plus the picker list
(`kind=horizontal [none|horizontal|vertical|grid|stack]`, from
`enumMembers(F)`, which folds to `[]` for a non-enum so the same line
compiles for every field type). Field ORDER is declaration order. Every row
folds over the SELECTION SET with the RectWidget conventions — common value or
the mixed marker — and a component only some of the selection carries heads
its rows with `(have/total)`.

**The override table.** `widgetOverride(name) ret WidgetSpec` is the
hand-written refinement, keyed by component name and merged over the
generated default exactly the way the reference editor merges custom entries
over generated ones. Today it carries: `bespoke` (Rect — RectWidget's line
with its driver annotation replaces the generic rows), `hideFields`
(runtime state such as `ScrollState.y`, rewritten every frame), `unit`
(`px` on Padding/Margin/Offset), and `step` / `min` / `max` (Opacity:
`±0.05 [0..1]`). Fields whose TYPE is runtime plumbing — `EntityId`,
`List(EntityId)`, a ListView's materialised rows — are hidden by type so no
override has to list them, and a component with no visible field gets no
header (Children).

**Decided: no language change for the metadata.** The task allowed one. Range,
step, unit, read-only and grouping are per-component EDITOR concerns, and
every way of putting them on the type was worse than a table:

- an `@range(0, 1)` attribute is the `@`-sigil vocabulary AGENTS.md rules out;
- a field keyword (`opacity: Float range 0 1 step 0.05`) is a real language
  feature whose only consumer is one tool's inspector, and it would put
  presentation (a unit, an increment) into a type that the layout system,
  the scene loader and the serializer also read;
- what reflection ALREADY gives — name, type, order, enum members, value text
  — is everything that is a property of the type. What is missing is
  precisely what is not.

If a second consumer of per-field metadata appears (a serializer wanting
`read-only`, say), the shape to reach for is a compile-time plain function
over the FIELD in the spirit of `fieldName` / `typeName` — never a sigil.

**A compiler gap this hit.** A `fields()` loop is UNROLLED into straight-line
code per field, so `continue` inside one has no enclosing loop. Sema rejects
that in a plain or directly-called generic body — but NOT when the generic's
`T` is inferred through a `ComponentTable(any)` field-loop binding; there the
bare `continue` reaches the C compiler. Nested `if`s express the skip; the
bug is queued with its repro.

The generated widgets render as the `components` ListView
(`chrome/ComponentRow`, one row per field, indent from the row cell), and
`RAE_UI_EDITOR_TEST_*_CLICKS` logs them as one `widgets Comp: f=v …; Comp2: …`
line the example gate asserts.

## The inspector is multi-selection only (#82314313)

There is no single-selection code path. `syncInspectorPanel` renders the
COMPOSITE of `inspectorSystem.selection`, and **n=1 is that same code with one
member** — which is what stops a single selection from drifting away from what
multi-selection shows.

| line | n = 1 | n > 1 |
|---|---|---|
| header | the node id | `Selection (n): id, id, …` |
| identity | `Type / Name` | the shared **type**, or the mixed marker |
| components | the node's components | the UNION, each partial one `(have/total)` |
| Rect | the value | common value or mixed, per field (#54986153) |
| parents | the parent chain | the chain when all agree, else mixed |

Two decisions are worth keeping:

- **A partial component is marked `(have/total)`, not just flagged.** `Layout
  (1/2)` says an edit there will reach one of the two selected nodes. A bare
  "partial" marker would leave the reader counting.
- **The identity line folds the TYPE but not the NAME.** A name is per-entity
  meta — two Text nodes are always named differently — so folding it would
  report every multi-selection as mixed and hide the one thing they share. At
  n=1 the name is shown, which is the "slightly more capable editor" a single
  selection gets: not a branch that says `if single`, just a field that only
  agrees when there is one member.

The union keeps `componentNamesFor`'s fixed per-entity order, so first-seen
order across the selection is the canonical order rather than an accident of
which node was clicked first.

One Rae gotcha this hit: assigning an owned `String` local into a variable that
outlives the loop iteration leaves it dangling — it showed up as NUL bytes in
the panel text. Interpolate (`line = "{value}"`) to make a new String instead.

## Selection (#20778145, #16096994)

### The canvas

The editor keeps ONE selection set — `InspectorSystem.selection`, with
`selection`'s last member as the ANCHOR (`selected`) — and the canvas, the
hierarchy and the inspector all read it. Every path that changes it goes
through `applySelect(… mode: SelectMode)`, so the panels cannot grow two
vocabularies for the one set:

| gesture | mode | effect |
|---|---|---|
| click | `replace` | the set becomes exactly the hit node |
| click on empty canvas | `replace` | the set is CLEARED |
| Cmd/Ctrl+click | `toggle` | in if it was out, out if it was in |
| Shift+click | `add` | in, never out |
| Alt/Opt+click | `subtract` | out, never in |

Three decisions are worth keeping:

- **A modified click on empty canvas does nothing.** Only a plain click
  clears. Someone holding Shift to extend a selection and missing the node is
  asking to add nothing, not to throw the set away, and losing a carefully
  built selection to a near-miss is what makes an editor feel hostile.
- **`subtract` is separate from `toggle`** even though a toggle can remove.
  With stacked nodes a toggle is ambiguous about which one you meant to drop,
  and "remove this" is worth being able to say without first knowing the
  state. When several modifiers are held they are ranked by specificity —
  subtract, then toggle, then add — so a stray Shift never silently turns an
  Alt+click into an add.
- **A press on chrome is not a click on empty canvas.** `onCanvas` gates both
  the hover and the click; without it every press on a panel toggle would
  clear the selection.

**An empty-canvas left drag is a marquee.** It becomes a drag only after four
screen pixels; below that threshold it remains the ordinary empty click above,
so a plain click still clears and a modified miss still does nothing. Once it
crosses the threshold, the authored `Marquee` node in `chrome/Guides.raescene`
shows the rubber band in chrome space. On release it selects every active,
visible document node whose complete transformed screen rectangle is ENCLOSED
by the band. Intersection is deliberately insufficient: a casual sweep across
part of a large container must not unexpectedly select that container.

The modifier is captured when the press begins. Plain replaces the set, Shift
adds every enclosed node, Cmd/Ctrl toggles each, and Alt/Opt subtracts each. A
plain band enclosing nothing clears; an empty modified band does nothing. The
multi-node operation is only a small adapter: its first plain node uses
`replace`, later nodes use `add`, and every mutation still calls `applySelect`.
Middle drag remains camera pan and never enters the left-button gesture.

### Click the same spot again to go up (#73816378)

A click lands on the DEEPEST hit, which is almost never the node you meant —
you click a Play sprite and you want its Button. Clicking the **same spot
again** steps one level UP the hit stack; past the top comes **nothing
selected**, and then it wraps to the deepest:

```
PlayLabel -> PlayButton -> Panel -> Screen -> (nothing) -> PlayLabel -> …
```

The "nothing" step exists because every click on the canvas lands on
*something*, so without it there was no way to empty the selection with the
mouse held still (#66601047). A click on genuinely empty canvas still just
clears and arms no cycle.

`hitStackAt` builds that stack deepest-first: the deepest hit, then each
ancestor whose own bounds also contain the point. An ancestor that does *not*
contain the point is skipped rather than ending the walk — a Hug container laid
out to zero on one axis would otherwise cut the stack off below nodes the user
can plainly see under the cursor.

Three decisions:

- **"Same spot" is a distance threshold and nothing else** (`clickCycleSlop`,
  4 px). There is no press-and-hold and no double-click timeout: clicking the
  same spot ten seconds later still continues the cycle. A few pixels of slop
  means a shaky hand keeps stepping instead of silently restarting.
- **The threshold is measured from the point that ARMED the cycle**, not from
  the previous click. Measuring from the previous click would let a hand
  drifting 4 px per click walk clear across the canvas without ever leaving
  the cycle.
- **A modified click never cycles.** Cmd/Shift/Alt are asking to change the
  set; stepping up the stack at the same time would make the result depend on
  how many times you had already clicked there. They take the deepest hit, as
  a first plain click does.

The stack is captured when the cycle arms and held on `InspectorSystem`, so a
relayout between clicks cannot renumber the steps under the user. `applySelect`
clears `cycleActive` on EVERY selection change and `canvasSelectAt` re-arms it
immediately for its own click — so the reset rule ("a tree click, an arrow key,
Esc or a modifier click ends the cycle") falls out of one assignment rather
than a list of cases to keep in sync.

Each step selects through the shared set, so the hierarchy marks the new active
row (`>`) and the inspector follows. Scrolling that row into view is the one
piece still missing, and belongs to the hierarchy's own expand/collapse +
scroll task.

### The hierarchy (#16096994)

The tree drives the SAME set with the same vocabulary, with one difference
that a row order makes possible: **Shift spans a contiguous RANGE** of visible
rows instead of adding a single node (`treeSelectMode` maps `add` → `range`;
everything else passes through unchanged). Arrow keys move the active node and
Shift+arrow spans to wherever it lands, so extending by keyboard and extending
by mouse are one operation over one anchor.

That range is why selection state is THREE fields and not two — collapsing
them is what makes Shift+click feel wrong in a hand-rolled tree:

| field | meaning | moved by |
|---|---|---|
| `selection` | the set | every gesture |
| `selected` | the ACTIVE node — what the inspector describes, where arrows start | every gesture |
| `rangeAnchor` | where a Shift-range spans FROM | plain / Cmd / Alt click, arrows — **not** Shift |

Because a Shift+click does not move the anchor, a second Shift+click re-spans
from the same origin rather than ratcheting the range outward, so a range that
overshot can be shrunk. The range also REPLACES the set rather than unioning
with it, which is the other half of being able to shrink.

Rows key on node identity, never row index: the row's action id carries the
ENTITY index, so the live tree (which rebuilds constantly) cannot hand a click
to whatever slid into that row. The marker column shows `>` for the active
node and `-` for the other selected rows — two glyphs, because with a range
selected "which of these is the arrow keys' origin" is not guessable from a
uniform highlight.

Selected nodes other than the anchor are outlined by an `OutlinePool` with
`matches: "selection"` (`chrome/SelectionOutlineItem`). The anchor keeps its
own single `OutlineFor { role: selection }` outline, so a selection of one
renders exactly as it did before multi-selection existed.

`RAE_UI_EDITOR_TEST_CLICKS` (canvas) and `RAE_UI_EDITOR_TEST_TREE_CLICKS`
(hierarchy rows) replay a script of clicks through that same gesture path once after the first layout — `"[mod:]<nodeId>"` clicks the middle
of a node's on-screen rect, `"[mod:]at:<x>,<y>"` clicks raw coordinates (how
you click empty canvas), and the run logs the resulting set. That is what the
example gate asserts; there is deliberately no back door that sets the
selection directly, since one would pass while the real gesture was broken.
