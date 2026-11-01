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
- The caret is drawn by the renderer after the text, not written into it;
  it blinks every 0.53 s from the last edit, and `textFieldDeadlineSec` is
  when it next changes, for the event loop's heartbeat
  (`earliestDeadlineSec` combines it with another deadline).
