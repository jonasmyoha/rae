# The `.raescene` format: scenes are modules, folders are packages (#1008)

This is the reference for how one `.raescene` refers to another and to the data
it draws with. The node/component grammar itself is documented with the
components (`lib/ui/Components.rae`, docs/ui-editor-design.md §5); the theme
token sections in docs/ui-theme-system.md.

## 1. A scene reference is a package path, without extension

A scene is a module and a folder is a package — exactly the Rae source rule
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
The 121 viewer takes mounts as `--scene-mount <prefix>=<folder>` (repeatable)
or `RAE_UI_EDITOR_MOUNTS="apps/notes/=downloads/notes/;..."`. Case 1295 has two
roots.

## 3. `import`: configuration into scope, nothing mounted

    { "type": "Scene", "version": 2,
      "import": ["Theme", "shared/GameAssets"],
      "root": "Screen", "nodes": { ... } }

`import` brings the named scenes' **configuration** into scope — their
`assets` block and their theme token sections (`palette`, `space`, `radius`,
`padding`, `shadow`, `text`, `container`) — and mounts nothing, the way a Rae
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
  it blinks every 0.53 s from the last edit, and `textFieldDeadlineSec` is
  when it next changes, for the event loop's heartbeat
  (`earliestDeadlineSec` combines it with another deadline).

## 12. `ListReorder`: drag to reorder a list's rows

A virtual `ListView` with `"animateMoves": true` and an `itemKeyField`
animates its rows when the data's order changes: each row eases from where
it was drawn to its new place (ui/listMotionSystem: a task ticked done
glides to the end) instead of jumping there. The app runs
`listMotionSystem(system, world, uiSystems, nowSec)` after the list system
while `listMotionNeedsRun` says so, keeping a `ListMotionSystem` (new with
each UI world); it returns true while a row moves.

A virtual `ListView` (the default mode) whose node also carries
`ListReorder` lets a person drag its rows into another order
(lib/ui/ListReorder, the system is ui/listReorderSystem):

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

The gesture is the one phones use. A press that stays still for `holdMs`
lifts the row: it grows to `liftScale`, casts a shadow, is painted over its
neighbors (`PaintOnTop`), and follows the finger, while the rows between its old and
its new place step aside to open a gap. The press is the row's from then on:
its release clicks nothing and the scroll root does not follow the finger. A
press that moves before the hold is a scroll or a tap, as without
`ListReorder`. On the drop the app reorders its data from the action; every
row then eases from where it was drawn into its new place. An app that does
not take the move (its data does not change within half a second) sees the
rows ease back into the old order.

The app runs `listReorderSystem(world, uiSystems, input, nowSec)` each frame
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

## 16. `SafeArea`, and a band the host keeps for itself

A node with `SafeArea` lays its content out inside the screen's safe area:
the notch, the status bar and the home indicator are added to its padding.
`apply` says which sides take part (1 a side, 0 not; all four by default)
and `extra` adds a fixed inset of its own:

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
