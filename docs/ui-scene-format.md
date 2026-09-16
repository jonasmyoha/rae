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
  the imported theme before deserialising components, loads declared text and
  icon fonts with `loadSdfFontJson`, resolves mounted sprite/animation textures
  through `assets.textures`, and keeps `gpuUi`'s fonts as fallbacks when the
  package declares none. The app owns `sceneRegistry` so later list
  materialisation can use the same registered sub-scenes.

No walking up for a `.raepack`, no magic files. A sub-scene therefore resolves
from the root, not from its parent scene's folder.

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
        "text":  "../fonts/ChelaOne-Regular.mtsdf.json",
        "icons": "../fonts/MaterialIconsRound-Regular.mtsdf.json"
      }
    }

- `map` is texture key → file; `dirs` is searched as `<dir>/<key>.png`.
  Lookup is `map`, then `dirs`, in resolution order (§3). Paths are relative
  to the declaring file.
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
`--scene-root`. They exist so a viewer can look at someone else's scene with
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
