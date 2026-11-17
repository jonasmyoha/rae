# Release notes

A release is a git tag (docs/versioning-and-toolchain.md §1). Before 1.0 the
MINOR number is a named milestone; PATCH counts the work in
between. Each milestone gets a section here, newest first.

## 0.4.11, 2026-11-18: middle-button pan and wheel zoom on a canvas

A patch release in the 0.4 line. `./rae toolchain pin 0.4.11` moves a project.

- A drag with the middle mouse button pans a canvas (ui/canvasGestureSystem), begun
  anywhere inside it, over a node too, and coasts when let go
  (`CanvasGestures.middleDown`).
- `CanvasControls.wheelZooms`: the mouse wheel zooms about the pointer with no modifier held
  (a sideways wheel still pans). Off by default (docs/ui-scene-format.md §41). Example 152
  uses both.

## 0.4.10, 2026-11-17: reliable messages no longer lost past the receive window

A patch release in the 0.4 line. `./rae toolchain pin 0.4.10` moves a project.

- lib/net's reliable channel could lose messages while the sender saw them delivered:
  the sender's window starts at its oldest unacked fragment, the receiver's at the first
  fragment of the oldest message it has not delivered, so a fragment beyond the receiver's
  window (after a message of several fragments, or while the app had not taken its
  messages) was refused and its packet still acked. Such a packet is now left unacked and
  the fragment comes again (`ReliableChannel.reliableIsAhead`, `Connection`). A run of one
  6-fragment message then 100 small ones through loss, reordering and duplicates stopped at
  59 on half the seeds; it now delivers everything on all of them.

## 0.4.9, 2026-11-17: smooth canvas zoom

A patch release in the 0.4 line. `./rae toolchain pin 0.4.9` moves a project.

- `CanvasControls.zoomEaseSeconds`: a wheel notch or a zoom key eases the camera to its
  target zoom, the point under the pointer staying put on every frame; `keyZoom` is the step
  of the new `=` / `-` zoom keys (`CanvasGestures.zoomKeys`); `pinchCoasts` lets a released
  pinch keep its zoom speed and slow with `friction`. All off by default
  (docs/ui-scene-format.md §41). Example 152 uses them.

## 0.4.8, 2026-11-17: zoom behavior per node

A patch release in the 0.4 line. `./rae toolchain pin 0.4.8` moves a project.

- `ZoomBehavior` (lib/ui/ZoomComponents, docs/ui-scene-format.md §44): under a Camera2D a
  node `Scales` with the zoom (the default), stays `Fixed` on screen, or is `Clamped`
  between two sizes, about a pivot; `showFrom` / `showTo` show it in a range of zooms with
  a cross-fade centered on each bound, so levels of detail cross-fade (or switch, with
  `fade: 1`). Applied by the transform pass.
- Example 152_zoomCanvas: a node canvas in the analytical and the zoomy feel.
- A UiWorld has one more table (`zoomBehaviors`).

## 0.4.7, 2026-11-17: line breaks in a Text

A patch release in the 0.4 line. `./rae toolchain pin 0.4.7` moves a project.

