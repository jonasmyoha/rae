# Release notes

A release is a git tag (docs/versioning-and-toolchain.md §1). Before 1.0 the
MINOR number is a named milestone; PATCH counts the work in
between. Each milestone gets a section here, newest first.

## 0.4.39, 2026-11-23: a turned or flipped clipping node paints its children

- A node that clips its children (`Overflow` clip, a card) and is turned by its `TransformFx`
  clips them to the window-space box round its turned box (`turnedWorldRectOf`), with rounded
  corners while the turn keeps it upright or upside down. The clip was its unturned box placed at
  its turned origin, which past a quarter turn no longer meets the card, so nothing of its
  content was drawn.
- A node flipped by a negative scale paints its children: the "scaled down to a speck" check
  took scale -1 for tiny. Every box is queued with a positive size (a negative width or height is
  the same box from its other corner, its corner radii swapped). Case 1568.

## 0.4.38, 2026-11-23: nine-slice borders shrink to fit

- A nine-slice (`canvasDrawImageKeyNineSlice`, a Sprite's `nineSlice`) in a box smaller than two
  of its borders scales all four together, s = min(1, w / (left + right), h / (top + bottom)),
  as Pixi's NineSliceSprite and CSS border-image do (`nineSliceBorders`). Each border used to be
  clamped to half the box alone, so a ribbon with 256-px caps drawn 52 high got caps of half its
  width and no middle. In a box that fits the borders nothing changes. Case 1567.

## 0.4.37, 2026-11-23: a loop over a temporary's list reads all of it

- `loop let point: view Point in outlineOf(count: 5).points` reads every element: the struct a
  call in a loop's header returns now lives for the whole loop, as a `let` would keep it. It was
  dropped at the end of the first iteration (and by a `continue`), and the loop read the first
  element and then freed memory. It is dropped once after the loop, or by a `break` or `ret`.
  Case 1566.

## 0.4.36, 2026-11-23: a hidden child no longer stretches a scroll

- `ScrollRoot`'s content extent (`scrollContentExtent`) counts only the children the layout
  places: a child switched off (`Active false`) keeps its last box, and the scroll went on as far
  as before, over empty space (a page whose sections a filter tab hides). Case 1565.

## 0.4.35, 2026-11-23: a List of List can be set

- `set`, `remove`/`removeAt`, `swapRemove` and the like on a `List(List(T))` (or a List of
  `StringMap`/`IntMap`) compile: the element they replace or take out is dropped by its
  container's own `drop`, where the C called a struct drop helper no List has and failed with
  "call to undeclared function 'rae_drop_struct_rae_List_...'". Case 1564.

## 0.4.34, 2026-11-23: UiShell runs fitSystem

- `app3d/UiShell`'s `runUiSystems` and `runScheduledUiSystems` run `fitSystem` between the layout
  and the animation step (example 106's order; ui/Pipeline's `uiSystemFit`). Without it, in every
  app on UiShell and in the 121 viewer, a `ScaleToFit` stage, `Aspect` and Overflow
  `scaleToFitY` did nothing: a 1080x2280 stage drew unscaled. Case 1563.

## 0.4.33, 2026-11-23: a layout check hides a node under an inactive parent

- `app3d/LayoutCheck`'s `layoutBoxOfEntity` (and `layoutBoxOf`) report a node hidden when any of
  its ancestors is inactive. Such a subtree is not laid out or transformed, so its nodes kept the
  boxes of an earlier pass and came back shown at a wrong place, also when read right after the
  parent's `Active` changed, before the next transform pass. Case 1562.

## 0.4.32, 2026-11-23: a scene value the decoder drops says so

- A number field of a scene written as `true` / `false`, a list or an object is a
  `wrongValueKind` scene diagnostic naming the key and the value used, where it was silently 0
  (`SafeArea`'s `"apply": {"t": true}`, the older format's spelling, applied no safe inset). A
  `null` stays the default with no diagnostic.
- `Constraints` clamps by a bound when the scene writes it: `"maxW": 430` alone sets
  `hasMaxW`. A written `has*` key still decides (`"hasMaxW": false` keeps the bound off).
  Case 1561.

## 0.4.31, 2026-11-23: tabs

- `Tab` and `TabPage` (`ui/tabSystem`, docs/ui-scene-format.md section 45): a group of tabs shows
  one page at a time. A tap on a tab selects it, the group's page of its id is active and the others
  are not, and the tab's fill takes its `selectedFill` slot; a group opens on its first tab.
  `tabSystem(world:, actions:)` each frame after input; `selectTab` and `selectedTab` for the app.
  Case 1560. Asked by the media editor's side panel.

## 0.4.30, 2026-11-23: a window goes fullscreen and back

- `Gpu2d.setWindowFullscreen(on:)` and `Gpu2d.isWindowFullscreen()`: SDL3's borderless desktop
  fullscreen, on macOS the window's own fullscreen Space (for a video editor's or player's F key).
  The flag changes at once; the new size comes with the usual `windowResized()` once the OS has
  moved the window (about 0.8 s on macOS), and leaving gives back the size and place the window
  had. A headless window only keeps the flag (case 1559).

## 0.4.29, 2026-11-20: WASM builds get a real stack

- `compiler/tools/wasm_build.sh` links an 8 MB stack (`RAE_WASM_STACK_BYTES`), for wasip1 and the
  threads target; a WASM module from `rae build` gets 1 MB; lib/wasm's `wasmDefaultLimits` gives an
  instance 1 MB of WAMR stack (was 64 KB). wasi-sdk's 64 KB stopped programs whose calls went a
  little deeper with "memory access out of bounds" (docs/wasm-modules.md, "The stack").
- `compiler/tools/wasm-stack-check.sh`: a pre-suite check that a recursion of about 400 KB of stack
  runs under Node.

## 0.4.28, 2026-11-20: `+=` and `-=`, and a loop that steps by more than one

- `x += e` and `x -= e` are statements (`x = x + e`, `x = x - e`) on a name or a field, numbers and
  Strings alike; a `let` cannot take them (docs/SYNTAX_RULES.md §4).
- A counted loop's step is `++i`, `--i`, `i += n` or `i -= n` on the loop's own number variable:
  `loop var degrees: Int = 0, degrees <= 90, degrees += 5 { }`. Anything else as the step is now an
  error (a call there used to compile and do nothing, so the loop never ended).

## 0.4.27, 2026-11-20: the walkers' keys are the world's

- The walkers' keys (WASD, arrows, Shift, jump, R, V, Page Up / Page Down) read through
  `ui/WorldInput` `worldKeyDown` / `worldKeyPressed`: they stop while a text field has focus or a
  modal shows, and no longer while the pointer only hovers the UI. `inputUpdate`, `jumpPressed`,
  `walkerResetKeyPressed` and `walkerSpawnHereKeyPressed` take the captures (breaking for a
  program that called them).
- `compiler/tools/world-input-gate.sh`: a pre-suite gate against device mouse and key reads in
  lib/app3d and packages outside its allowlist.

## 0.4.26, 2026-11-20: the world's input after the UI's

- `ui/LayerCapture`: `worldPointerStep`, `worldPointer(captures, id)` (down, pressed, released,
  place, canceled) and `worldKeys(captures)`: what the world layer gets after the layers above
  took theirs. A press on a button, or under a modal, never reaches the world, even dragged onto
  it; a modal opening over a world press ends it.
- `ui/WorldInput`: `feedWorldInput` (the device into the captures, after the layer walk),
  `worldKeyDown`, `worldKeyPressed`. The walkers feed it in `hudCapturesStep`.
- `CameraRig.updateCameraRigFor` with a `CameraRigInput` (the pointer and the keys apart;
  `updateCameraRig` stays): the walker camera's keys work while a UI button is held and stop while
  a text field has focus (app3d/HudOverlays `cameraRigInputFrom`).

## 0.4.25, 2026-11-19: reading the frame at a layer's end

- `gpu/FrameRegionReader` `frameRegionCopyNow`: copy a rectangle of the frame in the middle of
  it (the 2D flushed so far, the canvas's pass ended and started again), with the frame's own
  submission: the picture holds only what was painted before that point.
- qr 1.3.0: a `QrScanner` with `afterLayer` (`"world"`) reads at that layer's end through the
  plan (`qrScanBeforeStep`), so a code on the HUD is not read and a dialog over the middle does
  not hide the world's codes; `qrScanNow` for a program with no plan (docs/qr.md).

## 0.4.24, 2026-11-19: entities owned by peers, handed over at run time

- `net/Ownership`: an owner per shared entity that changes while the game runs. The host asks the
  owner, the owner sends its state to the new owner in one message and keeps simulating until
  the new owner takes it, and the host switches at once: every tick of an entity is one peer's,
  and its state goes on without a jump. A leaving peer's or host's entities go to the host
  (`ownershipPeerLeft`, `ownershipBecomeHost`); `ownerByNearest` is the usual rule; messages go
  as bytes over a walker session's game messages (docs/walker-net.md).

## 0.4.23, 2026-11-19: simulation levels round relevance centers

- `simulation/SimLevelSystem`: a subject (an entity id with a `SimRelevance` component) is near
  or far by its distance to a list of centers, with an enter and a larger leave distance, and
  `CameNear` / `WentFar` events the app answers (build the full entity, or keep only its record).
- `simulation/StandingSpot`: a free place to stand near a point, on the app's ground, clear of its
  colliders, within a radius, and out of a camera's view when one is free; the app answers each
  candidate. docs/simulation-levels.md (with TileStreaming's centers).

## 0.4.22, 2026-11-19: makeBox shows pictures upright

- `Mesh3d.makeBox`: a picture on any face now reads upright and the right way round: u runs to
  the right as seen from outside the face and v is 0 at its top (where the shader reads a
  picture's first row). Before, the side faces showed a picture upside down (-X and -Y mirrored,
  +X and +Y turned half round), and so did the top. A program that drew a picture made to look
  right on the old faces sees it flipped now; QR codes on cubes (example 147, the walker's QR
  cubes) now read like the code itself.

## 0.4.21, 2026-11-19: a rounded box mesh

- `mesh/RoundedBox`: a box with its vertical edges rounded (`radius`, `segments`) and a flat,
  sharp top and bottom, standing on z = 0: `makeRoundedBox` (one mesh) and
  `makeRoundedBoxParts` (the body and the four sides as meshes of their own, so a side can have
  its own material). Each side is a 0..1 UV square that shows a picture upright. Example 147 has
  a machine with its QR code on two sides.

## 0.4.20, 2026-11-19: UI layers, released

A patch release in the 0.4 line that closes the UI layers work (docs/ui-layers-design.md):
everything 2D in a window is painted and hit in one order, the window's layer stack. It sums up
0.4.15 to 0.4.19 and the layer steps since 0.4.14. `./rae toolchain pin 0.4.20` moves a project;
a program on the removed names below moves first.

- **Input through the plan and the claims.** `ui/LayerCapture`: a press is captured in layer order
  until it is let go; a modal layer or draw that opens over it cancels it (`PointerCanceled`); a
  press outside a `dismissOnOutsideTap` layer is reported (`LayerTappedOutside`); the camera, the
  touch stick, the scroll root and a game's gestures ask `uiClaimsPointer` / `uiClaimsTouch` /
  `uiClaimsKeys` first. `uiHitTest` and the walkers' input follow the same order; a shown dialog
  holds the presses under it.
- **Order inside a layer.** `LayerTag { id, z }`, `ZIndex` among siblings (`ui/ZOrder`), raised
  nodes that keep their parent's place but take no ancestor opacity, `reparentKeepingPlace`,
  `LayerScope` keeping a subtree's layers to itself, a scene's own layers, and a node's z set at a
  motion's keys (`ui/ZKeys`).
- **Effects and page transitions.** Safe rectangles, draw areas as clips, world anchors sorted far
  to near, the backdrop blur per layer, isolated layers composited once at their opacity, and page
  transitions painting both worlds by one plan.
- **The moved programs.** The "..." menu on the dialog layer, the editor panel and the walkers'
  overlays by a plan, 106 and 121 on the stack, the pong and tetris boards as program draws under
  the scene UI.
- **LayerTag is the one link from a node to its layer** (0.4.17, breaking). Removed: `LayerRoot`,
  `LayerRef`, `collectPaintableRoots`, `createStandardLayers`, `createLayerEntity`,
  `findLayerEntity`, `entityLayerOrder`, `raiseTaggedNodes`, `layerDialogOrder`,
  `gpu2dLayerRange` and `ui/LayerLegacy`; "content" and "back-button" are now "hud" and "dock".
- **Seeing the layers.** The debug overlay's Layers page (`ui/DebugLayers`), `layerReportRows`, and
  121's Layers panel with inspector edits of LayerTag, ZIndex and CustomPaint (0.4.18).
- **Measured** on an iPhone 15 Pro against the commit before the layers: the same frame times, CPU
  time and draw calls (0.4.19, `RAE_GPU2D_DRAWS=1`).
- Packages: qr 1.2.0 (a QrCode node painted in its place in the order, needs Rae 0.4.13).

## 0.4.19, 2026-11-19: the UI layers measured on a phone

- `RAE_GPU2D_DRAWS=1`: the 2D canvas logs its draw calls a frame (docs/profiling.md).
- The UI layers measured on an iPhone 15 Pro against the commit before them: the same frame times,
  CPU time and draw calls for 106 and a walker (docs/ui-layers-design.md §11).

## 0.4.18, 2026-11-18: the layers in the debug overlay and the UI editor

- The debug overlay's dialog has a Layers row: the window's layers in paint order with what is
  in each (`ui/DebugLayers` `feedDebugLayers`, `RAE_DEBUG_LAYERS=1`). The walkers feed it.
- `ui/LayerReport` `layerReportRows`: the report as rows, each with its layer, node or draw.
- `componentNamesFor` lists `LayerTag`.
- Example 121: a Layers panel (L) for the edited document, and inspector edits of `LayerTag`,
  `ZIndex` and `CustomPaint`.

## 0.4.17, 2026-11-18: LayerTag is the one link from a node to its layer

A patch release in the 0.4 line, breaking. `./rae toolchain pin 0.4.17` moves a project.

- A node names its layer only by `LayerTag { id, z }` (a .raescene's `"LayerTag": { "id":
  "dialog" }`, or `setLayerById` / `raiseNodeToLayer` in code, which set the tag and keep its z).
  The name is looked up in the window's `LayerStack`; a root without a tag is in hud, and a name
  the stack has not is painted in hud. A node inside a scene whose tag names a layer is raised
  (`isRaisedNode`), unless it is below a `LayerScope`.
- Removed (breaking): the layer entities `LayerRoot` and `LayerRef` (`UiWorld.layerRoots`,
  `layerRefs`), `createStandardLayers`, `createLayerEntity`, `findLayerEntity`,
  `entityLayerOrder`, `collectPaintableRoots`, `raiseTaggedNodes`, `layerDialogOrder`,
  `gpu2dLayerRange`, and `ui/LayerLegacy` (`adoptWorldLayers`, `stackOrderOfLegacy`).
  `raiseNodeToLayer` takes no `order`. A program that made its own layers either tags its roots
  with the standard names (121: the document in hud, its guides in hudOver, the chrome in dock) or
  adds its layers to the stack it keeps (`addLayer`). The old names "content" and "back-button"
  are no longer read as hud and dock: write "hud" and "dock".
- `uiHitTest` walks the roots in the order of the standard stack's plan (`rootsInPaintOrder`).
  A layer contribution notices a LayerTag that changed its layer or z on its next refresh.

## 0.4.16, 2026-11-18: a screen tint

- `setScreenTint(cache: renderer.passes.transparent, r, g, b, strength)`: a color over the whole
  lit frame after the transparents (one full-screen triangle; nothing drawn at strength 0).

## 0.4.15, 2026-11-18: the transparent pass takes several meshes a frame

- `lib/TransparentForward` draws each run of one mesh in the order queued (`transparentRuns`);
  style 4 is a shimmering surface (`addTransparentShimmerInstance`, `TransparentShimmer`); the blend
  is premultiplied, so a style can be additive. `Mesh3d.makeOpenCylinder`. Styles 2 and 3 (the
  explosions' puff and glow) now run as designed.

## 0.4.14, 2026-11-18: the 3D shell's overlays by the layer plan; HudOverlay removed

A patch release in the 0.4 line. `./rae toolchain pin 0.4.14` moves a project.

- `app3d/HudOverlays`: the shell draws everything 2D by the window's layer stack:
  `HudLayerPaint` (`createHudLayerPaint`, `drawHudLayers`, `buildHudLayerPlan`) puts the world's
  labels in the world layer (game mode), the scene UI's roots in their layers, the log strip in
  the log layer (editor mode) and the debug readout in the debug layer. An app's own drawing is a
  `LayerDraw` it adds. The walkers keep it on their `WalkerRender`.
- Removed (breaking): `ui/HudOverlay` (`HudOverlay`, `OverlayDraw`, `OverlayShows`,
  `overlaysInOrder`, `UiWorld.hudOverlays`) and `addStandardHudOverlays`, `drawRaeOverlay`,
  `drawOverlays`. A program on them moves to a `LayerDraw` in its layer.
- `setLayerShows` paints a layer in one mode only.

## 0.4.13, 2026-11-18: UI layers, the first steps

A patch release in the 0.4 line. `./rae toolchain pin 0.4.13` moves a project. The first steps of
docs/ui-layers-design.md (one paint and input order for everything 2D); the rest is queued.

- `ui/UiLayers`: one LayerStack per window (the standard layers world, hud, hudOver, dock, popover,
  toast, dialog, cover, notification, log, debug; a program's own in a gap), several UI worlds
  joining it, each world's contribution, and the paint plan (`buildLayerPaintPlan`). `LayerTag`
  gains `z`; a scene's LayerTag now raises its node by itself (`raiseTaggedNodes`).
- `renderSystemGpu2d` paints by the plan; `Gpu2dCanvas` has a layer push (`canvasPushLayer`:
  opacity and offset). `collectPaintableRoots` sorts stably.
- `ui/LayerHit`: a hit walk down the plan over every world (surfaces, modal layers and draws, the
  layers a press missed, drop targets).
- `ZIndex` orders siblings in paint and hit (`ui/ZOrder`).
- A cover layer hides what is under it; `ui/LayerMotion` eases a layer's opacity and offsets.
- `CustomPaint`: a node the program draws in its own place in the order (plan steps,
  `beginCustomPaintG` / `endCustomPaintG`); `ui/LayerReport` lists what is drawn in which layer.
- qr 1.2.0: a QrCode node is a CustomPaint of kind `qrCustomPaintKind`, painted by
  `qrCodePaintStep` in its place (under a dialog over it); `qrCodePaint` still works.

## 0.4.12, 2026-11-18: tile streaming with a primary center

A patch release in the 0.4 line. `./rae toolchain pin 0.4.12` moves a project.

- app3d/TileStreaming `streamTilesPrimary` and `TileGridSettings.priority` (TilePriority): the
  first center (a fast camera) gets the slots first, round it and where it is heading; the other
  centers keep a smaller reach; a full set of slots gives up its least needed tile to a missing
  one needed clearly more. Off by default (docs/open-world-tiles.md).
- `streamPropTiles` takes `deltaTime` (breaking: one more argument); the walkers' forest streams
  with the camera's focus primary.

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
