# The `.raescene` format: scenes are modules, folders are part of their path (#1008)

This is the reference for how one `.raescene` refers to another and to the data
it draws with. The node/component grammar itself is documented with the
components (`lib/ui/Components.rae`, docs/ui-editor-design.md §5); the theme
token sections in docs/ui-theme-system.md.

## Units: design units, which are points

Every size, position, font size, padding, radius and offset in a scene is in **design units
(du)**, and a design unit is a logical point: the one coordinate model of every Rae program
(docs/ui-coordinate-and-responsive-layout.md, rules 1-6). A phone is about 393 wide (the
reference frame is 393x852), a margin 16, body text 17, a touch target 44. The layout fills the
window (the window in points, / the user zoom), so a scene's root lays out into the real window and
never scales as a whole. Origin top-left, y down. A scene declares no unit; one authored at
another scale is converted with `tools/scale-ui-lengths.py`.

A fixed art-directed area (a game table, a board, a splash) is a subtree at its own fixed size,
in any numbers, scaled as one block by `ScaleToFit` into its parent's box:

```json
"Table": { "Rect": { "x": 0, "y": 0, "w": 1080, "h": 2280 }, "ScaleToFit": { "mode": "FitHeight" } }
```

- `Contain` shows the whole area, letterboxed; `Cover` fills the box, cropped (the parent should
  clip); `FitHeight` always shows the full height, its sides cropped in a box narrower than the
  area's shape and pillarboxed in a wider one (a game table on any phone and on a desktop).
- The area and its descendants keep their authored Rects; each descendant is scaled with it
  (`fitSystem`, after layout). Case `1517_ui_scale_to_fit_height`.
- **Against the visible window, from inside the area.** A descendant with `StageEdge` is placed
  against the window (the area's parent box) instead of the area:

  ```json
  "Felt": { "Rect": { ... }, "StageEdge": { "bleed": true } },
  "Hud":  { "Rect": { "x": 40, "y": 40, "w": 1000, "h": 120 }, "StageEdge": { "top": "Safe" } },
  "Play": { "Rect": { "x": 340, "y": 2060, "w": 400, "h": 160 }, "StageEdge": { "bottom": "Safe" } }
  ```

  Each of `top`, `bottom`, `left`, `right` is `None` (the default), `Visible` or `Safe`. A pinned
  edge keeps the node's distance from that edge of the area, measured instead from the window's
  edge (`Visible`) or from the window's edge less the safe inset (`Safe`: the notch, the home
  bar); both edges of an axis pinned stretch the node between them. `bleed` makes the node cover
  the whole window (a background that fills the pillarbox bars or the cropped band).
- **A `SafeArea` node inside the area** is inset only where the window's unsafe bands (its safe
  insets on the sides `apply` asks for, plus `extra` in the area's units) overlap it, so the
  notch and the home bar keep their size on screen whatever the area's scale; its children keep
  their authored places from its new top-left. Case `1519_ui_stage_edges` (a 393x852 phone with
  59/34 insets and a 1440x900 window, the area under `FitHeight`).

A laid-out column whose content may be taller than it (a menu of a game screen, Pixi's
`scaleToFitY`) shrinks instead of overflowing:

```json
"Menu": { "Layout": { "type": "Vertical" }, "Overflow": { "mode": "ScaleToFitY", "minScale": 0.8 } }
```

- When the laid-out content is taller than the column's height inside its padding, the column is
  scaled uniformly about its top center so the content fits, not below `minScale` (0, the
  default: no limit); content that fits is left at 1. The scale is the column's TransformFx
  (pivot 0.5,0), which the policy then owns, so the column's own box and background scale with
  it. It is recomputed from the layout every frame (`fitSystem`), so it never compounds.
- `Overflow` is also written `OverflowPolicy`; `Clip` cuts the content at the node's box instead.
  Case `1518_ui_overflow_scale_to_fit_y`.

The sizes this document quotes in points (a 44-point `circleButton`, a 24-point swipe band,
`edgePoints`) are the same numbers in du. A new scene starts at the reference phone, 393x852
(the UI editor's default for a scene that gives no size). `compiler/tools/ui-model-gate.sh`, one
pre-suite case of every full run, fails on a scene that declares its own unit or design size and
on a program that sets its own design resolution outside its allowlist.

## 1. A scene reference is a path, without extension

A scene is a module and a folder is part of its path — like the Rae source rule
(AGENTS.md): scene files and ids are **PascalCase**, folders are **camelCase**,
and a reference is the path from the scene root with no extension:

    "import": ["Theme", "shared/GameAssets"]
    "SceneInstance": { "sceneId": "cards/HeroCard" }
    "ListView": { "itemSceneId": "rows/TrackRow" }

`CoverageCard` (today's bare id) is simply the zero-folder case. The extension
is a property of the file on disk, never of the reference — the format can be
renamed later and no scene changes. There is no top-level `"sceneId"`: the
root-relative package path is the identity. A legacy document that still
writes the field gets `sceneId is the path; remove the field`.

Binary data is the opposite: **a file path with its extension, relative to the
file that declares it** — `"../art/Play_Button.png"`,
`"../fonts/Body.mtsdf.json"`. A bare path is "another scene in the package
tree"; a path with an extension is "a file I load". (AGENTS.md draws the same
line for Rae code: modules are imported, shader/image assets are string paths.)

## 2. The scene root

Package paths resolve as `<root>/<path>.raescene`, from ONE root — the way
`import a/b/C` resolves from the project root, not from the importing file.
The root is an input, defaulted from what you opened, never discovered:

- the 121 viewer: the opened scene's directory, unless `--scene-root <dir>`
  (or `RAE_UI_EDITOR_ROOT`) says otherwise — mirroring `rae run`, whose
  project is the entry file's folder unless `--project` is given;
- an app that mounts scenes from Rae code passes the root explicitly to
  `mountSceneWithImports`:

      var gpuUi: Gpu2dUi = loadUiFonts(
        directory: "assets/"
        textFontName: "FallbackBody"
        iconFontName: "FallbackIcons"
      )
      var world: UiWorld = create()
      var canvas: Gpu2dCanvas = createGpu2dCanvas()
      var sceneRegistry: SceneRegistry = createSceneRegistry()
      let root: EntityId = mountSceneWithImports(
        world: world
        root: "assets/"
        sceneId: "screens/MainMenu"
        resources: gpuUi
        canvas: canvas
        registry: sceneRegistry
      )

  The mount resolves `assets/screens/MainMenu.raescene`, its imports and every
  reachable `SceneInstance` / `ListView` scene from that same root. It installs
  the imported theme before deserializing components, loads declared text and
  icon fonts with `loadSdfFontJson`, resolves mounted sprite/animation textures
  through `assets.textures`, and keeps `gpuUi`'s fonts as fallbacks when the
  package declares none. The app owns `sceneRegistry` so later list
  materialization can use the same registered sub-scenes.

No walking up for a `.raepack`, no magic files. A sub-scene therefore resolves
from the root, not from its parent scene's folder.

### Mounted roots: one package-path prefix, another folder

A host that shows a plugin's scenes keeps them in the plugin's own folder (a
download or install folder), while they import the host's theme. A
`SceneRoots` (`lib/ui/SceneRoots.rae`) maps package-path PREFIXES to folders;
every path no prefix claims stays in the default root:

    var roots: SceneRoots = createSceneRoots(defaultRoot: "assets/scenes/")
    mountSceneRoot(roots: roots, prefix: "apps/notes/", folder: "downloads/notes/")
    let root: EntityId = mountSceneWithRoots(
      world: world
      uiSystems: uiSystems
      roots: roots
      sceneId: "apps/notes/Main"
      resources: gpuUi
      canvas: canvas
      registry: sceneRegistry
    )

`apps/notes/Main` is `downloads/notes/Main.raescene`: the prefix is taken off,
the way a dependency's `import x/Foo` is `<depdir>/Foo.rae`. The plugin's
`"import": ["Theme"]` is `assets/scenes/Theme.raescene`, and its own
sub-scenes and ListView rows are written with the prefix
(`"sceneId": "apps/notes/Card"`). The longest matching prefix wins; a prefix
is whole folder names (`apps/notesOld/Main` is not under `apps/notes/`).
`sceneRootsFile` is the lookup that `import`, `SceneInstance` and ListView row
scenes all go through; `sceneRootsPackagePath` maps a file back to its
package path. `mountSceneWithImports(root: ...)` is the same with no mounts.
A page or a dialog over the screen is presented the same way with
`presentPageWithRoots(roots: ...)` (lib/app3d/UiShell); `presentPage(root: ...)`
is it with no mounts. Case 1470 presents a dialog that imports a mounted
`theme/Theme`.
The 121 viewer takes mounts as `--scene-mount <prefix>=<folder>` (repeatable)
or `RAE_UI_EDITOR_MOUNTS="apps/notes/=downloads/notes/;..."`. Case 1295 has two
roots.

## 3. `import`: configuration into scope, nothing mounted

    { "type": "Scene", "version": 2,
      "import": ["Theme", "shared/GameAssets"],
      "root": "Screen", "nodes": { ... } }

`import` brings the named scenes' **configuration** into scope — their
`assets` block and their theme token sections (`palette`, `space`, `radius`,
`padding`, `shadow`, `outline`, `text`, `container`) — and mounts nothing, the way a Rae
`import` makes declarations available without running anything. Mounting is
`SceneInstance`. There is no `open`: tokens and texture keys have no
qualified/bare distinction.

Resolution is **nearest-wins in list order**: the scene's own blocks first,
then each import depth-first (an imported scene may import too), first
declaration of a token / texture key / font wins. A text style may `extends` a
style from any imported scene; the chain is flattened after the full
nearest-first table is assembled. A cycle is skipped, the depth
is capped (8) with a diagnostic, a missing or unparsable import is a diagnostic
and the page still mounts. A `SceneInstance` child resolves through its own
file + imports, then the mounting page's resolved set.

A "config scene" is just a scene with no nodes (`version: 3`, no `root`): the
file name carries no meaning, `Theme` and `Assets` below are conventions of
the author, not of the loader.

## 4. `assets`: textures and fonts, allowed in any scene

    "assets": {
      "textures": {
        "dirs": ["../textures"],
        "map":  { "menu.playButton": "../art/Play_Button_x1024.png" }
      },
      "fonts": {
        "text":  "../fonts/ChelaOne-Regular.mtsdf.json"
      }
    }

- `map` is texture key → file; `dirs` is searched as `<dir>/<key>.png`.
  Lookup is `map`, then `dirs`, in resolution order (§3). Paths are relative
  to the declaring file.
- `mat:<name>` icons draw from the stdlib's Material Icons atlas
  (`lib/assets/fonts`, every icon of the font, ui/MaterialIconFont), so a
  scene declares `icons` only for another icon font. A `mat:` name the font
  does not have is an `unknownIcon` diagnostic at load (ui/IconCheck).
- A font names its `.mtsdf.json` sidecar; the atlas is the sibling `.png`
  (what msdf-atlas-gen writes) or `.raw` (the RGBA dump), whichever exists —
  `SdfText.loadSdfFontJson`. Fonts are world-level in v1: the first `fonts`
  declaration wins and a conflicting later one is a diagnostic (per-node fonts
  are a renderer change).
- Mounting many scenes costs no font memory: a mount whose theme names the
  font already in use keeps it (`SdfFont.jsonPath`), and the runtime returns
  the atlas it already holds for a loaded atlas path, so alternating two
  themes uses two atlases. The atlas table grows; it is never full. A font
  that does not load is logged with its path and is a `missingFont`
  diagnostic, and the font in use stays (case 1296).
- There is no manifest-of-Rae-source: a `.raescene` never parses `.rae`. A
  project whose texture table lives in Rae generates the `map` from it.

## 5. The two shapes, shipped as 121 samples

- **Self-contained** — `examples/121_ui_editor/assets/samples/SelfContained.raescene`:
  palette, tokens, `assets` (a texture `map` and the text font) inline, no
  `import`. One file you can hand to anyone.
- **Split by responsibility** — `samples/split/Page.raescene` with
  `"import": ["split/Theme", "split/Assets"]`: `split/Theme.raescene` holds
  tokens only, `split/Assets.raescene` textures (`map` + `dirs`) and fonts
  only. Opened with the samples directory as the root:

      rae run --project examples/121_ui_editor examples/121_ui_editor/Main.rae \
        -- examples/121_ui_editor/assets/samples/split/Page.raescene \
        --scene-root examples/121_ui_editor/assets/samples

The other samples (`MainMenu`, `Settings`, `CardStrip`, `Coverage`) import
`Theme`, whose `assets.textures.dirs: ["."]` names the samples folder.

## 6. What the viewer adds on top (overrides, never conventions)

`RAE_UI_EDITOR_ASSETS/<key>.png` (tried first), `RAE_UI_EDITOR_FONT` /
`RAE_UI_EDITOR_ICON_FONT` (a `.mtsdf.json`), `RAE_UI_EDITOR_ROOT` /
`--scene-root`, `RAE_UI_EDITOR_MOUNTS` / `--scene-mount`. They exist so a viewer can look at someone else's scene with
substitutes; a scene that needs them to render is incomplete.

## 7. Migration from the `theme.raescene` auto-load

Before #1008 the viewer loaded `<scene dir>/theme.raescene` by name. That is
gone: a page writes `"import": ["Theme"]` (and the file is `Theme.raescene`,
PascalCase like every scene). A page that declares no import and no tokens of
its own while a `Theme.raescene` / `theme.raescene` sits beside it gets a
diagnostic naming the line to add. Apps that load a theme explicitly
(`loadUiAssets(themeFile: "Theme.raescene")`) are unaffected — that was never
a convention, it is a path the app passes. New app code should use
`mountSceneWithImports`; `loadUiAssets` remains the compatibility path for apps
that have not yet migrated their scene packages.

### Renamed keys: `...Sec` is `...Seconds` (2026-11-13)

`Sec` is an abbreviation (AGENTS.md naming rules), so every key that ended in it now ends in
`Seconds`: `Carousel.autoAdvanceSeconds`, `Loop.periodSeconds` and `forSeconds`. A scene that still writes the old key
loads as before: the loader reads `fooSec` when `fooSeconds` is missing and adds a `renamedKey`
diagnostic naming the new key (lib/ui/RegistryGeneric `authoredKeyOf`; case 1398). The old
spelling will go once the scenes outside this repo are moved over.

## 8. `Camera2D`: zoom and pan a page by writing one component

A page or layer ROOT may carry `"Camera2D": { "zoom": 1, "offsetX": 0, "offsetY": 0 }`
(default identity, so the bare `{}` is a no-op). The lib `cameraSystem` (registered in
the ui Pipeline between `fit` and `transform`, `lib/ui/cameraSystem/`) derives the
root's `TransformFx` scale (about the origin) and `RuntimeOffset` from it every frame it
changes — the two transform-pass inputs a scroll already uses — and the stock transform,
render and input systems carry the inherited scale through every descendant (boxes,
glyphs, clips, masks, hit tests). So ANY app zooms or pans a page by writing one
component: the UI editor's wheel / middle-drag / zoom pill (its `cameraSystem/` is only
the input side), a map, a photo view. The camera owns the root's scale and pivot; the
root's own alpha / visible / rotation are kept. Compare-before-write: a still camera
bumps neither table, so an idle frame stays idle. `Coverage.raescene` authors one.


