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

## 5. Coverage rule

"Everything found in `lib/ui` must be supported" is a test, not a promise: #1000 adds
a fixture scene that uses EVERY authorable component (`registeredComponentNames`)
with every field, loaded under `report` policy — the gate is **zero diagnostics** —
plus sample scenes (our own, no trademarked content) that exercise the visual
surface: nine-slice sprite, masks, opacity, rotation + anchor, `Hug`/`Fill`/`Fixed`
sizes, `spaceBetween`, text wrapping, shadows, corner radii, sub-scene instancing,
a `ListView`. Each sample is rendered headlessly and asserted non-blank; the
screenshots are the visual reference for the UI tab.

## 6. Not in the first versions

Writing scenes back (the "editor" half), a file picker, drag-to-move, undo. The
inspector is read-only until the DAW/editor capability design
(`ui-ecs-refactor-status.md` §2.7) lands; the app is shaped so those are new
systems, not rewrites.
