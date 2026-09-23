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
  toggles them; `RAE_UI_EDITOR_GUIDES=0` starts hidden. The safe-area guide shows
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

### Click the same spot again to go up (#73816378)

A click lands on the DEEPEST hit, which is almost never the node you meant —
you click a Play sprite and you want its Button. Clicking the **same spot
again** steps one level UP the hit stack, wrapping at the top:

```
PlayLabel -> PlayButton -> Panel -> Screen -> PlayLabel -> …
```

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