## 9. `bundles`: named component sets, applied by name

A **bundle** is a named set of components with values, declared once and
applied to any node — the `.raescene` answer to "these seven pills repeat the
same seven components". (The word is Bevy's; not *archetype*, which is a
storage term Rae's sparse tables would falsely promise.)

```jsonc
"bundles": {
  "pill": {
    "Rect":    {"x": 0, "y": 0, "w": 100, "h": 44},
    "Size":    {"w": {"mode": "Hug"}, "h": {"mode": "Fixed"}},
    "Layout":  {"type": "Horizontal", "gap": 0, "alignMain": "Center", "alignCross": "Center"},
    "Padding": {"l": 12, "t": 0, "r": 12, "b": 0},
    "HitArea": {"kind": "Rect"},
    "Shape":   {"kind": "RoundedRect", "fill": "editorButtonSecondary", "radius": 22},
    "Shadow":  {"blur": 12, "opacity": 0.5, "color": "editorButtonGlow", "hoverOnly": true}
  },
  "panelToggle": { "bundles": ["pill"], "PanelToggle": {} }   // a bundle may include bundles
}
...
"OpenPill": {
  "bundles": ["pill"],
  "Shape":   {"fill": "editorButtonPrimary"},                 // field-wise over the bundle's Shape
  "Shadow":  {"opacity": 0.22, "hoverOnly": false},
  "OnClick": {"actionId": "file.open", "label": "Open a scene"},
  "Children": ["OpenLabel"]
}
```

- **Where they come from.** The scene's own top-level `bundles` block, then
  the bundles of every scene in its `import` list — resolution order,
  first declaration wins, exactly like the theme tokens. A `Bundles.raescene`
  next to `Theme.raescene` is the shared form (`"import": ["Theme",
  "Bundles"]`). Hosts seed `world.bundles` with `sceneImportedBundles` before
  mounting, the way they seed the theme; 106 parses its `Bundles.raescene`
  once at boot and clones it onto every world.
- **Expansion at load, field-wise.** For a node listing `bundles`, each
  bundle in order — its own includes first, then its components, so a bundle
  overrides what it includes — then the node's own object for each component
  over it, top-level field by field. Later wins; the node wins over every
  bundle; a field the node does not mention keeps the bundle's value. The
  merge is done on the JSON before decoding, so one path serves lib
  components and an app's own alike: a component the lib does not know
  (`PanelToggle`) reaches the pending outbox with the merge applied.