- A `"\n"` in `Text.text` is a hard line break (`"\r\n"` and `"\r"` count as one):
  each line one line height (the style's `lineHeight`) below the one before, aligned on
  its own; a `Hug` text measures the widest line and every line's height
  (docs/ui-scene-format.md §43). It used to draw every part on one line with nothing
  between them. A wrapping text treats `"\r\n"` as one break too.
- `SdfText`: `sdfHasHardBreak`, `sdfHardLines`, `sdfMeasureLines`.

## 0.4.6, 2026-11-17: text outlines in the theme

A patch release in the 0.4 line. `./rae toolchain pin 0.4.6` moves a project.

- A theme has `outline` tokens (`color`: a palette name or rgba, `width` in points,
  `extends`), and a text style names one with `"outline": "<token>"`, inherited through
  the style's `extends` (docs/ui-scene-format.md §42). The renderer draws the shadow,
  the outline, then the glyphs, from the font's distance field.
- A text style naming an `outline` or `shadow` token the theme does not declare is an
  `unknownToken` scene diagnostic (`ui/ThemeParse reportThemeTokenDiagnostics`, run by
  `app3d/UiShell` and the UI editor); before, a misspelled shadow was silently dropped.
- `lib/ui/ThemeParse` is a module folder now; imports are unchanged.
- Also since 0.4.5: an instance override may set `Layout.columns` / `gap`; a call that
  returns a reference cannot be the argument of a by-value parameter (bind it first).

## 0.4.5, 2026-11-17: saving edits into a frame

A patch release in the 0.4 line. `./rae toolchain pin 0.4.5` moves a project.

- `ui/FrameWriter` (new): `writeFrameEdits` saves edits made while a frame is shown
  into that frame's override bags (docs/ui-coordinate-and-responsive-layout.md §2): each
  edited component whole, out of the bag when it equals the base, a removed one in the
  bag's `remove` list. The rest of the file stays byte for byte.
- `ui/SceneWriter`: `writeSceneTextWith(..., edits: SceneEdits)` writes from the world
  only the components an editor names (`"<nodeId>/<Component>"`); every other one stays
  the author's text, so a camera's or an effect's runtime values are not saved.
  `writeSceneText` is unchanged in what it writes.
- Changed: a save keeps a header the author wrote after `nodes` (a `frames` or `states`
  block) after `nodes`; it used to move every header before them.
- The UI editor (121) saves an edit made in a frame into that frame.

## 0.4.4, 2026-11-17: frames picked by the whole window

A patch release in the 0.4 line. `./rae toolchain pin 0.4.4` moves a project.

- A scene frame's `when` (`ui/Frames`, docs/ui-coordinate-and-responsive-layout.md §3)
  names `minWidthPt` / `maxWidthPt`, `minHeightPt` / `maxHeightPt`, `orientation`
  (`portrait` / `landscape`), `pointer` and `inputType` (`touch` / `pointer`).
  `minPtW` still works as the older spelling of `minWidthPt`.
- The most specific matching frame wins (the most conditions, then the largest
  minimum width, then the smallest maximum width), then the one declared first; then
  the `default` frame. `forcedFrameId` overrides all of it.
- A frame may declare its reference `viewport: { "w": 393, "h": 852 }` in points
  (`FrameSet.viewportWs` / `viewportHs`).
- `FrameWindow { widthPt, heightPt, pointer }`, `selectFrameForWindow` and
  `applySceneFramesForWindow` pick and apply by the window's width and height. The
  width-only functions stay; with them a height or orientation condition does not hold.
- `lib/ui/Frames` is a module folder now; imports are unchanged.

## 0.4.3, 2026-11-17: one UI coordinate model, in points

A patch release in the 0.4 line. `./rae toolchain pin 0.4.3` moves a project
(and `./rae update physicsScenes` to 1.2.1). docs/ui-coordinate-and-responsive-layout.md
is the standard; its first rule changed on 2026-11-17 from 3 units per point to:

- **A design unit is a logical point**, the unit of iOS, Android (dp), CSS and Figma
  (`ui/DesignUnits`, `unitScale` 1). A phone is about 393 wide, a margin 16, body text
  17, a touch target 44. Every UI size, scene and theme is in points.
- **The layout fills the window** in points: a bigger window shows more, never a bigger
  UI. `app3d/UiShell` sets it (`syncDesignResolution`, `fitUiToWindow`,
  `resizeUiToWindow`); a program does not set its own design resolution.
- **A user zoom**, written `1.25x` (`UiWorld.uiZoom`, `ui/UiZoom`; Ctrl/Cmd + - 0 with
  `UiShell.handleUiZoomKeys`): the layout is the window / zoom, the pointer, safe area
  and size class follow.
- **A fixed art-directed area** (a game table, a playfield) is a stage: a `ScaleToFit`
  subtree in a scene (modes `Contain`, `Cover`, new `FitHeight`), or on the immediate
  canvas `Gpu2dCanvas` `stageFit` + `canvasPushStage` / `canvasPopStage` (new).
  Inside a scene's stage, `StageEdge` (new) pins a node to the visible or safe window
  edge or bleeds it over the whole window, and a `SafeArea` keeps the notch's size.
- `Overflow` / `OverflowPolicy` `ScaleToFitY` (with `minScale`) shrinks a column whose
  content is too tall, as Pixi's.
- `ui/ResponsiveExtent` (new): the window-filling layout of an app with a phone page
  column, by size class, with a side panel when expanded and a device preset emulated.

**What 0.4.3 changes for a program that ran on 0.4.2**

- lib/ui's fallback sizes are points: a text style its theme does not size is 17 (was
  43.37), `SheetPanel.rowHeight` 56 (was 121.44), `UiWorld`'s default layout 393x852.
- `DebugOverlay` has no `scale` and `setDebugOverlayScale` is gone; the perf HUD's draw
  functions lost their `scale` parameter. The user zoom enlarges them.
- `app3d/StaminaWheel.drawStaminaWheel` takes `camera:` (0.4.1).
- A scene that declares a unit (a top-level `"unitScale"`) is refused by the repository's
  gate; `tools/scale-ui-lengths.py --divide 3` converts a scene or theme authored at 3
  units per point (it reports any number it does not know).
- `lib/ui/Components` and `Gpu2dCanvas` are module folders now; imports are unchanged.

