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

### Built-in bundles

lib/ui provides a few bundles itself (lib/ui/BuiltInBundles), so every scene
can use them without declaring them; they are looked up last, after the
scene's own and its imports', so a theme that declares the same name
replaces one. They carry shape and placement, never colors, and combine
with a theme's look by coming after it in the list:

| bundle | what it is |
|---|---|
| `circleButton` | a round button for an icon: a 44-point square box (`Size` fixed; a node's own `Rect` sets another size, keep it square), a `Circle` shape (fill and stroke from the bundles before it or the node) and its child centered. `"bundles": ["secondaryButton", "circleButton"]` is the theme's button drawn as a true circle. A tap anywhere in the square counts. |
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

A `ListView` with `"animateMoves": true` and an `itemKeyField` animates its
rows when the data's order changes: each row glides from where it was drawn
to its new place (section 23).

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

### Scrolling a list to a row

An app asks for a row of a virtual `ListView` in view with
`requestScrollToRow(world, listEntity, index, place)` (lib/ui/ScrollToRow,
ui/scrollRootSystem/ScrollToRowSystem): the list's scroll root (its own,
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

## 22. `AnimateLayout` and `AnimateText`: moves and text changes

```json
"Row": { "bundles": ["glideOnChange"] },
"Time": { "Text": { "text": "09:59" }, "AnimateText": { "kind": "roll", "rollFrom": "below", "motion": "smooth" } }
```

| Component | Fields and their defaults |
|---|---|
| `AnimateLayout` | `motion` smooth |
| `AnimateText` | `kind` fade (or roll), `rollFrom` below (or above), `motion` quick |

- **`AnimateLayout`**: when layout moves the node, it glides from its old place to the new one. Layout runs once and
  gets the true places, and later frames run no layout. Only position glides: size changes at once. There is no
  glide on a first layout, on a window resize, on a scroll, or for a node that was hidden. The bundle
  `glideOnChange` is this component with its default motion.
- **`AnimateText`**: when the node's text changes, the old text goes as the new one comes. `fade` crosses the two.
  `roll` also moves them, inside the node's box, with the new text coming from `rollFrom`. The first text a node
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

A host has one of two page models (`lib/ui/pageTransitionSystem`), with a `PageTransitionSystem` from `createPageTransitionSystem()`:

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

Case 1373 holds the rules; `checks/uiPageTransitions` paints them.

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