- **Systems never see bundles.** After expansion the world holds ordinary
  components. The only trace is `BundleRefs { names, inherited, overridden }`
  (`ui/SceneBundles`), written by the loader for the round-trip: the bundles
  applied, the components the node inherited whole, the ones it authored a
  field over. Authoring `BundleRefs` by hand is rejected like any runtime
  table. The editor's inspector shows the line (`bundles: pill [inherited:
  Rect, Size …] overrides: Shape`). On save (`lib/ui/SceneWriter`) a bundled
  node is written as its `bundles` list plus its own component objects —
  the inherited ones are not expanded into the file.
- **Code-side.** `applyBundle(world, entityId, name)` runs the same expansion
  on a live entity (a debug menu can use the authored bundles instead of
  listing components), appending to its `BundleRefs`.
- **Diagnostics, never a hang.** An unknown name is `unknownBundle` (ignored);
  an include cycle or a chain deeper than 8 is `bundleCycle` (the cycle is
  skipped, the rest still applies).
- `ContainerStyle` (§ theme, a Shape + Padding bundle keyed by theme tokens)
  stays as it is; it becomes an entry of the theme's `bundles` in a later
  cleanup.

Fixture: `compiler/tests/cases/883_scene_bundles`. Proof in the tree: the
editor chrome's three pills are one `pill` bundle
(`examples/121_ui_editor/assets/scenes/Editor.raescene`), 106's Now Playing
transport buttons are `transportButton` / `transportButtonSmall`
(`examples/106_mobile_ui/assets/scenes/Bundles.raescene`) — both pixel-identical
to the hand-written nodes they replaced.

### Built-in bundles

lib/ui provides a few bundles itself (lib/ui/BuiltInBundles), so every scene
can use them without declaring them; they are looked up last, after the
scene's own and its imports', so a theme that declares the same name
replaces one. They carry shape and placement, never colors, and combine
with a theme's look by coming after it in the list:

| bundle | what it is |
|---|---|
| `circleButton` | a round button for an icon: a 44-point square box (`Size` fixed; a node's own `Rect` sets another size, keep it square), a `Circle` shape (fill and stroke from the bundles before it or the node) and its child centered. `"bundles": ["secondaryButton", "circleButton"]` is the theme's button drawn as a true circle. A tap anywhere in the square counts. |
| `menuPanel`, `menuRow`, `menuIcon`, `menuText`, `menuShortcut`, `menuSeparator`, `menuLine` | the parts of a menu (section 47) |
| `switch` | an on/off switch: a 52 by 32 point box (`Size` fixed; a node's own `Rect` sets another size), a `Switch` and an `OnClick`, so a tap reaches it (§18) |

## 10. `OnClick` labels: what a tap does, in words

Every action an `OnClick` fires carries a human label: `label` for
`actionId`, `labelDouble` for `actionIdDouble`, `labelTriple` for
`actionIdTriple`. Sentence case, saying what the tap does: `"Play"`,
`"Open settings"`, `"Next track"`.

```jsonc
"PlayCircle": {
  "OnClick": {"actionId": "playback.toggle", "label": "Play or pause"}
},
"ShadowBox": {
  "OnClick": {
    "actionId": "coverage.shadow", "label": "Shadow",
    "actionIdDouble": "coverage.shadow2", "labelDouble": "Shadow, double tap"
  }
}
```

- **When they are sent** is section 46: `actionId` at the first click, the others at the second and
  third of a series.
- **Who reads it.** A voice command and a screen reader say and match the
  label. A voice system (SUMU) derives every command from the labels; it
  never keeps its own phrase table. lib/ui has no voice code: the one query
  such a layer reads is `ui/ClickTargets.clickTargets(world, uiSystems)`,
  every action of every clickable node on the active screen (a node under an
  inactive parent is left out) with its node id, action id, gesture
  (`single` / `double` / `triple`) and label.
- **The loader checks it.** An `OnClick` that sets an action id without its
  label is a `missingLabel` scene diagnostic, naming the node and the id.
  An empty action id needs no label.
- **Set at mount and by data, like the id.** A scene-instance override and a
  `ListView` binding set `label` (or `labelDouble`, `labelTriple`) the way
  they set `actionId`: `{"nodeId": "Label", "component": "OnClick", "field":
  "label", "value": "Home"}`, or a row binding `{"nodeId": "TrackRow",
  "component": "OnClick", "field": "label", "itemField": "title", "prefix":
  "Play "}`. A row's label says which row it is ("Play Midnight City").
- **121** shows and edits the label in the inspector's `OnClick` widget,
  generated from the component's fields like every other one.

**Why the label is on `OnClick`, not on an app's action-table row.** An app
such as 106 also maps every action id to a row of its action table (what the
id does). The label still belongs to the scene node, for three reasons:

1. **The voice system needs it from the scene.** The command is derived
   from what is on the screen, and the scene is the one place every app has;
   an action table is one app's own design, not something lib/ui or a voice
   layer can read.
2. **One id, several buttons, several words.** The same id is fired from
   different places with different wording: 106's dock tab, its icon and
   its label all fire `nav.home` (one "Home"); a track list fires each
   row's id from one row template, and each row is labeled with its own
   title; the walkers' settings fire `settings.weather.auto` and
   `settings.wind.auto`, both shown as "Auto", which only the labels tell
   apart ("Automatic weather", "Automatic wind"). A label per id would be
   wrong for the rows and ambiguous for the buttons.
3. **It sits next to what the user sees.** A label is the spoken twin of the
   button's visible text and changes with it; keeping it on the node keeps
   the two edited together, in the scene and in 121.

An action table may still describe an action for the app's own purposes
(logs, menus); 106's action audit checks both that every id has a table row
and that every `OnClick` id has a label.

## 11. `TextField`: a single-line text field

    "NewTask": {
      "TextField": { "placeholder": "New task", "actionId": "todo.add" },
      "OnClick": { "label": "New task" },
      "Children": ["NewTaskLine"]
    },
    "NewTaskLine": { "Text": { "text": "" } }

- The field's first Text child is its visible line: the text in
  `textStyleId` (default `primary`), or the `placeholder` in
  `placeholderStyleId` (default `muted`) while it is empty and has no focus.
  The `OnClick` makes the node tappable; it needs no action id.
- `lib/ui/textFieldSystem` runs it: a tap on the field gives it focus
  (`focused`, data on the component, at most one field), a tap elsewhere or
  Return takes it away; typing and Backspace edit the focused field; Return
  sends `actionId` as a UiAction with no entity, as a button with that id
  does. `textFieldTextFor` reads the text, `clearTextFieldsFor` empties it.
- OS text input is on only while a field has focus (on a phone that is what
  shows the keyboard), and `uiSystems.textField.keyboardHeight` is the
  keyboard's height in points so a layout can keep the field above it.
- Cmd+V (Ctrl+V) pastes the clipboard's text at the caret (the field's end;
  line breaks dropped, as the field is one line). Cmd+C copies and Cmd+X cuts
  the field's whole text. `readTextInputFrame` reads those keys into the
  frame's `paste` / `copy` / `cut`. The clipboard is the system's
  (`Gpu2dTextInput.clipboardText` / `setClipboardText`, SDL3 over
  NSPasteboard and UIPasteboard; never touched under a headless run). A test
  or self-test calls `setTextFieldClipboard(textFieldSystem:, text:)`, which
  makes the clipboard a scripted one holding `text`, then sends a frame with
  `paste: true` (case 1309).
- On iOS a press held still (0.5 s, within 10 units) on the focused field
  shows the system edit menu there, as a UITextField does:
  `textFieldMenuSystem(world:, uiSystems:, pointer:, now:)`
  (ui/textFieldMenuSystem), run after `textFieldSystem` with the frame's
  pointer (the app's `UiInput.pointer`), detects the press and calls
  `Gpu2dTextInput.showEditMenu` (a
  UIEditMenuInteraction, iOS 16+) with Paste when the clipboard has text and
  Copy and Cut when the field has some. The item picked is polled by the next
  `readTextInputFrame` (`Gpu2dTextInput.editMenuChoice`) and arrives as that
  frame's `paste` / `copy` / `cut`. Elsewhere the menu is a no-op (case 1311
  checks when it is shown). A double tap does not open it.
- The caret is drawn by the renderer after the text, not written into it;
  it blinks every 0.53 s from the last edit, and `textFieldDeadlineSeconds` is
  when it next changes, for the event loop's heartbeat
  (`earliestDeadlineSeconds` combines it with another deadline).

## 12. `ListReorder`: drag to reorder a list's rows

A `ListView` with `"animateMoves": true` and an `itemKeyField` animates its
rows when the data's order changes: each row glides from where it was drawn
to its new place (section 23).

A virtual `ListView` (the default mode) whose node also carries
`ListReorder` lets a person drag its rows into another order
(lib/ui/ListReorder, the system is ui/ListReorderSystem):

```json
"Tasks": {
  "ScrollRoot": { "axis": "y", "floor": "content" },
  "ListView": { "itemSceneId": "scenes/TaskRow", "itemHeight": 52, "itemGap": 6, ... },
  "ListReorder": { "actionId": "tasks.move", "label": "Move task" }
}
```

| field | default | what it does |
|---|---|---|
| `actionId` | (required) | the UI action a drop fires: `<actionId>.<from>.<to>`, the moved row's index in the list's data and the index it moves to (a template action, so an action table row `tasks.move` takes `2.5` as its payload); nothing is fired when the row is put back where it was |
| `label` | `""` | what the gesture does, in words, sentence case, as `OnClick.label` |
| `enabled` | `true` | `false` turns the gesture off |
| `holdMs` | `350` | how long a press must stay still to lift the row |
| `handleNodeId` | `""` | a node of the row scene (a grip): a press on it lifts the row at once, a press anywhere else never does |
| `edgePoints` | `64` | the band at each end of the scroll window where a held row scrolls the list, faster closer to the edge |
| `liftScale` | `1.03` | the lifted row's size, as a multiple of its own |
| `liftShadow` | `true` | a soft drop shadow under the lifted row, growing with the lift (a row root with a `Shadow` of its own keeps it) |
| `haptics` | `true` | the phone's ticks (lib/Haptics): a light impact when the row lifts and a selection tick each time the gap moves to another index; the generators are warmed at the press, so neither lags. Nothing to feel off iOS; a headless run logs `[haptics] ...` lines instead |
| `fixedField` | `""` | the name of a cell of the list's rows (`"isHeading"`): a row whose cell is `"true"` stays where it is, as a section's heading. A press on it never lifts it, and a lifted row moves only among the rows between the fixed rows around it, so no fixed row moves and the drop's indexes never cross one |

The gesture is the one phones use. A press that stays still for `holdMs`
lifts the row: it grows to `liftScale`, casts a shadow, is painted over its
neighbors (`PaintOnTop`), and follows the finger, while the rows between its old and
its new place step aside to open a gap. The press is the row's from then on:
its release clicks nothing and the scroll root does not follow the finger. A
press that moves before the hold is a scroll or a tap, as without
`ListReorder`. On the drop the app reorders its data from the action; every
row then eases from where it was drawn into its new place. An app that does
not take the move (its data does not change within half a second) sees the
rows ease back into the old order. With an `itemKeyField` the new order is
taken by key, so an answer that also adds or removes a row (a heading that
goes once its last task moved out from under it) still moves every row
smoothly from where it was drawn.

The app runs `listReorderSystem(world, uiSystems, input, nowSeconds)` each frame
`listReorderNeedsRun` says so, after `updateUiInput` and before
`updateScrollRoot`, and `listReorderPoseSystem(world, uiSystems)` after the
list system and before the layout. The first steps the gesture and fires the
drop's action; the second puts the rows' offsets on the rows once the list has
bound them to this frame's data, so the frame in which the app's new order
arrives (the app takes the action in its update, the same frame) draws every
row where it was, with no jump. `listReorderSystem` returns true while a row
is held or rows still move, so an event-driven loop keeps drawing frames until
they are home, and then runs nothing. Rows are moved by their `RuntimeOffset`
(no relayout). Flow lists (`mode: "Flow"`) are not
reorderable yet.

## 13. Dialogs are pages: `PageRoot.presentation`

A dialog is a page scene whose root says how it is shown:

```json
"Card": {
  "PageRoot": { "id": "removeTask", "presentation": "dialog" },
  "DialogOptions": { "maxWidth": 340, "dismissActionId": "dialog.dismiss" },
  "bundles": [ "dialogCard" ],
  "Layout": { "type": "Vertical", "gap": "spaceL" },
  "Children": [ "Title", "Message", "Buttons" ]
}
```

`presentation` is `"page"` (the default) or `"dialog"`; it is the one field
that turns a dialog into a page and back. An app shows either the same way:
`presentPage(world, uiSystems, root, sceneId, resources, canvas, registry,
overrides)` (lib/app3d/UiShell) mounts the scene into the current world, its
node ids scoped to the page (`sceneId`, so they never clash with the
screen's), and presents it on the "dialog" layer (ui/Layer; made when the
app has none). `overrides` are `SceneOverride`s (as a `SceneInstance`'s): the
text of a message, the action of a button. It returns the host, the handle
`closePresentedPage` takes.

- **dialog**: a scrim dims the whole screen and takes every tap outside the
  card (it fires `DialogOptions.dismissActionId`, `"dialog.dismiss"` by
  default; `""` for none). The card is the page's root, in the middle of the
  screen; with `"placement": "bottom"`, at its bottom (`margin` above the
  safe area); or with `"placement": "at", "at": 0.67`, its middle at that
  fraction of the screen's height (two thirds down: where a thumb reaches
  well, and lower down on a taller phone), kept inside the safe area and the
  margins. It is as wide as `maxWidth` (360) allows less `margin` (24) on
  each side, and as tall as its content. Its look is the scene's own (a
  bundle such as a card's Shape). An `at` card is moved after layout by
  `dialogPlaceSystem(world, uiSystems)` (presentPage runs it; an app runs it
  after its own layout pass, so a resized window keeps it in place).
- **page**: the page's root fills the screen, over everything, with no scrim.

Either way it is modal: taps reach the topmost layer first, and
`presentedPageBlocks(world, uiSystems, entity)` tells pointer systems to
leave what is under it alone (the scroll root and ListReorder already do).
`dialogSystem(world, uiSystems)`, run on a frame with UI actions after the
app has read them, closes a dialog whose own button fired one
(`DialogOptions.closeOnAction`, on by default: an alert's buttons answer
it); a page stays until the app closes it. `DialogOptions` is optional and a
page ignores it.

## 14. `RadialGradientFill`: a round glow

A soft round light (a backdrop's aura, the highlight on an orb) is one
`RadialGradientFill`, not a centered `Shadow` on a clear circle. A Shadow is
painted as stacked layers, and at 3x on an iPhone each layer shows as a ring;
the radial gradient is painted per pixel by the gpu2d box shader (its radial
mode, `Gpu2dCanvasRadial`), with a dither of half an 8-bit step against
banding.

```json
"Aura": {
  "Shape": {"kind": "Circle", "fill": "#00000000"},
  "RadialGradientFill": {"from": "glow", "to": "#00000000", "stop": 0.62}
},
"Orb": {
  "Shape": {"kind": "Circle", "fill": "accent"},
  "RadialGradientFill": {"from": "#FFFFFF", "to": "accent",
                         "centerX": 0.35, "centerY": 0.3, "radius": 0.9}
}
```

- `from` is the color at the center, `to` the color from `stop` outward.
  Either may be a hex color, an RGBA object or a palette name; `fromSlot` /
  `toSlot` name a palette color directly, and a name that is no palette
  color is an `unknownToken` diagnostic (that end paints magenta), as for
  `GradientFill`.
- `centerX` / `centerY` (0.5 / 0.5): the center as a fraction of the box.
- `radius` (1): a fraction of the distance from the center to the box's
  farthest corner, CSS's `circle farthest-corner`.
- `stop` (1): the share of the radius where the color reaches `to`. So
  CSS's `radial-gradient(circle, color 0%, transparent 62%)` is
  `{"from": color, "to": "#00000000", "stop": 0.62}`.
- Like `GradientFill` it replaces the node's Shape fill, rounds with the
  Shape's radius (a Circle: half its smaller side), else a `CornerRadius`,
  and the Shape's stroke and the node's Shadow are painted with it (shadow,
  fill, stroke). With a `GradientFill` on the same node the linear gradient
  paints first and the radial one over it.
- Case 1314 checks what is queued; the 121 Coverage sample has an orb light
  (FadedBox).

## 15. `ScrollRoot.anchor`: a list that fills from the bottom

A UI that puts every control at the bottom of the screen for one-thumb use,
or a chat, wants its list to start at the bottom too, next to those
controls. `"anchor": "end"` on the `ScrollRoot` (ui/ScrollAnchor; the
default is `"start"`, the top):

```json
"Tasks": {
  "ListView": { "itemHeight": 56, "visibleItemCount": 10, ... },
  "ScrollRoot": { "axis": "y", "anchor": "end" }
}
```

- Content shorter than the window sits at the window's end: its last row
  ends where the window ends (less `endInset`). The position is the room
  left over (`ScrollRange.rest`, positive), applied like any scroll as the
  root's `RuntimeOffset`, so a virtual `ListView` windows its rows from it
  as usual.
- A longer list opens scrolled to its end (a root mounted with no
  remembered position), with the window on its last rows.
- While its end is in view the root holds it (`ScrollDrag.pinnedToEnd`): a
  row added at the end keeps the end where it was, as a chat does. A drag or
  the wheel lets go of it, so a row added while the list is scrolled back
  does not move it; coming to rest at the end (a drag or fling that ends
  there) holds it again.
- Rubber-banding and flings are the same as at the start. This is
  `updateScrollRoot`'s physics; an app with its own scroll state machine
  reads `scrollRootRange(...).rest` for where a short list rests.
- On the frame a list is mounted it is not laid out yet, so it shows at its
  start for that one frame; `scrollRootSettling` stays true until it has
  moved to its end, so an idle event loop renders the next frame.

Case 1315 checks a short, a long and a growing list, scrolling away and
back, and the start anchor.

### Scrolling a list to a row

An app asks for a row of a virtual `ListView` in view with
`requestScrollToRow(world, listEntity, index, place)` (lib/ui/ScrollToRow,
ui/ScrollRootSystem/ScrollToRowSystem): the list's scroll root (its own,
else its nearest ancestor's) scrolls so data row `index` is wholly in view
at the window's `end` (the bottom: a list read from the bottom, near the
thumb) or `start`, kept within the root's range; an index past the end is
the last row, and a list that does not overflow its window does not move.
The request waits in the `scrollToRows` table until
`scrollToRowSystem(world, uiSystems)` takes it, which the app runs each
frame before the list system (so the rows bound are the ones now in view);
it reads the last layout, so a list not laid out yet keeps its request
until it is. It returns true when a root moved (the frame must draw).

## 16. `SafeArea`, and a band the host keeps for itself

A node with `SafeArea` lays its content out inside the screen's safe area:
the notch, the status bar and the home indicator are added to its padding.
`apply` says which sides take part (1 a side, 0 not; all four by default;
a side written as `true` / `false` is a `wrongValueKind` diagnostic and
counts as 0, so write numbers) and `extra` adds a fixed inset of its own:

```json
"Screen": { "SafeArea": {}, "Layout": { "type": "Vertical" }, "Children": [...] }
```

The insets are the OS's, or a simulator's or a layout test's
(`setSimulatedSafeInsets(world:, insets:)`), fed each frame by
`feedSafeArea` (lib/ui/safeAreaSystem) before the layout.

A host that draws its own bar along the top of every screen (a date, the
time, the weather, the battery), in its own UI world, keeps that band for
itself the same way: `setReservedInsets(world:, insets:)` on each screen's
world, in that world's layout units (points for a world laid out in
points). The reserved insets are added to the screen's, so every SafeArea
node lays out inside both, as it stays out of the notch:

```rae
setReservedInsets(world: screenWorld, insets: Insets { l: 0.0, t: 48.0, r: 0.0, b: 0.0 })
```

Zero insets give the band back; `reservedInsets(world:)` reads them. The
safe-area step writes a node's insets only when they change, and the
scheduled layout reads them (the layout step's input), so a new band, a
rotation or a new notch lays the screen out again and an idle frame does
not. Case 1322 checks a header moving down by the notch and then by the
band, and back.

## 17. `ValueSlider` and `ValueText`: widgets bound to the app's numbers

A slider or a readout shows one named number of a store the app owns
(`UiValues`, lib/ui/UiValues), and the number lives only there. Whatever
writes it (a button's handler, the app's own model, a drag) the widgets
bound to its key show it on the next frame; nothing else keeps them in
step. Their system is `valueSystem(world, uiSystems, input, values)`
(lib/ui/valueSystem), run once a frame after `updateUiInput`, before the
layout; it returns true when it changed a number or what a widget shows.

```json
"CloudLabel": { "Text": { "text": "Clouds" },
                "ValueText": { "key": "weather.cloud", "prefix": "Clouds ", "suffix": "%", "scale": 100 } },