## 0.4.2, 2026-11-17: kinematic bodies stop at their pose

A patch release in the 0.4 line. `./rae toolchain pin 0.4.2` moves a project.

- A kinematic box3d/ecs body whose Transform3D is written once a frame now
  stops at that pose when the frame runs several fixed steps. The push gave it
  the velocity that reaches the pose in one step and left it there for the
  frame's other steps, so it overshot, and at 3 or more steps a frame swung to
  the world's top speed. `RigidBody` remembers the pose; on a step with no new
  pose the body is aimed at it again and stopped once there. A program that
  touched every kinematic Transform3D before each step to avoid this can stop.
- A kinematic body riding a platform can end elsewhere than before in a
  program that ran several steps a frame (the platform no longer overshoots).

## 0.4.1, 2026-11-17: a camera that rolls

A patch release in the 0.4 line. `./rae toolchain pin 0.4.1` moves a project.

- `Camera3d` has a `roll`: radians about the view direction, the right side
  down above 0 (a drone banking into a right turn). It defaults to `0.0`, and
  at 0 every view is exactly what it was.
- `Scene3d.cameraViewMatrix(camera:)` is the camera's view matrix with its
  roll; the deferred and forward frames and the underwater fog build their view
  with it. `Math3d.viewUp(eye:, target:, roll:)` is the up reference it hands
  `mat4LookAt`.
- `app3d/WorldLabel.projectCameraPoint(camera:, aspect:, point:)` projects a
  point the way the frame is drawn, roll included: use it for labels and a
  reticle. `projectToScreen(eye:, target:, ...)` stays the unrolled form.
- Changed: `app3d/StaminaWheel.drawStaminaWheel` takes `camera:` in place of
  `eye:`, `target:` and `fovYDeg:`.
- Unchanged with a roll, on purpose: the shadow cascades (spheres round the
  view axis) and the camera-facing leaf cards and water drops, which stay
  upright in the world.

## 0.4.0, 2026-11-17: module folders

Named on 2026-11-16 and built in the 0.3 line from 0.3.173; the 0.3 line ended
at 0.3.178 (tag `v0.3.178`). docs/module-model-options.md is the design. A
project moves with `./rae toolchain pin 0.4.0`, then takes its packages' new
versions with `./rae update <name>` (physicsScenes 1.2.0, qr 1.1.0).

**What 0.4.0 refuses that 0.3.x took**
- An import or qualifier whose upper and lower case differ from the file or
  folder on disk (`import vec3`, `import sys`, `math.sqrt`, `sdl3.isKeyPressed`,
  `Particles.tick()` for the folder `particles/`): write it as on disk
  (`import Vec3`, `Math.sqrt`, `particles.tick()`). These worked on macOS only.
- A file and a folder of one name in one place (`Hud.rae` beside `Hud/`), at
  any depth: keep one, or move the file into the folder.
- An import of a file inside a module folder (`import Gpu2d/Window`): import
  the module (`import Gpu2d`).
- The same function with the same parameters in two files of one module.
- `Gpu2dImages.` and `import Gpu2dImages`, which existed in 0.3.172 only: write
  `Gpu2d.` and drop the import.
- The old imports of the ui system modules that became folders; each folder is
  one module now, and a call qualified by one of its files' names takes the
  module's name (`RenderFills.paintGradient` -> `RenderSystem.paintGradient`):

  | 0.3.x import (every file of the folder) | 0.4.0 import |
  |---|---|
  | `ui/renderSystem/RenderSystem`, `RenderFills`, `RenderSprite`, `RenderCull`, `RenderText` and the 13 others | `ui/RenderSystem` |
  | `ui/animationSystem/...` (8 files) | `ui/AnimationSystem` |
  | `ui/buttonSystem/...` (4 files) | `ui/ButtonSystem` |
  | `ui/pageTransitionSystem/...` (3 files) | `ui/PageTransitionSystem` |
  | `ui/inputSystem/...` | `ui/InputSystem` |
  | `ui/layoutSystem/...` | `ui/LayoutSystem` |
  | `ui/listMotionSystem/...` | `ui/ListMotionSystem` |
  | `ui/listReorderSystem/...` | `ui/ListReorderSystem` |
  | `ui/listViewSystem/...` | `ui/ListViewSystem` |
  | `ui/scrollRootSystem/...` | `ui/ScrollRootSystem` |

  The one-file system folders (`ui/dialogSystem/DialogSystem`, the lib/ecs
  systems and the rest) keep their import paths.

**Modules**
- A module is a PascalCase file or a PascalCase folder (0.3.174): `import
  Gpu2d` finds lib/Gpu2d.rae or the folder lib/Gpu2d/, whose files are one
  module. A module that reaches the 1,000-line cap becomes a folder with no
  change to the code that uses it. Refused: a file and a folder of one name, an
  import of a file inside a module folder, the same function in two files of
  one module. A program's PascalCase folders are module folders too (0.3.178),
  seen with no import like every program file.
- Imports and qualifiers are spelled as on disk, with the same upper and lower
  case, on every system (0.3.173): `import vec3` and `math.sqrt` are refused
  with the right spelling. They used to work on macOS only.
- lib/Gpu2d is a module folder (0.3.175), and its image functions are back in
  it: `Gpu2d.loadImageKey`, `Gpu2d.drawImageKey` and the rest.
  **`Gpu2dImages.` existed only in 0.3.172**; code written against it goes back
  to `Gpu2d.` and drops `import Gpu2dImages`.
- The ui system folders that held several modules are one module each
  (0.3.177): `import ui/RenderSystem` replaces `import ui/renderSystem/RenderSystem`
  and its 17 sibling imports, and `RenderFills.`, `RenderSprite.`, `RenderCull.`
  calls are `RenderSystem.`; likewise ui/AnimationSystem, ButtonSystem,
  PageTransitionSystem, InputSystem, LayoutSystem, ListMotionSystem,
  ListReorderSystem, ListViewSystem and ScrollRootSystem. physicsScenes 1.2.0
  and qr 1.1.0 use them.
- The rules follow (0.3.176): AGENTS.md says a module is a PascalCase file or
  folder, a system is a module of its own, and a module at the line cap becomes
  a folder; "package" means a `.raepack` package only. `rae init` writes these
  module rules into a new project's AGENTS.md. `run_cases.sh --grep` given a
  file of a module folder searches for its module; `--list` prints the cases.

## 0.2.0, 2026-10-11: physics and servers

The first milestone after 0.1.0 (2026-09-12). The 0.1 line ended at 0.1.268
(tag `v0.1.268`). A project whose `.raepack` requires `rae: { version: "0.1" }`
(or any `0.1.x`) must change it to `"0.2"`.

**Physics**
- Box3D's C library is Rae's physics engine, through generated bindings
  (`lib/box3d`) and an ECS layer (`lib/box3d/ecs`), with examples for the
  playground and a driven vehicle. The Rae port of Box3D (`lib/physics`) stays
  as a frozen reference and the portable fallback
  (docs/physics-two-implementations.md).
- A physics scaling benchmark for both tracks (`benchmarks/physics_scaling`).

**Networking and servers**
- `lib/net`: TCP sockets, a readiness poller (kqueue on Apple and BSD, epoll
  on Linux) and byte buffers, all Rae over one-call system shims.
- `lib/http`: an HTTP/1.1 parser and response encoder in plain Rae.
- `lib/webSocket`: the upgrade, frame decoder and encoder in plain Rae.
- `lib/net/ecs`: HTTP and WebSocket servers as ECS systems.
- Server benchmarks against tuned Rust, Node and Bun (`benchmarks/servers`,
  and three tabs in the devtools Benchmarks view): Rae is in the same range,
  ahead on single-core HTTP throughput.

**Tasks and concurrency**
- Spawned tasks run M:N on the worker pool, scheduled in Rae; `get()`,
  `sleep` and socket waits suspend a task instead of holding a thread.
- `blocking` externs run a long C call on its own thread.
- A spawn inside a `taskScope` lends a view instead of copying it.
- In the browser, waiting tasks run between `mainLoop` frames.

**Language**
- `const` / `let` / `var`, with a `let` frozen as a whole; `if var`.
- Compile-time `when` and the `Target` prelude for per-platform code;
  `rae build --check-targets`.
- `match let`, `equals`, `enumName` / `enumMembers` / `enumCount`, `toString`
  for lists, optionals and structs, one-UTF-8-character `Char` literals.
- SIMD: `Float4` masked tails and `Float8` (AVX2 on x86-64).
- Many fixes in generics, ownership and drops, and sema checks that reject
  wrong members and qualifiers.

**Toolchain**
- The Live / VM target is gone: the C backend is the one target.
- `rae format` and the build's format preflight own the layout.
- Projects declare the Rae they need; `rae toolchain`; dependencies with
  `rae add` / `fetch` / `update` / `tree`.
- The compiler builds with -O2 and LTO, and emitting is linear.
- Browser builds of WebGPU apps, with WASM SIMD and a frame-callback loop.
- The generated C compiles without warnings; gates for TSan, ASan, banned
  terms and stress cases run before every suite.
- Runtime logic moved from C to Rae (strings, JSON, time, logging); C is kept
  for platform calls, and a faster platform implementation wins where one
  exists (CommonCrypto SHA-1).