"CloudTrack": { "Shape": { ... }, "Children": ["CloudFill"],
                "ValueSlider": { "key": "weather.cloud", "min": 0, "max": 1, "actionId": "weather.slider" } },
"CloudFill":  { "Rect": { "x": 0, "y": 0, "w": 0, "h": 24 }, "Shape": { ... } }
```

- `ValueSlider` is on the track; its FIRST child is the fill, as wide as
  the number's share of `[min, max]` of the track's width (written only
  when it moves). A press on the track jumps the number there and a drag
  moves it, also past the track's ends, until the release. Each change by
  hand sends the UI action `actionId` (when set) with the track as its
  entity, so the app learns that the number is now set by hand.
- `ValueText` writes its node's text: `prefix`, the number times `scale`
  with `decimals` decimals, `suffix` (0.7, scale 100, `"%"`: `70%`).
- The app writes a number with `setUiValue(values:, key:, value:)` and
  reads it with `uiValue(values:, key:, fallback:)`.

The walkers' settings panel binds its weather sliders this way (Clouds,
Rain, Snow, Mist, Wind; app3d/SettingsWeather): the weather buttons write
their numbers, the weather model writes its own on Auto, and the drawn
weather is read back from them. Cases 1327 (the widgets) and 1328 (the
weather's numbers).

## 18. `Switch`: an on/off switch

```json
"Sound": {
  "bundles": ["switch"],
  "Switch": { "on": true, "actionId": "settings.sound" },
  "OnClick": { "label": "Sound" }
}
```

A built-in that carries its own state: the node draws a pill and a round knob, with no child
nodes to show and hide. `Switch` has these fields:

| field | default | what it is |
|---|---|---|
| `on` | `false` | its state |
| `actionId` | `""` | the action a flip sends, and an action that flips it |
| `onColor` | `"accent"` | the theme slot of the track when on |
| `offColor` | `"surfaceAlt"` | the theme slot of the track when off |
| `knobColor` | `"textPrimary"` | the theme slot of the knob |

- A tap on the node flips it and sends `actionId` as a `UiAction` with no entity. The `switch`
  bundle brings the `OnClick` that lets a tap reach it; a node's own adds its label.
- An action with that id from elsewhere flips it too, so a whole row can carry the `OnClick` with
  the same `actionId` and a tap anywhere in the row counts. The action is then not sent again.
- The app calls `switchSystem(world:, actions:, deltaTime:)` (`ui/switchSystem/SwitchSystem`) each
  frame, after the input system and before it reads the frame's actions. It answers true while
  anything changed or moved: **feed that into the frame loop's "is animating" flag**, or the knob
  stops between events (`ui/EventLoop`).
- `switchIsOn(world:, entityId:)` reads the state and `setSwitch(world:, entityId:, on:)` sets it.
  An override sets it too, with `Switch` `on` and the value `"true"` or `"false"`; another field,
  or a node with no `Switch`, is an `unknownOverride` diagnostic.
- The knob glides in 0.18 s, and turns round from where it is when flipped halfway. A screen opens
  with each knob at its side, nothing moving. The position is the runtime table `SwitchMotion`,
  which a scene does not author.

## 19. `Shape.strokeFixed`: a stroke that does not zoom

`strokeWidth`, `strokeDash` and `strokeGap` scale with a `TransformFx` or a camera above the node,
like everything else it paints. `"strokeFixed": true` keeps them that wide on screen at any zoom, like
SVG's non-scaling-stroke: for a guide inside an editor's camera, which would nearly vanish at a low
zoom. The default is false. Case 1364 checks it; the UI editor's frame and safe-area guides use it.

## 20. `motion`: named springs and curves

A theme section, parsed like `palette` and `space`, imported the same way and redeclared by name
(the nearest declaration wins):

```json
"motion": {
  "quick":  { "kind": "spring", "response": 0.24, "damping": 1.0 },
  "fadeIn": { "kind": "curve", "curve": "easeOutCubic", "seconds": 0.20 }
}
```

A spring has `response`, the seconds to get about there, and `damping`, where 1.0 has no bounce and
lower bounces (lib/Spring). A curve has a name from lib/Easing and a length. `quick`, `smooth`,
`lively`, `fadeIn` and `fadeOut` are built in, so a scene with no theme still has them. A wrong `kind`
or `curve` in a theme is logged and that token is not added. A use of a name no one declares is an
`unknownToken` diagnostic and the motion runs as `smooth`. docs/ui-animation-design.md section 3.

## 21. `Appear`, `Disappear`, `Stagger`: how a node arrives and leaves

```json
"Row": { "bundles": ["appearRise", "disappearFade"], "Appear": { "y": 40 } }
```

| component | fields, with their defaults |
| --- | --- |
| `Appear` | `motion` lively, `opacityMotion` fadeIn, `opacity` 0, `scale` 1, `x` 0, `y` 16, `rotation` 0, `delaySeconds` 0 |
| `Disappear` | `motion` quick, `opacityMotion` fadeOut, `opacity` 0, `scale` 0.96, `x` 0, `y` 0, `rotation` 0 |
| `Stagger` | `stepSeconds` 0.028, `maxSeconds` 0.22, `appearFrom` end, `disappearFrom` start; a from is `start`, `end`, `center` or `none` |

- **`Appear`** says where a node starts from; it ends at its place. It plays on mount and whenever the node
  comes to be shown, by its own `Active` or by a parent's. Under a parent that is shut it waits. The start pose
  is on before the first paint.
- **`Disappear`** says where a node ends. It plays through `setActiveAnimated(world, entity, active)` and
  `destroyAnimated(world, uiSystems, entity)`, which an override of `Active`, `setNodeActive` and `bindNodeActive`
  go through. With no `Disappear` they act at once. With one the node stays laid out and drawn and takes no hit
  and no focus, its subtree too, until its track ends. Showing it again mid-way turns it around from where it is.
  A plain `componentSet` of `Active` still acts at once.
- **`Stagger`** on a parent starts its children's `Appear` and `Disappear` one after another. `maxSeconds` caps the
  total: the step shrinks to fit. Only children overlapping the parent's box count; one outside starts at once. A
  leaving parent waits for its children.
- A motion no one declares is an `unknownToken` diagnostic at load, and the default for that field runs.

The built-in bundles: `appearFade`, `appearRise`, `appearPop`, `appearDrop`, `disappearFade`, `disappearShrink`,
`staggerFromEnd`, `staggerFromStart`, `pressable`, `dialogMotion`. Case 1368 holds the rules; `checks/uiAppear` paints them.

### `Loop`: a motion that repeats for a while, then rests

```json
"Drop1": {
  "Loop": {
    "keys": [
      { "t": 0.0, "y": -10, "opacity": 0.0 },
      { "t": 0.3, "y": -4, "opacity": 1.0 },
      { "t": 1.0, "y": 10, "opacity": 0.0 }
    ],
    "periodSeconds": 0.9, "forSeconds": 4.0, "delaySeconds": 0.15
  }
}
```

| field | default | meaning |
| --- | --- | --- |
| `keys` | none | keyframes over one period: `t` (0 to 1 of the period) and the pose's offsets `x`, `y` (points), `scale`, `rotation` (radians), `opacity`; a field a key does not write is the laid-out pose's (0, 0, 1, 0, 1). Read in `t` order; between keys the pose moves in a straight line |
| `periodSeconds` | 1 | seconds per period |
| `forSeconds` | 0 | how long it runs; 0: as long as the node is shown |
| `delaySeconds` | 0 | it starts this much later (the first key's pose meanwhile): siblings with different delays run out of phase |
| `motion` | smooth | the glide home at the end |

- It starts when the node is shown, as `Appear` does: on mount, and when it or a parent comes to be shown
  again (which starts it over). Its first key's pose is on before the first paint.
- When `forSeconds` has passed it glides from where it is to its laid-out place on `motion`, and stops: no
  code runs and nothing is drawn for it any more. A node hidden while it runs stops where it is.
- While it runs its pose is the loop's, over any other motion's on the node.
- With reduced motion (or `RAE_ANIMATION=off`) it does not run: the node stays at its place.
- A running loop keeps the frame loop awake (`animationInFlight`); one that has ended costs nothing.
- Case 1398 holds the rules.

## 22. `AnimateLayout` and `AnimateText`: moves and text changes

```json
"Row": { "bundles": ["glideOnChange"] },
"Time": { "Text": { "text": "09:59" }, "AnimateText": { "kind": "roll", "rollFrom": "below", "motion": "smooth" } }
```

| Component | Fields and their defaults |
|---|---|
| `AnimateLayout` | `motion` smooth |
| `AnimateText` | `kind` fade (or roll), `rollFrom` below (or above, or auto), `motion` quick |

- **`AnimateLayout`**: when layout moves the node, it glides from its old place to the new one. Layout runs once and
  gets the true places, and later frames run no layout. Only position glides: size changes at once. There is no
  glide on a first layout, on a window resize, on a scroll, or for a node that was hidden. The bundle
  `glideOnChange` is this component with its default motion. The on-screen keyboard is not a window resize: a host
  that lays the page out shorter for it calls `layoutKeyboardMoves(world, Gpu2d.keyboardSeconds())` first, and the
  nodes glide with the keyboard on the `keyboard` token (case 1385).
- **`AnimateText`**: when the node's text changes, the old text goes as the new one comes. `fade` crosses the two.
  `roll` also moves them, with the new text coming from `rollFrom`. `auto` comes from below when the new text sorts
  after the old one, each run of digits read as a number ("10:45" after "9:30"), and from above when it sorts before:
  a time that can go both ways. A roll is drawn inside the node's box and the nearest shape it sits on (itself or
  an ancestor), with that shape's round corners when the text fills it, as a label that fills a pill does. The first text a node
  shows plays nothing. The old text is only drawn, so the node's measured size never depends on it.
- A motion no one declares is an `unknownToken` diagnostic at load.

Case 1369 holds the rules; `checks/uiLayoutText` paints them.

## 23. List motion

```json
"Tasks": { "bundles": ["staggerFromStart"], "ListView": { "itemKeyField": "id", "itemSceneId": "TaskRow", "animateMoves": true } },
"TaskRow": { "bundles": ["appearRise", "disappearFade"] }
```

A list takes part when it has `animateMoves`, or when the root of its row scene has `Appear` or `Disappear`. Rows need an
`itemKeyField`: without one a list plays its first fill only, and a log line says so once. Any other list is not touched. The
host needs nothing but `animationRun`, which is already in the pipeline.

| What happened | What the rows do |
|---|---|
| First fill | The rows in the window play the row root's `Appear`, in the list's `Stagger` order |
| A row comes into the window by scrolling | Nothing |
| The same data again | Nothing |
| A new key | Its row plays `Appear`, and with `animateMoves` the rows around it glide |
| A key is gone, its row has `Disappear` | Its row plays it where it was, and takes no hits; with `animateMoves` the rows around it glide |
| A key moved, with `animateMoves` | The row glides |
| A row is held by drag | All motion stops, and the new order is only noted |

`listRowsChange(world, listEntityId, direction)` said together with new data (`ListChangeDirection.forward` or `back`) sends every
old row out to one side as the new rows arrive from the other, slightly staggered. It works on any list, with or without keys.

In `Flow` mode a row whose key went stays in the layout until its `Disappear` ends, then the rows around it glide into the gap, and
a key that comes back meanwhile turns its row around.

Case 1370 holds the rules; `checks/uiLists` paints them.

## 24. Dialog motion

A dialog animates with no change to its scene. The scrim fades. The card plays its root's `Appear` and `Disappear`; a root with neither
gets the bundle `dialogMotion`. The host needs nothing but `animationRun` and `releaseDeadEntities`.

- `closePresentedPage` closes the page for the app at once: it is out of `presentedPageCount`, its node ids are gone, the screen under
  it takes input, and a new page can open, even with the same page id. What is on screen stays, taking no input, until its motion ends.
- A dialog with another dialog over it steps back to scale 0.97, and comes back when the top one closes.
- `DialogOptions.placement` holds at every scale: the card scales about its middle, which is where placement put it.
- A page with `"presentation": "page"` follows its root's `PageTransition` (section 25), a `sheet` by default. Only the page moves, and
  it closes by going back out the way it came. A host that runs no animation shows it in place and closes it at once.

Case 1371 holds the rules; `checks/uiDialogs` paints them.

## 25. `PageTransition`: how a page comes and goes

```json
"Page": { "PageTransition": { "kind": "push" } }
```

On a page's root. `kind` is `push`, `sibling`, `zoom`, `sheet`, `fade` (the default) or `none`, and `motion` names its motion (`smooth`
by default, `livelySoft` for a zoom). Going forward the arriving page's says how; going back the leaving page's does. The
page's nodes need nothing more: the leaving page's `Disappear` start within the first 40% of the transition, its opacity is zero by
60% in every kind, and the arriving page's `Appear` start at 25%. Taps go to the arriving page from its first frame, where it is drawn.

A host has one of two page models (`lib/ui/PageTransitionSystem`), with a `PageTransitionSystem` from `createPageTransitionSystem()`:

- **Resident pages in one world:** `pageTransitionResident(system, world, uiSystems, fromRoot, toRoot, back)`. The leaving page deactivates at the end.
- **A new world per screen:** `pageTransitionBegin(system, own oldWorld, own oldUiSystems, root, back)`, build the new world and lay it out as
  always, then `pageTransitionArrive(system, world, uiSystems, root)`. Each frame, `pageTransitionStep(system)` runs the old world, and
  `pageTransitionDraw(system, world, uiSystems, res, canvas)` replaces `renderSystemGpu2d`. `pageTransitionInFlight` goes in the host's `animating`.

For a `zoom`, say the rectangle first with `pageTransitionSetSource`; Back shrinks into the same one.

A second navigation during a transition drops the page still leaving, and the current page leaves from its current pose. Going back along
the same path is the same transition run backward.

Case 1372 holds the rules; `checks/uiPageTransitions` paints them.

## 26. `SharedElement`: what stays across a page transition

```json
"Bar": { "SharedElement": { "key": "bottomBar", "mode": "stay" } },
"Cover": { "SharedElement": { "key": "cover", "mode": "morph", "motion": "smooth" } }
```

A node on each page with one `key`, in either page model, and across worlds. `mode` is `morph` by default.

- **`stay`**: on both pages at one place and size, it does not take part. The new one shows from the first frame, the old one is hidden, and it does not
  move or fade with its page. It is drawn over the page, which moves under it. At another place it just goes with its page.
- **`morph`**: the new node moves and resizes from the old one's rectangle with a spring, with no layout, over its page and outside its clips. It can be any
  node, with its children. Its corner radius goes from the old one's to its own. A source off screen makes no morph.

`HeroWidget { key }` is another way to write a `morph`. A host with two pages in one world and no page transition starts one with `spawnHeroTransition(world, uiSystems, key, source, target)`.

**Inside one page** a `morph` also moves a marker: when a node with the key hides and another with the same key shows in the same
frame, on the same page, the one that shows comes from where the other was drawn. A chosen day's fill in a week row, bound to the
list's data, glides to the newly chosen day:

```json
"ChosenFill": { "Shape": { "kind": "RoundedRect", "radius": 14 }, "SharedElement": { "key": "chosenDay" } }
```

It is drawn under the list or row that holds both places, so it passes under the labels. A key that only shows plays nothing.

Case 1373 holds the rules across pages and 1382 inside one; `checks/uiPageTransitions` and `checks/uiLayoutText` paint them.

## 27. A swipe from the left edge

A scene needs nothing: this is a host's Back. Each frame, after the input step and before the scroll root:

```
let event: EdgeSwipeEvent = edgeSwipeStep(world: world, input: input, deltaTime: deltaTime)
```

On `started`, call `pageTransitionDragStart(system)`, then go back as usual (`pageTransitionResident`, or `pageTransitionBegin` and `pageTransitionArrive`). On `moved`, call
`pageTransitionDrag(system, world, uiSystems, progress, velocity)`. On `released`, call it once more, then `pageTransitionDragEnd(system, world, uiSystems)`, which says whether it finishes or
springs back, from the finger's position and speed. With two worlds, also call `pageTransitionTakeBack(system, world, uiSystems)` each frame after `pageTransitionStep`: when it says true, the
old world is the host's again.

A press within 24 points of the left edge that moves 10 points along x, more than along y, is the swipe's: its release clicks nothing and a scroll root leaves it alone. A press that moves along
y first is the scroll's, and one that hardly moves is a tap. It works during a running transition too. `push` and `sheet` are the kinds it is for, and a `SharedElement` morph just goes with its page.

Case 1374 holds the rules; `checks/uiPageTransitions` with `RAE_PAGE_DRAG` runs it.

## 28. Reduced motion

A scene and an app need nothing. With the system's reduced motion setting on (iOS, macOS), position, scale and rotation do not animate, and everything that moved is a short fade. It is read whenever
motion starts. `RAE_REDUCE_MOTION=on` or `off` fixes it for a test. Case 1375 holds the rules.

## 29. `HoverScale.motion`

```json
"HoverScale": { "pressedScale": 0.96, "motion": "quick" }
```

With `motion` the scale runs on that spring, and keeps its speed when the pointer changes its mind. Without one it eases at `speed` as before, so existing scenes look the same. An
unknown name is an `unknownToken` diagnostic at load. The bundle `pressable` uses `quick`. Case 1376 holds the rules.

## 30. Scrolling runs on seconds

`updateScrollRoot(world, uiSystems, entityId, input, deltaTime)` takes the frame's time, so a fling and a spring-back take the same time at 60 and 120 Hz. `ScrollState.velocity` is in points a second. A
frame longer than 0.05 s counts as 0.05 s, so a long frame slows scrolling and never skips it. At 60 Hz it does what it did. Case 1377 holds the rules.

## 31. `FocusStyle`: a focused field's edge

```json
"Title": {
  "Shape": { "kind": "RoundedRect", "fill": "surface", "stroke": "fieldEdge", "strokeWidth": 1.5, "radius": "radiusPill" },
  "TextField": { "placeholder": "Title" },
  "FocusStyle": { "stroke": "accent", "strokeWidth": 2.5, "motion": "quick" }
}
```

On the node with the `TextField` and its `Shape`: while the field has focus, the edge takes `stroke` (a palette slot, or `#RRGGBB` /
`#RRGGBBAA`) at `strokeWidth` (below 0, the default, keeps the Shape's width), and it fades there and back on `motion` (`quick` by
default) as focus comes and goes, from wherever it is. A field with no edge of its own fades one in. With motion off it changes at once.
An unknown motion is an `unknownToken` diagnostic at load. Case 1384 holds the rules; `checks/uiFocusField` paints them
(`docs/images/ui-focus/`).

## 32. `BackdropBlur`: frosted glass

```json
"Tile": {
  "BackdropBlur": { "radius": 24, "tint": "rowFill" },
  "Shape": { "kind": "RoundedRect", "fill": "#00000000", "radius": 20 }
}
```

Before its own `Shape` and children, the node paints what the frame has drawn under its box so far,
blurred, rounded like its `Shape` (its radius, or `CornerRadius`), with `tint` over it. `radius` is
how far the blur reaches, in design units (default 24). `tint` is a theme color name or `#RRGGBB` /
`#RRGGBBAA`, `""` for none (the default). Nothing else is needed: the app does not capture or blur
anything itself, unlike `BackdropImage`.

How it is drawn (lib/Gpu2dCanvasBackdrop.rae): the canvas flushes, ends its pass, shrinks the frame
to a quarter of its size, or an eighth for a blur of 32 pixels or more, and blurs it there with one
25-tap Gaussian pass across and one down (the taps a quarter of the blur's sigma apart, so never
more than about a texel), opens its pass again and draws each node's own region of that copy: four
passes a copy (0.3.77; it was eight, the blur repeated up to three times with 13 taps). Every `BackdropBlur`
node of a layer shares the one copy; a node in a later layer (a dialog's card over blurred tiles)
takes a new one when that layer has drawn since. The frame's drawable must be sampleable: the
surface asks for `TextureBinding` where it supports it, and where it does not, the frames after the
first render into the offscreen target. The blur's size is that of the first node of a copy.

A frame keeps the last frame's copy when nothing under the nodes changed (0.3.71): when the last
frame used a backdrop, the canvas folds the frame's clear color and every box, image and glyph it
flushes before its first copy into a signature, and a first copy whose signature, size and blur
match the last frame's is not redone. What is drawn after the copy (a node's own Shape and text, a
layer on top) never counts. A frame whose later layer took a second copy makes the next frame
redo its first. An image whose pixels change under the same handle (a video) is not seen: it
would keep an old blur. On a Mac a still screen with three blurred tiles then costs about what it
does without them (the same number of command buffers; the tiles' own draws are ~16 us a frame).

On the iPhone 15 Pro (2026-11-10, SUMU's weather screen with its `glassCard` tiles, the hour strip
swiped six times, `RAE_GPU2D_TIMING=1`): the frames hold 120 Hz and the longest Metal command buffer
a second is 5.1-7.0 ms with the blur against 6.2-7.2 ms without it, so the frame's work does not
grow. The blur makes one copy every frame there, never kept: an image under the first tile (about
232 x 101 points at 80, 375) moves about 0.04 points a frame, so what is under the tiles is never
still. Each of the copy's passes is a Metal command buffer of its own in wgpu-native: 2,197-2,592
buffers a second with the blur, 702-739 without (about 15 more a frame), and their summed time
overlaps (2.5-3.3 s a second against 0.53-0.63 s), so it counts waits, not only work.

With four passes a copy (0.3.77), the same run: 1,763-1,853 buffers a second (about 9 more a frame
than without the blur, not 15), their summed time 2.0-2.3 s a second, the longest 5.2-6.3 ms, and
the frame interval during the swipes a median 8 ms, 90% 9 ms (2 of 558 over 9 ms).
`checks/backdropBlur` (its radius 24): the gray across a tile ranges 3 levels, not 13; at radius 64
it is even, at radius 8 the 20-unit stripes still show (a sigma of 4 units only halves them).

There is no host call to capture a backdrop (decided 2026-11-11): an app keeps what lies under
its blurred tiles still, and the kept copy does the rest. SUMU's background breathes only on its
launcher and eases to rest in 0.3 s when an app opens; then its TODO list, dragged on the iPhone,
keeps the copy in 50 to 107 of every 120 frames, the others being frames where TODO's own text
above the first blurred row moves with the finger.

`checks/backdropBlur` paints three tiles over stripes, reads the picture back, and counts the copies
over 20 frames (the stripes move once, a box moves on top every frame).

## 33. `Timeline`: lanes of spans against time

A chart for large data (docs/perf-tools-design.md §6), general, not tied to the profiler. The
scene authors its look; the app gives it its data in code, and the data is never saved with the
scene.

```json
"Chart": { "Rect": { "x": 0, "y": 0, "w": 760, "h": 300 },
           "Shape": { "kind": "RoundedRect", "fill": "surface", "radius": 6 },
           "Timeline": { "rowHeight": 22, "laneGap": 6, "actionId": "timeline.pick" },
           "WheelTarget": {} }
```

- `rowHeight` (default 16) is one row of a lane; a lane has as many rows as its spans' deepest
  nesting. `laneGap` (default 4) is the space between lanes. `actionId` (when set) is sent as a UI
  action, with the chart as its entity, when a click selects a span.
- Its system is `timelineSystem(world, uiSystems, input)` (lib/ui/timelineSystem), once a frame
  after `updateUiInput`; it gives the node a `TimelineState` (not saved) on its first frame. The
  wheel over the chart zooms about the pointer (give the node a `WheelTarget` so the wheel comes to
  it), a drag pans, and a click without a drag selects the span under the pointer
  (`selectedLane`, `selectedSpan`; -1 for none). On a trackpad a sideways two-finger scroll pans
  and a pinch zooms about the pointer (`UiInput.wheelX`, `UiInput.pinch`, from lib/Gpu2dGestures);
  on a touch screen two fingers inside the chart pan with their middle and zoom with their spread
  (`UiInput.twoFingers`, `twoFingerShiftX`, `twoFingerScale`), and they never start a drag.
- The app fills the state: `timelineAddLane(state:, name:)`, `timelineName(state:, name:)` (a
  span's name as an index), `timelineAddSpan(state:, lane:, start:, end:, nameId:, depth:)` in
  order of `start` (an earlier one is refused), `timelineAddMark(state:, lane:, time:)`, and
  `timelineShow(state:, start:, end:)` for the view.
- It is drawn (lib/ui/RenderSystem/RenderTimeline) over the node's own fill, clipped to its box:
  the lanes' bands, each span in its name's color with the name inside when it is wide enough,
  the selection outlined, the marks as thin lines, and each lane's name on a chip. Its cost is
  what is visible: spans narrower than a pixel are one rect per pixel column, found by binary
  search, so a lane of a million spans zoomed all the way out is about one rect per column.

Case 1472 checks the keys, the rects, the hit test, the click, the wheel, the drag, the trackpad
and touch gestures, and a million spans; `checks/uiTimeline` paints one and compares it with its
`reference.png` (checks/README.md, "Reference pictures"), like `checks/uiFlameHistogram` and
`checks/uiLineTable` for the other charts.

## 34. `FlameGraph` and `Histogram`

Two more charts of the same kind as the `Timeline` (section 33): the scene authors the look, the
app fills an unsaved state in code, a system once a frame after `updateUiInput` gives the node its
state and handles a click, and the drawing is over the node's own fill, clipped to its box.

```json
"Flame":  { "Rect": { "x": 0, "y": 0, "w": 760, "h": 200 },
            "FlameGraph": { "rowHeight": 24, "actionId": "flame.pick" } },
"Spread": { "Rect": { "x": 0, "y": 0, "w": 760, "h": 240 },
            "Histogram": { "budget": 16, "barGap": 2, "actionId": "histogram.pick" } }
```

- `FlameGraph` (lib/ui/flameGraphSystem, `flameGraphSystem(world, uiSystems, input)`): a call
  tree's totals as nested bars. The focused node is the top row, as wide as the chart; each child
  is under its parent, as wide as its share of the parent's total, left to right in the order
  added. `rowHeight` (default 18) is one row; `actionId` is sent when a click selects a bar. A click
  on a bar zooms into it, a click on the focus zooms back out to its parent. The app adds nodes with
  `flameAddNode(state:, parent:, name:, total:)` (node 0 is the root, "all") or whole call paths
  with `flameAddSample(state:, path:, value:)`, which adds `value` to the root and every node on the
  path, making the nodes on first use. A bar under half a pixel is not drawn, nor anything under it.
- `Histogram` (lib/ui/histogramSystem, `histogramSystem(world, uiSystems, input)`): buckets of
  values as bars, each as wide as its range and as high as its count against the largest, with a
  red line at `budget` (the app's unit; 0 for none) and lines at the 50th, 95th and 99.9th
  percentiles, read from the buckets straight across within one (`histogramPercentile(state:,
  perMille:)`). `barGap` (default 1) is the space between bars; `actionId` is sent when a click
  selects a bucket. The app sets the buckets with `histogramSetBuckets(state:, edges:, counts:)`
  (one more edge than counts, rising; anything else is refused).

Case 1473 checks both charts' keys, layout, hit tests and clicks; `checks/uiFlameHistogram` paints
them.

## 35. `LineGraph` and `StatTable`

Two more charts of the kind in sections 33 and 34 (authored keys, an unsaved state the app fills,
a system once a frame after `updateUiInput`, a clipped drawing over the node's own fill).

```json
"Track":    { "Rect": { ... }, "Timeline": {}, "WheelTarget": {} },
"Counters": { "Rect": { ... }, "LineGraph": { "link": "Track", "actionId": "line.pick" } },
"Stats":    { "Rect": { ... },
              "StatTable": { "rowHeight": 24, "headerHeight": 28, "actionId": "table.pick" },
              "WheelTarget": {} }
```

- `LineGraph` (lib/ui/lineGraphSystem, `lineGraphSystem(world, uiSystems, input)`, run after the
  timelines): counters over time, a line per series. `link` names a Timeline node whose view it
  shows, so the two zoom and pan together; with no `link` the wheel zooms it about the pointer
  (give it a `WheelTarget`) and a drag pans it. Its height spans `minValue` .. `maxValue`, or (both
  0) the lowest to the highest point in view. A click selects the point nearest the pointer
  (`selectedSeries`, `selectedPoint`) and sends `actionId`. The app adds series with
  `lineAddSeries(state:, name:)` and points with `lineAddPoint(state:, series:, time:, value:)` in
  order of time. Only the points in view are read, each pixel column of a series drawn as one
  stroke from its lowest to its highest value (a one-sample spike stays), joined to the next.
- `StatTable` (lib/ui/statTableSystem, `statTableSystem(world, uiSystems, input)`): a header of
  column titles over rows of cells. A click on a header sorts by that column (the same header
  again: the other way; a numeric column by its numbers, others by text, ties keeping their
  order), the wheel scrolls three rows a notch within the rows, and a click on a row selects it
  (`selected`, an index into the rows) and sends `actionId`. `rowHeight` (default 22) and
  `headerHeight` (default 26) size it. The app adds columns with `statAddColumn(state:, title:,
  width:, numeric:)` and rows with `statAddRow(state:, texts:, values:)` (a text per column, a
  number per column for sorting); `statSort(state:, column:, descending:)` sorts from code. Only the
  rows that show are drawn; a numeric column's cells are drawn to the right.

Case 1474 checks both; `checks/uiLineTable` paints them.

## 36. `PlaneView`: items placed anywhere, live only in view

A plane of items, each a rectangle on the plane with a sub-scene: a canvas of mini timelines, a
node graph, a map of cards. It is `ListView` in two dimensions (lib/ui/planeViewSystem). Put it on
the node that carries the `Camera2D`:

```json
"Canvas": {
  "Rect": { "x": 0, "y": 0, "w": 1280, "h": 800 },
  "Camera2D": {},
  "PlaneView": { "itemSceneId": "mini", "detailPixels": 120, "maxLiveItems": 400 }
}
```

- The keys: `itemSceneId` (an item's sub-scene, unless the item names its own), `lowDetailSceneId`
  (an optional smaller sub-scene for items from a quarter of `detailPixels` up), `detailPixels`
  (120: the size on screen from which an item gets its full sub-scene), `cellPixels` (24),
  `marginPixels` (200: how far past the view items stay live) and `maxLiveItems` (400).
- The app gives the items in a `PlaneViewData` (not saved): each `PlaneItem` has `x`, `y`, `w`,
  `h`, an optional `sceneId` and box `color`, and a data row; the bindings are ListView's.
- `planeViewSystem(world, uiSystems, scenes)` runs before the ui pipeline, like `listViewSystem`.
  A ui/SpatialGrid over the items finds those that overlap the view plus the margin. They get
  pooled entities, re-pointed at their rows as the camera moves; an entity whose item left is
  hidden and kept for the next one. Items smaller on screen are boxes the plane draws itself
  (lib/ui/RenderSystem/RenderPlane), and when a grid cell is under `cellPixels` on screen, each
  cell is drawn as one box, as opaque as its items cover it. Past `maxLiveItems` the rest are
  boxes, the items in the view before those in the margin.
- When an item's entity is let go, its bound `Text.text`, `Text.styleId`, `Sprite.textureKey` and
  `Active.value` are read back into its row, so an edit stays when it scrolls out and back. A
  change the app makes to the items (the `PlaneViewData` table changed) is bound again to every
  live item.
- `planeItemEntityId(world, planeEntityId, item)` gives an item's live entity (or none).

Case 1482 checks the windowing, the recycling, the read-back, the levels and the budget;
`checks/uiPlaneView` mounts 50,000 mini timelines (in 20 ms on an M-series Mac, at most 1,146
entities, the systems at most 0.54 ms a frame from zoomed in to everything on screen).

## 37. Hit tests, marquees and snapping on a big canvas

The hit test (`uiHitTest`, and the pointer's in `updateUiInput`) used to walk every node of every
root: 400 us a test with 50,000 nodes. It now asks ui/HitIndex, a grid of the clickable nodes
(those with an `OnClick`) that `updateUiInput` keeps current once a frame
(lib/ui/InputSystem/HitIndexQuery `hitIndexSync`):

- Each node's rectangle is kept in the coordinates of its nearest ancestor with a `Camera2D` (the
  screen when it has none), so a camera that pans or zooms moves nothing in the grid.
- A frame where none of the tables the hit test reads changed costs nothing. When they changed,
  the tree is walked once, in the hit test's order and with its rules; when the clickable nodes
  are the same, only a node whose cells changed is moved in the grid (a moved node updates its
  own cells), and otherwise the grid is built again.
- A query looks at the cells around the point and tests those nodes exactly as the walk does, so
  the answer is the walk's. When a table changed after the sync, `uiHitTest` walks instead.
- `hitIndexInRect(world, uiSystems, left, top, right, bottom)` gives the clickable nodes in a
  rectangle (a marquee), topmost first; `hitIndexNearestEdgeX` / `hitIndexNearestEdgeY` give the
  nearest node edge to a line within a distance (snapping), leaving out the node being dragged.

Case 1484 compares the index with the walk on a random scene (a camera canvas, a screen toolbar,
nested, inactive and rotated nodes), with the marquee and the edges against testing every node;
`checks/uiHitIndex` times 50,000 nodes: 0.11 us a hit test (the walk 390 us), built in 3.3 ms, a
panned camera's frame 1.4 ms (the walk to see what changed), an idle frame 0.2 us.

## 38. `Camera2D.originX` / `originY`: a canvas far from the origin

A `Float` holds about 7 digits. Under a `Camera2D` a canvas point lands on screen at
`offset + zoom * point`, and far from 0 those are two big numbers whose difference is the small
screen place: at 1e7 under zoom 50 both are near 5e8, where a Float steps by 32, and the point
jumps by up to 16 px as the camera pans (1e6: 2 px, 1e5: 0.25 px).

`originX` and `originY` (Float64) name the canvas point that sits at the camera root's origin. The
transform pass takes it off each child's place at full precision BEFORE the zoom scales it, so
every Float stays small: a screen point is `root + offset + zoom * (point - origin)`.

```json
"Camera2D": { "zoom": 50, "offsetX": 0, "offsetY": 0, "originX": 10000000.25, "originY": 0 }
```

- Pan a big canvas by moving the origin, and keep the offset for the small part (the screen
  place of the origin). With the origin at 0 (the default) nothing changes.
- The plane view (§36), its boxes and cells, and the hit index (§37) map through the origin too;
  the hit index keeps its rectangles in canvas coordinates (Float64), so a moving origin moves no
  entry.
- An item's own place is still a Float (its `Rect`, a `PlaneItem`): at 1e7 it is placed to the
  nearest whole unit. That error is fixed, not a jitter; give such items whole-unit places.

Case 1485 pans a node at 3, 1e5, 1e6 and 1e7 under zoom 50 both ways (0.000 px by the origin at
every distance) and windows a plane view at 1e7.

## 39. `Wires`: curves between nodes

A node with `Wires` draws many wires (its `WireData`, filled by the app), each a cubic Bezier from
one end to the other, leaving and arriving level, as a node editor draws its links:

```json
"Links": { "Rect": { "x": 0, "y": 0, "w": 1, "h": 1 }, "Wires": { "color": "accent", "width": 2 } }
```

- The keys: `color` (a theme palette name), `width` (in the node's units, so it zooms with a
  camera), `minScreenWidth` (1.5: never thinner on screen, so wires stay readable zoomed out),
  `hitTolerance` (4 screen points past a wire's edge still hit it) and `bend` (0.5: how far the
  middle points reach out along x, as a share of the ends' horizontal distance).
- Each end follows a node (`wireEndOn(entityId, anchorX, anchorY)`: a share of its box, 1, 0.5 the
  right edge's middle) or is a fixed point in the Wires node's units (`wireEndAt(x, y)`);
  `wireConnect(world, wiresEntityId, fromEntityId, toEntityId)` joins a right edge to a left
  edge. The ends are read from the nodes' places every frame, so a wire follows a moved node.
- `wireAt(world, uiSystems, wiresEntityId, x, y)` gives the wire under a screen point (the
  nearest within the tolerance), or -1.
- The drawing (lib/ui/RenderSystem/RenderWires, lib/Gpu2dCanvasCurve) puts each wire in the
  frame's one box draw, in the box shader's curve mode: cut into a few exact sub-curves, each a
  quad turned along its chord, whose pixels measure their distance to the curve. Put the Wires
  node first under the canvas, so wires go under the nodes.

Case 1486 checks the ends, the hit test (on the curve and 3 points off hit, 10 points off on either
side do not), a moved node, the minimum width and the scene keys; `checks/uiWires` draws 10,000
wires between 2,000 boxes: 1.1 ms of CPU a frame with all on screen (0.26 ms zoomed in), and
about 1.7 ms of GPU time.

## 40. `PaintCache`, and what a zoomed-out canvas leaves out

A canvas of many nodes, zoomed out, used to draw every node: 50,000 mini timelines took 3.6 s a
frame. Three rules make it cheap (lib/ui/RenderSystem/RenderPaintCache):

- **Specks.** Under a zoomed-out camera (a node's scale under 1), a node at most 4 pixels on
  screen draws its own box only, not its children.
- **Small text.** Under a zoomed-out camera, text under 3 pixels tall is not drawn.
- **A cached picture.** A node with `PaintCache` (put it on the camera's root) whose whole
  subtree fits on screen draws that subtree once into a texture the frame's size
  (lib/Gpu2dCanvasCache) and then draws the texture as one image, moved and scaled with the
  camera. It draws the subtree again when the content changes (rects, shapes, texts, sprites,
  layout's inputs, which nodes are active or there) or when the zoom leaves `zoomOutLimit`..
  `zoomInLimit` (0.5..1.5) times the zoom it was drawn at. When the subtree does not all fit
  (zoomed in), the nodes are drawn as they are.

```json
"CanvasRoot": { "Rect": { ... }, "Camera2D": {}, "PaintCache": {} }
```

At scale 1 or more nothing is cut, so an ordinary page draws as it always has. A cached subtree
does not animate: a hover or a motion inside it shows when the picture is drawn again.

`checks/uiPaintCache`, 50,000 mini timelines headless on an M-series Mac: the capture frame
14 ms, then each frame drawn from the picture 0.6 ms (its paint walk 0.25 ms); zoomed in 4 times,
the nodes themselves 8 ms. The pop where the picture gives way to the nodes, the same camera drawn
both ways: a mean of 0.01 of 255 when the picture was drawn at that zoom, 0.48 when it was drawn
at 0.7 of it and scaled up. Case 1487 checks the rules without a GPU.

## 41. `CanvasControls` and `CanvasDraggable`: a canvas's gestures

Put `CanvasControls` (and a `WheelTarget`, so the wheel comes to it) on the node that holds the
`Camera2D`, and `CanvasDraggable` (with an `OnClick`, so the hit test finds it) on the nodes a drag
should move:

```json
"Canvas": { "Rect": { ... }, "Camera2D": {}, "CanvasControls": { "snapDistance": 6 }, "WheelTarget": {} }
"Clip":   { "Rect": { ... }, "OnClick": { "actionId": "clip.pick" }, "CanvasDraggable": {} }
```

Each frame, after `updateUiInput`, the app runs
`canvasGestureSystem(world, uiSystems, canvasGesturesFrom(input, canvasEntityId, deltaTime))`
(lib/ui/canvasGestureSystem); a test passes its own `CanvasGestures`.

- A trackpad's pinch, and the wheel with Ctrl or Command held, zoom about the pointer
  (`wheelZoom` a notch, within `minZoom`..`maxZoom`); the canvas point under the pointer stays
  where it is. Two fingers on a touch screen zoom about their middle and pan with it.
- Smooth zoom (all off by default, so the zoom jumps as before):
  - `zoomEaseSeconds` (0): a wheel notch or a zoom key sets a target zoom that the camera
    eases to over this time (a cubic ease-out, even steps in the zoom's log), the canvas point
    under the pointer staying put on every frame of it. A notch during an ease starts again
    from where the camera is, toward the target moved by one more step; a target past
    `minZoom` or `maxZoom` stops there, with no frame beyond it.
  - `keyZoom` (1.25): the zoom step of `=` (in) and `-` (out), with no modifier held
    (Ctrl/Cmd + - is the user zoom, ui/UiZoom), about the pointer.
  - `pinchCoasts` (false): a pinch follows the fingers 1:1, and let go while zooming it keeps
    its zoom speed about the same point and slows with `friction`, as a released pan coasts;
    it stops when slow, or at `minZoom` / `maxZoom`.
- The wheel without the modifier pans (`wheelPan` points a notch, down and sideways), unless
  `wheelZooms` (false) is set: then the wheel zooms about the pointer with no modifier, as in a
  node editor or a map, and only a sideways wheel pans.
- A drag with the middle mouse button pans the canvas, begun anywhere inside it, over a node
  too (the middle button never drags a node), and coasts when let go while moving
  (`CanvasGestures.middleDown`).
- A drag on the canvas (on nothing clickable inside its box, or on a node of it that is not
  draggable) pans it. Let go while moving, it coasts and slows (`friction`); a tap stops it.
- A drag on a `CanvasDraggable` node moves its `Rect`. Its left or right edge snaps onto the
  nearest left or right edge of another clickable node on screen within `snapDistance` points,
  and its top or bottom the same way (ui/HitIndex's nearest-edge queries; what is inside the
  dragged node does not count). A guide line is drawn across the screen where they meet
  (lib/ui/RenderSystem/RenderCanvasGuides).

Case 1489 drives each gesture with scripted input: the zooms leave the point under the pointer
within a hundredth of a pixel, the pans move by what they should, a released drag coasts and a
tap stops it, and a dragged card snaps onto another's edge 3 points away but not 23.

## 42. Text outlines: an `outline` token on a text style

A text style draws an outline around its glyphs when it names an `outline`
token, the way it names a `shadow` token. Both are theme data, in the scene or
a scene it imports:

    "palette": { "brown": { "r": 96, "g": 52, "b": 20, "a": 255 } },
    "outline": {
      "label":     { "color": "brown", "width": 2 },
      "labelGold": { "extends": "label", "color": { "r": 255, "g": 190, "b": 30, "a": 255 } }
    },
    "text": {
      "menu": { "size": 34, "outline": "label" },
      "play": { "extends": "menu", "outline": "labelGold" }
    }

- `color` is a palette name (resolved when drawn, so it follows the light or
  dark palette) or an `{r, g, b, a}` object (`a` defaults to 255).
- `width` is in points, around the glyph's edge; a token without one is 1.
- A token may `extends` another: it takes the parent's fields and overrides
  the ones it names (`labelGold` above is 2 wide and gold).
- A text style's `outline` is inherited through the style's `extends` like
  its other fields; a child names another token, or none, to change it.
- A style naming a token the theme does not declare draws without it and is
  an `unknownToken` diagnostic (`text.<style>.outline`); a misspelled
  `shadow` token is reported the same way (`text.<style>.shadow`).

The renderer draws the style's shadow (of the outlined glyphs), then the
outline, then the glyphs in the style's color, all from the font's distance
field. The outline cannot reach past that field: about half the atlas's
distance range, scaled to the text size (`distanceRange / 2 * size /
atlasSize`; Roboto's atlas, range 12 at size 48, allows about 2 points at 17
point text and 4 at 34). A wider outline is cut off at that distance. The
check `checks/uiTextOutline` draws two outlined styles in one frame; case
1524 checks the parsing.

## 43. Line breaks in a `Text`

A `"\n"` in `Text.text` (a JSON newline in a `.raescene`, or one set from
code) is a hard line break; `"\r\n"` counts as one break, and so does a lone
`"\r"`.

    "Label": { "Text": { "text": "Chest\nslot", "styleId": "slot" },
               "Size": { "w": { "mode": "Hug" }, "h": { "mode": "Hug" } } }

- Each line starts one line height below the one before: the style's
  `lineHeight` (a factor of its size, 1 by default) times the size.
- A `Hug` text measures every line: its width is the widest line, its height
  the size plus one line height for each line after the first (two lines of
  size 20 at `lineHeight` 1.5 are 50 high). Like a single line, the box ends
  at the last line's baseline, and descenders hang below it.
- Alignment applies per line: a centered style centers each line in the node,
  an end-aligned one ends each line at the node's right. A style that centers
  vertically (`vAlign`, or `OpticalAlign.centerY`) centers the block of lines.
- A text that wraps (`wrapWidthMode: NodeWidth`) breaks at a hard break too and
  wraps each part on its own.

Case 1525 measures two- and three-line texts and draws start-aligned and
centered ones in one frame.

## 44. `ZoomBehavior`: how a node acts as its canvas zooms

Under a `Camera2D` every node scales with the zoom, text included. A node with
`ZoomBehavior` (lib/ui/ZoomComponents) chooses otherwise:

```json
"Port":   { "Rect": { ... }, "ZoomBehavior": { "mode": "Fixed", "pivotX": 0.5, "pivotY": 0.5 } },
"Title":  { "Rect": { ... }, "Text": { ... },
            "ZoomBehavior": { "mode": "Clamped", "minScale": 0.6, "maxScale": 2.5, "showFrom": 0.3 } },
"Summary":{ "Rect": { ... }, "ZoomBehavior": { "showTo": 0.7, "fade": 1.6 } },
"Details":{ "Rect": { ... }, "ZoomBehavior": { "showFrom": 0.7, "fade": 1.6 } }
```

- `mode`:
  - `Scales` (the default) grows and shrinks with the camera, text too. This is
    the zoomy feel: you come closer and the canvas's own words get bigger.
  - `Fixed` keeps its authored size on screen at every zoom: handles, ports,
    selection outlines, labels that must stay readable. Its children keep
    theirs with it.
  - `Clamped` follows the zoom between `minScale` and `maxScale` on screen
    (1 is its authored size), then holds.
- `pivotX`, `pivotY` (0..1 of its box, 0, 0 by default): the point that stays
  where the camera puts it while the node's size is held. A port in the middle
  of an edge uses 0.5, 0.5; a title at a card's corner 0, 0.
- `showFrom`, `showTo`: the camera zooms the node shows between (0: no
  bound). Across each bound it fades over a zoom factor of `fade`, centered
  on the bound (from `bound / sqrt(fade)` to `bound * sqrt(fade)`); outside
  its range it is hidden and not hit. Two levels of detail that share a bound
  and a fade cross-fade, their alphas adding up to 1; `fade: 1` switches them
  at the bound, the discrete feel of a tool that shows data.
- The zoom a node sees is the product of the Camera2D zooms above it, up to an
  ancestor with a ZoomBehavior of its own, which passes on the size it
  settled on. The transform pass applies it (lib/ui/Transform): the node's
  scale is multiplied by its wanted size over the zoom it sees, about its
  pivot, and its alpha by its fade.

Which to use is a choice per canvas. A data tool (a timeline, a profiler)
reads best analytical: text at one size, levels that switch. A canvas you
explore (a mood board, a node graph) feels better zoomy: text that scales
within limits and levels that cross-fade. Example 152 (`examples/152_zoomCanvas`)
is a node canvas with both, `F` switching between them; case 1526 checks
each mode's size on screen at zoom 0.25, 1 and 4, the pivot, a fixed node's
child and a cross-fade.

A camera's zoom eases and coasts with `CanvasControls.zoomEaseSeconds` and
`pinchCoasts` (§41); example 152 uses both.


## 45. `Tab` and `TabPage`: tabs

```json
"ClipTab": {
  "bundles": ["pressable"],
  "Shape": { "kind": "RoundedRect", "fill": "buttonQuiet", "radius": "radiusPill" },
  "OnClick": { "actionId": "side.clip", "label": "Clip tab" },
  "Tab": { "group": "side", "id": "clip", "fill": "buttonQuiet", "selectedFill": "accent" },
  "Children": ["ClipTabLabel"]
},
"ClipPage": { "TabPage": { "group": "side", "id": "clip" }, "Children": ["..."] }
```

A group of tabs shows one page at a time. The tab bar and the pages are ordinary nodes anywhere in
the scene: a tab names its group and its id, and a page the group and the id of the tab that shows
it. `Tab` has these fields:

| field | default | what it is |
|---|---|---|
| `group` | `""` | the tabs that choose among one set of pages |
| `id` | `""` | this tab, and the pages it shows |
| `selected` | `false` | whether it is the group's chosen tab |
| `fill` | `"surfaceAlt"` | the theme slot of its `Shape`'s fill |
| `selectedFill` | `"accent"` | the slot while it is selected |

`TabPage` has `group` and `id`.

- A tap on a tab selects it and leaves the group's others. A tab needs an `OnClick` for a tap to
  reach it; its action goes on to the app like any tap's, so the app hears which tab was chosen.
- The group's pages of the selected id are active and its others are not: not laid out, painted or
  hit (`Active`, through `setActiveAnimated`, so a page's authored `Appear` or `Disappear` plays).
- A group with no tab selected selects its first, in load order, so a scene opens on its first page
  without saying so; a tab authored `"selected": true` opens selected instead.
- The app calls `tabSystem(world:, actions:)` (`ui/tabSystem/TabSystem`) each frame, after the input
  system and before layout. It answers true when it changed anything.
- `selectedTab(world:, group:)` reads a group's chosen id and `selectTab(world:, group:, id:)` sets
  it (false when the group has no such tab); the page shows at the next `tabSystem`.

Case 1560 checks it: the first frame, a tap's action, `selectTab`, two groups apart, and real taps
through the input path hitting the shown page's node and never the hidden one's. The media editor's
side panel is a group of them.

## 46. `OnSecondaryClick`, double clicks and the other buttons

```jsonc
"Row": {
  "OnClick": {"actionId": "row.select", "label": "Select", "actionIdDouble": "row.open", "labelDouble": "Open"},
  "OnSecondaryClick": {"actionId": "row.menu", "label": "Show the row's menu"}
}
```

- **Double and triple clicks.** A click sends `OnClick.actionId` at once. The second click of a
  double click sends `actionIdDouble`, the third and later ones of a series `actionIdTriple`, each
  where the node has it, else `actionId` again. The system's own double-click interval decides. A
  click on another node starts the count again, so a list must keep a row's node between two clicks.
  The action's event says which click it is: `UiAction.clicks`. (`maxDelayMs` is not read.)
- **The secondary click** is the right button, Control-click on macOS, or a finger held still (0.5 s,
  within 10 points, not a scroll). It sends the `OnSecondaryClick.actionId` of the nearest node that
  has one, going up from the topmost painted node under the pointer, as the button goes down. So a
  row has its own, and the list behind it one for the empty space. A finger's release then clicks
  nothing. A panel that paints over the node, or a dialog over it, keeps it from the click.
- **Labels** are required as `OnClick`'s are (`missingLabel`), and `clickTargets` lists the action with
  the gesture `secondary`.
- **The buttons themselves**: `UiInput.pointer.middle` and `.secondary` (held, pressed, released,
  clicks); in a game's world `worldPointer(captures:, pointerId: pointerIdSecondary)`.

Case 1577 checks it with simulated mouse events and scripted fingers.

## 47. Menus

A menu is opened by code (`ui/MenuSystem`); its items are data.

```rae
openMenu(world: world, uiSystems: uiSystems, request: MenuRequest {
  id: "row"
  owner: rowEntityId
  items: {
    MenuItem { id: "open", text: "Open", icon: "mat:folder_open", shortcut: "Cmd O" }
    MenuItem { id: "with", text: "Open with" }
    MenuItem { id: "editor", text: "Text editor", parent: "with" }
    MenuItem { separator: true }
    MenuItem { id: "paste", text: "Paste", enabled: false }
  }
  x: input.pointer.x
  y: input.pointer.y
})
```

Each frame: `updateUiInput`, then `menuSystem(world:, uiSystems:, input:, keys: menuKeysFromDevice())`,
then the program reads `world.menuEvents` (`MenuEvent`: `chosen` with the item's id, and `closed`,
which also follows a choice) and its actions. `menuPlaceSystem` runs after the layout; the pipelines
of `app3d/UiShell` call it. A world has one menu open at a time.

- **Items**: an id, a text, an `icon` (a `Sprite`'s texture key), the `shortcut`'s text (the menu does
  not read the keys), `enabled`, `separator`, and `parent`: the id of the item whose submenu it is in.
  Submenus have one level.
- **Place**: below and to the right of the anchor (`x`, `y`; with `w` and `h` a button's rectangle),
  above or to the left where it would not fit, always inside the window and its safe area. A menu
  taller than the window slides to its focused row and by the wheel. It is on the `popover` layer, or
  on its owner's layer when that is above (a dialog's). Rows are 28 points tall, 44 with `touch`.
- **Pointer**: the row under it is focused, a submenu opens after 0.2 s or at a click; a press of the
  secondary button dragged onto a row and let go there chooses it. A primary press outside closes the
  menu and does nothing else; another button's press goes on to what is under it.
- **Keys**: the arrows (disabled rows are skipped), Home, End, Return, Escape (first the submenu),
  right and left for the submenu, and typing the start of a row's text, case and accents aside. While
  a menu is open Escape does not quit the program and the keys are the UI's (`uiClaimsKeys`); a program
  that reads keys itself leaves them while `world.menu.open`.
- **Look**: the bundles `menuPanel`, `menuRow` (its `Shape` is drawn while the row is focused),
  `menuIcon`, `menuText`, `menuShortcut`, `menuSeparator` and `menuLine`. A theme that declares one of
  them replaces it. A focused row's text and icons are the slot `accentText`.
- It closes when its owner is destroyed. A program that sleeps between frames wakes while
  `menuWaiting(world:)`.

Case 1578 checks it with scripted input, `checks/uiMenu` is its picture.
