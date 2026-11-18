# UI layers: one paint and input order for everything 2D

Status: design (2026-11-18), revised the same day after a review against every local Rae
program (§15). The build is queued in `QUEUE.md` right after the design task, as "ui layers
1/8" to "8/8". Breaking changes are allowed; the UI stays ECS.

## 1. The problem

Rae has two orders today, and they do not meet:

1. **Inside one scene UI world**, `ui/Layer`: `LayerRoot { id, order }` entities
   (`createStandardLayers`: content 0, dock 100, back-button 200, dialog 300, debug 400), a
   `LayerRef` on a root, the authored `LayerTag { id }` that `layerTagSystem` turns into one, and
   raised nodes (`raiseNodeToLayer`, used by `app3d/ModeMenu`'s "..." menu). `collectPaintableRoots`
   sorts the roots by their layer's order every frame, and the paint and the hit test walk that.
2. **Between whole blocks**, `ui/HudOverlay`: `HudOverlay { draw, order, shows }` entities (world
   labels 10, the whole scene UI 20, log strip 80, debug readout 90, `OverlayDraw.app` for an
   app's own), drawn by `app3d/HudOverlays.drawOverlays`. Programs without that shell order their
   blocks by the order of their calls.

The whole scene UI, with its dialog layer, is one block. So anything a program paints on the
`Gpu2dCanvas` itself is either under every scene node or over every scene node, dialogs included.
The review found this in every program that draws anything itself:

- Game proto2's compass and kill feed were drawn over the open "..." menu. Its map and its QR codes
  are still drawn after the whole frame, over that menu. One QR code belongs inside a button.
- Example 147 paints its QR code node after the whole scene UI, so it would show over a dialog.
- The media editor draws its whole canvas and viewer after the scene UI, inside a node's
  rectangle, so no scene node (a future inspector or menu) can go over them.
- SUMU paints five UI worlds in a fixed call order. It fixes up input afterwards: notifications
  cancel the screen's press after the screen has seen it (`suppressUiPress`).

Input has the same split. The scene's hit test walks the sorted roots of one world, top first. The
camera's touch turn, the touch stick, keys, a program's own drawn controls and the other UI worlds
each check their own things (`presentedPageBlocks`, a `mapOpen` flag, `uiPointerCaptured`, the
stick's zone, a chain of "took the press" booleans).

## 2. What the canvas already gives

`Gpu2dCanvas` is an immediate, painter's-order stream. Every draw call queues a box, an image or a
text glyph run. `canvasKeepOrder` flushes only when an item would land under what is already
batched: one flush draws its boxes, then its images, then its text, font by font. So:

- **The order of the calls IS the paint order**, across everything drawn on the canvas in a frame:
  every UI world, world labels, the log strip, a program's compass.
- Two draws of the same kind in a row share a batch, whoever made them. A layer boundary costs
  nothing by itself; only the kind changing costs a flush.

The design builds on this. Nothing in it needs a depth buffer or a render target per layer.

## 3. Decision: one stack of layers, one paint plan, painted in order

| Approach | Verdict |
|---|---|
| **A. One stack of layers per window; per frame a paint plan in layer order, made from every UI world and every program draw; each step draws on the shared canvas in order** | **Chosen.** Uses what the canvas does now. Alpha blending is right because the order is right. Batches carry across layer boundaries. No new GPU state. |
| B. One sorted draw list `(layer, z, clip, pipeline)`, collected from every source, sorted, then drawn | Sorting by pipeline inside a layer reorders overlapping translucent items, which is wrong without depth. Every source would have to emit into a retained list instead of drawing: a second API for programs. The canvas already batches in order, so the win is small. |
| C. An offscreen target per layer, composited | One full-screen RGBA target per layer is about 30 MB at 2556x1179 for eight layers, plus a composite pass each. Too costly on a phone. Kept only as an opt-in for one layer (§8, `isolated`). |
| D. A depth buffer, z per item | Translucent UI (text edges, shadows, scrims) still needs back-to-front order. Depth would only help opaque boxes, which are not the cost. |

Rae has no function references, so a program cannot hand a callback to a layer. As with a
`Schedule` and with today's `overlaysInOrder`, **Rae owns the order and the program calls the
draws**. Each frame the program walks the plan. A Rae step draws itself (`paintStepRae`), and a
program's step comes back to the program with its kind. The 3D shell (`app3d/HudOverlays`) does this
walk for the standard kinds.

## 4. The model

### 4.1 The stack belongs to the window, not to a UI world

A program can have several UI worlds in one window:
- SUMU has a background world, a screen world that is replaced on every screen change, a top bar,
  a screensaver and notifications, plus the outgoing screen world during a page transition.
- The 3D editor panel (`app3d/EditorPanel`) is a second world over a game's own.
- Example 121 keeps an edited document and the editor around it.

So the stack is a resource of the window, held by the program next to its canvas:

```
type LayerStack {                   # one per window (a resource on the App)
  layers: List(UiLayer)             # sorted by order
  generation: Int                   # a layer added or removed
}

type UiLayer {
  name: String                      # "hud", "dialog", a program's "minimap"
  order: Int                        # lower paints first; standard layers are 100 apart
  modal: Bool = false               # while it shows anything, input below it is blocked (§7)
  dismissOnOutsideTap: Bool = false # a press outside what it shows sends LayerTappedOutside (§7)
  hidesBelow: Bool = false          # while it shows anything, the layers under it are not painted
  shows: LayerShows                 # always | gameMode | editorMode (was OverlayShows)
  opacity: Float = 1.0              # multiplies every draw of the layer (§8)
  offsetX: Float = 0.0              # moves every draw of the layer, in design units (§9)
  offsetY: Float = 0.0
  safeArea: Bool = false            # its clip and its full-screen roots keep inside the safe
                                    # area and the host's reserved insets (§10)
  isolated: Bool = false            # opt-in: drawn to an offscreen target, composited once
}
```

Each UI world joins the stack. `layerStackAddWorld(stack, worldKey)` names it with a small `Int`
key, and the key orders worlds that share a layer: the outgoing screen under the incoming one, and
a program's main world under the editor panel. A world's nodes name their layer as they do now. The
world keeps, per layer name, its roots sorted by `z` (§6): its **contribution**. Plan building
merges the worlds' contributions, which are already sorted. That is a merge, not a sort.

### 4.2 Components in a UI world

```
type LayerTag {          # authored in a .raescene (the key exists today) and set by code
  id: String             # the layer's name
  z: Int = 0             # order inside the layer (§6)
}

type OnLayer {           # runtime: the resolved layer (replaces LayerRef)
  layer: Int             # index into the window's LayerStack
  z: Int
}

type CustomPaint {       # this node's content is drawn by the program, in the node's place (§5)
  kind: Int              # the program's own meaning (its enum, as an Int)
  hit: Bool = false      # the program also hit-tests inside it (a surface, §7)
}
```

And in the stack, for what is drawn but is not a node:

```
type LayerDraw {         # one program or Rae draw in a layer
  layer: Int
  kind: LayerDrawKind    # worldLabels | logStrip | debugReadout | app
  z: Int = 0
  appKind: Int = 0
  hit: Bool = false      # it also hit-tests, as a surface, inside `area`
  area: Rect             # where it draws (the whole layer by default); its clip
  modal: Bool = false    # it blocks input under it while active (a full-screen map)
  active: Bool = true
}
```

### 4.3 The standard stack

`createStandardLayers(stack)` makes these. A program adds its own in a gap
(`addLayer(stack, "minimap", order: 250)`, or `layerOrderAbove(stack, "hud")`) and never needs the
other numbers.

| order | name | holds | flags |
|---|---|---|---|
| 100 | `world` | world-anchored text and bars over the 3D view, depth-sorted (§5.3) | |
| 200 | `hud` | the default layer: page content, a game's HUD, the touch stick | |
| 300 | `hudOver` | over the HUD: a full-screen map, a shade | |
| 400 | `dock` | the dock, toolbars, the back button, a top bar | `safeArea` |
| 500 | `popover` | non-modal popovers, tooltips, dropdowns, a radial menu | `dismissOnOutsideTap` |
| 600 | `toast` | toasts under a modal dialog | `safeArea` |
| 700 | `dialog` | dialogs, sheets, context menus, the "..." menu | `modal`, `dismissOnOutsideTap` |
| 750 | `cover` | a screensaver, a lock screen: full screen over dialogs | `modal`, `hidesBelow` |
| 800 | `notification` | system notifications: over dialogs, still pressable | `safeArea` |
| 850 | `log` | the log strip (editor mode) | |
| 900 | `debug` | the debug readout, the debug button and its menu | |

A node with no layer is in `hud`, as a root with no `LayerRef` is in `content` now. 106's
"content" becomes "hud". The name means the main layer, not a game term.

A toast goes under a dialog and a notification goes over one. That difference is why both exist.
The cover's first press is taken by `modal`. Its `LayerTappedOutside` (a press on nothing it shows,
which for a full-screen cover is any press) is how a screensaver closes.

### 4.4 The paint plan

```
type PaintStep {
  layer: Int
  kind: PaintStepKind   # layerBegin | layerEnd | sceneRoot | sceneRootPart | customPaint
                        # | worldAnchor | draw
  world: Int            # the world's key (scene steps)
  entityId: EntityId    # the root, the CustomPaint node, or the LayerDraw's entity
  appKind: Int          # a draw's or a CustomPaint's kind
  part: Int             # which part of a root split at CustomPaint nodes (§5.1)
}
```

`layerPaintPlan(stack, contributions, editor)` returns the steps in order:
- Within a layer, scene roots and `LayerDraw`s are sorted by `z`.
- At an equal `z`, draws come before roots, so they paint under them. A compass and a kill feed at
  the layer's default `z` sit under the HUD's buttons, which is what game proto2 needs. A draw that
  must go over sets a higher `z`.
- Roots at an equal `z` keep the world key's order, then mount order.

`layerBegin`/`layerEnd` frame each layer that has anything to show. They set the layer's
opacity, offset and clip on the canvas (`canvasPushLayer` / `canvasPopLayer`). A step carries the
layer's rectangle, its safe area included, so a draw knows where it may go. A program that captures the
frame (game proto2 scans QR codes from it) does it at a `layerEnd`, so it chooses which layers the
picture includes.

The program's frame then reads:

```
loop let step: view PaintStep in plan {
  if step.kind is PaintStepKind.draw or step.kind is PaintStepKind.customPaint {
    drawMine(step: step, ...)               # its compass, its map, its waveform, its QR code
  } else {
    paintStepRae(step: step, worlds: ...)   # scene roots, world labels, log strip, debug
  }
}
```

`renderSystemGpu2d` stays as the one-call form for a program with one world and only scene nodes.

## 5. A program's drawing in its right place

The review found three places where a program's drawing must go, and the first design had only one
of them (a whole-layer draw).

### 5.1 Inside a node: CustomPaint

A node with `CustomPaint` is painted by the program, where the node is:
- in its subtree's order, under its ancestors' clip, opacity and transform;
- at its own rectangle, with its own clip pushed.

The renderer stops before the node, the step comes back to the program, and the renderer goes on
with the rest of the root. Users:
- QR codes: example 147's QrCode node, game proto2's Drone button and join code (the qr package's
  `qrCodePaint` becomes this);
- the media editor's canvas and viewer, in `CanvasArea` / `ViewerArea`;
- a particle trail between cards (game proto4), as a sibling node at a `z` among the cards;
- a hosted guest's frame in a rectangle (SUMU's game host plan).

To make this possible, the renderer walks a root as a flat **paint list** instead of recursing. The list
is the root's nodes in paint order, with clip push and pop markers, cached per root and rebuilt
when the root's tree changes. A root with `CustomPaint` nodes becomes several `sceneRootPart`
steps around the `customPaint` steps. The flat list also speeds up the walk and the culling.

### 5.2 Over a whole layer: LayerDraw

A compass, a kill feed, a full-screen map, the log strip and the debug readout belong to no node.
They are `LayerDraw`s, with an `area` that is the layer's rectangle by default.

### 5.3 At a point in the 3D world: the world layer sorts by depth

The `world` layer holds Rae's `WorldLabel`s and a program's own per-anchor drawing (health bars,
names, a struck-through name). The two must interleave far to near; today all bars are drawn,
then all names, so a far player's name covers a near player's bar. A program adds an anchor with
`worldAnchorDraw(stack, x, y, z, appKind, id)` each frame. The plan sorts Rae's labels and the
program's anchors together by camera distance, far first, into a buffer that is reused.

## 6. Order inside a layer, raised nodes, the stage

- **Roots** of a layer are ordered by `z`, then by world key, then by mount order. A dialog
  presented later is over one presented earlier because `presentPage` gives it a rising `z`. It no
  longer depends on where its entity sits in the alive list, and a shade mounted first is under
  the HUD because of its layer, not its mount order.
- **Inside a subtree**, children paint in tree order. A child may carry `ZIndex { z }` to paint
  over or under its siblings: siblings are sorted by `z`, keeping tree order for ties. Hit tests walk
  the same order backwards. `z` never takes a node out of its parent's clip.
- **Changing a z is cheap.** Setting `z` on a node or a root re-sorts only its siblings or its
  layer's bucket: an insertion sort of a list that is already nearly sorted, with no allocation.
  It does not rebuild the plan. Game proto4's cards change `z` on every grab, hover and flight.
  Only adding or removing a layer, a world or a root changes the generation.
- **A raised node** (a node with a `LayerTag` and a parent) is laid out and placed by its parent,
  as now, and painted and hit as a root of its layer, at its `z`:
  - Layout: unchanged. Its rectangle comes from its parent's layout.
  - Transform: its parent's whole world transform applies, a `ScaleToFit` stage's scale
    included, so a card raised out of the scaled table keeps the table's scale and position.
  - Visibility: an inactive or hidden ancestor hides it (`raisedNodeShown`).
  - Opacity: its ancestors' opacity does not multiply into it; its layer's does.
  - Clip: its parent's clip does not cut it. The layer's clip does.
- **A node declared in a scene as a dialog** (game proto4's dialogs are scene nodes switched on
  with `Active`, not presented pages) is raised by its `LayerTag { id: "dialog" }` and is modal
  while it is shown, exactly as a presented one.
- **Moving a node to another parent without a jump**: `reparentKeepingPlace(node, newParent)`
  keeps its place on screen. It sets the node's offset from the old and the new parent's
  transforms, and keeps its layer and `z`. This is for a card going from hand to slot to reveal row.
- **A stack inside a node (`LayerScope`)**: 121 shows an edited document inside its own view.
  The document's own `LayerTag`s (its dock, its dialogs) must stay inside that view, under the
  editor's panels. A node with `LayerScope` opens a nested stack: names in its subtree resolve to
  a stack of its own, and the nested stack is painted, clipped and hit as one step in the node's
  place.

## 7. Input in the same order

One hit test, `uiHitAt(stack, worlds, x, y, kind)`, walks the plan from the top step down and
returns a `UiHit`:

```
type UiHit {
  world: Int             # the world it hit in
  entityId: EntityId     # the scene node hit, or noEntity()
  surface: Int           # a LayerDraw or CustomPaint with `hit`: its appKind; -1 when none
  surfaceEntityId: EntityId
  layer: Int
  blockedByModal: Bool   # nothing was hit, but a modal layer or draw above took the press
}
```

- **Scene roots** are hit as now: the hit index, in reverse paint order inside the root.
- **Surfaces**: a `LayerDraw` or a `CustomPaint` node with `hit` answers "something of mine is
  here" for its whole area at its place in the order. The program then runs its own fine hit test
  inside it. The media editor's canvas has hundreds of targets that change with the zoom; it does
  not register one entity per target. A program button drawn alone is a small surface.
- **A modal layer, or a modal draw, that shows anything** stops the walk. "Shows anything" means
  a visible root, a shown raised node or an active modal draw. A mounted root that is inactive
  does not count: 106 keeps full-screen pages mounted and hidden on the dialog layer. A press
  inside its content goes to that content. A press outside goes to nothing (`blockedByModal`).
- **A press outside** what a `dismissOnOutsideTap` layer shows sends `LayerTappedOutside { layer
  }`. `dialogSystem` answers it with the dialog's `dismissActionId`, which replaces the scrim's
  own click node. A context menu, a popover or the radial menu closes the same way. For a modal
  layer the press stops there. For a non-modal popover it then goes on to what is under it.
- **One claim for everything else.** A press is **captured** by its hit target until it is let go:
  a node, a surface or nothing-under-a-modal. Then:
  - `uiClaimsPointer(input)` and `uiClaimsTouch(input, touchId)` answer whether the UI took this
    pointer or finger. The camera rig's touch turn, the walker's click-to-move, the touch stick,
    weapon touches and a program's gestures ask this, instead of each knowing about dialogs. The
    stick is a scene node in `hud`, so a dialog over it takes the touch first.
  - `uiClaimsKeys(input)` is true while a text field has focus or a modal layer shows. Shortcut
    and movement keys check it (the media editor's Space, X and Delete; game proto2's M, R and
    WASD).
  - **A capture survives what appears later**, unless that is modal. A toast that slides in under
    a dragged card does not take the drag. A modal that opens cancels every capture under it
    (`PointerCanceled` to the holder), so a drag ends cleanly.
  - Hover, the wheel and a drag go to the same target in the same order. A wheel over an open
    dialog scrolls the dialog, and a wheel over the media editor's canvas goes to the canvas
    surface.
- **Across worlds**: the walk is over the merged plan, so SUMU's notifications take a press before
  the screen world sees it, and `suppressUiPress` is no longer needed. The UI input system runs
  once per frame over every world, from the plan.
- **Drop targets**: `uiHitAtSkipping(..., skipEntityId)` hits what is under a dragged node,
  leaving out the node and its subtree.
- **Raw readers**: code that reads `Gpu2d.pointerPressed` directly sees every press, as now. The
  layers cannot block a read they never see. Such code moves to a surface (`LayerDraw` with `hit`)
  and `uiHitAt`. Game proto2's map is the known one.

## 8. Transparency and effects

- **Alpha blending in order** comes from the order: premultiplied alpha, as the canvas does now.
- **A layer's opacity** multiplies into every draw while the layer is pushed: a canvas-wide factor
  that the box, image and text paths read. Overlapping translucent items in a faded layer show
  through each other, as in CSS without isolation. A layer that must fade as one picture sets
  `isolated`. It is drawn to a pooled offscreen target the size of its content and composited
  once, so only a layer that needs this pays for it.
- **Dim and blur behind a dialog**: the scrim is the dialog layer's lowest root. `BackdropBlur`
  reads what the frame has drawn so far, so a blur in `dialog` blurs every layer and every world
  under it (SUMU's glass over its background world) and nothing over it. The shared blurred copy
  is made once per layer and reused while nothing under it changed.
- **Shadows** stay a node's own paint, so they sort with the node.
- **Clipping**: each layer pushes its clip at `layerBegin`. Node clips, `CustomPaint` clips and a
  `LayerDraw`'s `area` nest inside it.
- **`hidesBelow`**: while a cover shows, the layers under it are not painted (a screensaver costs
  only itself). Their systems still run.

## 9. Animation

- `UiLayer.opacity`, `offsetX` and `offsetY` are plain fields, so animation tracks
  (docs/ui-animation-design.md) drive them like a node's. One track on a layer fades a whole HUD.
- Dialogs, toasts and notifications keep their own node motion (`Appear`, `Disappear`,
  `dialogMotion`). A toast sliding in is a node in `toast` with an offset track.
- A transform never changes the order. `z` and the layer do. A motion may set `z` or the layer at
  a key, at its start or its end: a card is raised when its flight starts and lowered when it
  lands. The order between keys stays fixed, so nothing flickers.
- A page transition paints two screen worlds at once, with the outgoing world's key under the
  incoming one's in every layer they share. Each world's dialogs stay over its own pages, and a
  shared element that stays put is in the incoming world.
- A layer has no scale. Scaling every draw about a point is a node transform's job (the root's
  `Transform`), and a layer has no single middle.

## 10. Layout stays apart from paint

Layout reads the tree, the window, the safe area and the host's reserved insets, never a layer's
order or `z`. Moving a node to another layer changes where it is painted and hit, not where it is
laid out. The one thing a layer adds is `safeArea`: its clip and its full-screen roots keep inside the
safe area less the reserved insets (SUMU keeps a top bar band for itself with `setReservedInsets`).
A `LayerDraw` gets the same rectangle, so a compass placed by a program stays out of the notch.

## 11. Performance

- **No extra GPU work for layers.** The canvas batches across layer and world boundaries. A layer
  adds a flush only when its first draw is of a lower kind than the last draw under it (text,
  then a box). That is the same cost a node boundary has now.
- **No per-frame allocation.** The plan, the contributions, the paint lists and the depth buffer
  are reused. A structural change rebuilds them, and a `z` change re-sorts only its bucket.
  Today `collectPaintableRoots` scans every live entity and sorts the roots by selection sort
  (O(roots^2)) for the paint and again for each hit test.
- **Culling and dirty tracking stay.** `RenderCull` and the paint cache work per root; the flat
  paint list makes the walk cheaper, not dearer.
- **Measured on a phone** (step 8, §14): frame time and draw calls of 106 and of a walker with its
  HUD on the iPhone, before and after. The bar is "no worse than today".

## 12. Developer experience

- **A `.raescene` says a node's layer and z** with the key it has today, which gains `z`:
  `"LayerTag": { "id": "hudOver", "z": 2 }`, on a root or on a node inside a scene (raised).
  `"ZIndex": { "z": 1 }` orders a node among its siblings. `"CustomPaint": { "kind": 3 }` marks a
  node the program paints. `"LayerScope": {}` opens a nested stack. A scene may declare its own
  layers: `"layers": { "minimap": { "order": 250 } }`, made when it is mounted if the stack has none
  of that name.
- **One concept.** `ui/HudOverlay` and `LayerRoot`/`LayerRef` are removed. An overlay is a
  `LayerDraw` in a layer, and the scene UI is no longer drawn as one piece.
- **The scene editor (example 121)** shows and edits a node's `LayerTag`, `ZIndex` and
  `CustomPaint`, and has a Layers list: each layer, its order, its flags and what is in it, from
  every world. The edited document is in a `LayerScope`.
- **A debug view**: `layerReport(stack, contributions)` lists every layer in order, with its
  worlds' roots, its draws, its surfaces, and whether it is modal and showing. The debug overlay
  gets a "Layers" page, and the devtools page can show the same text.

## 13. What moves to it

- `lib/ui`:
  - `Layer.rae` becomes `UiLayers.rae`, and `HudOverlay.rae` goes.
  - `RenderSystem` paints from the plan through flat paint lists.
  - `InputSystem` and `HitIndexQuery` hit through the plan.
  - `dialogSystem` uses `z` and `LayerTappedOutside`.
  - `touchStickSystem` claims through `uiClaimsTouch`.
- `lib/app3d`:
  - `HudOverlays` walks the plan: world labels in `world`, the log strip in `log`, the debug
    readout in `debug`.
  - `ModeMenu`'s menu is raised into `dialog`; `SettingsDialog` is a dialog already.
  - `CameraRig`, `WalkerInput` and the weapon touches ask the claims.
  - `Minimap` draws as a `LayerDraw` in `hudOver`.
  - `EditorPanel`'s world joins the game's stack.
- The qr package: `qrCodePaint` becomes a `CustomPaint` kind.
- Examples:
  - 106: its layer names; its resident full-screen pages stay on `dialog`, modal while shown.
  - 121: the document in a `LayerScope`; its guides and chrome become its own layers.
  - 147: the QR code node.
  - The walkers 136, 137, 143 and 148.
  - 91 and 94: their boards become `LayerDraw`s under their scene UI.
- Programs in their own repositories, each a task in its own queue:
  - game proto2: the compass and feed as `hud` draws, the map as a modal `hudOver` draw with a
    surface, the QR codes as `CustomPaint`, health bars and names as world anchors, its input
    gates replaced by the claims.
  - SUMU: its five worlds join one stack; notifications go in `notification`, the screensaver in
    `cover`, the debug widget in `debug`; `suppressUiPress` goes.
  - The media editor: the canvas and viewer as `CustomPaint` surfaces; its tooltip and future
    menus in `popover`; its shortcuts ask `uiClaimsKeys`.
  - game proto4: cards with `z` and raised nodes on its table stage, dialogs by `LayerTag`.

## 14. The order of work (queued in QUEUE.md)

1. **The stack and the plan.** `LayerStack` with the standard stack, worlds joining it, each
   world's contributions with `LayerTag`'s `z`, the merge, cheap re-sorts, and the old API kept as
   wrappers for now. Case: the plan's order.
2. **Paint by the plan.** Flat paint lists per root, `CustomPaint` steps, `LayerDraw` steps with
   their area, world anchors sorted by depth, the layer's push and pop on the canvas. `HudOverlay`
   is removed. A check frame reads back pixels of program drawing between two scene layers and a
   `CustomPaint` under a dialog.
3. **Input by the plan.** `uiHitAt` over every world, surfaces, modal layers and draws,
   `LayerTappedOutside`, captures and their cancel, `uiClaimsPointer`, `uiClaimsTouch` and
   `uiClaimsKeys`, and drop targets. Case.
4. **Order inside a layer.** `ZIndex`, the raised-node rules (the stage's scale included), scene
   dialogs by `LayerTag`, `reparentKeepingPlace`, `LayerScope`, and the `.raescene` keys. Case.
5. **Layer effects and motion.** Opacity and offset as animated fields, `z` set at a motion's
   keys, the blur per layer, `isolated`, `hidesBelow`, a toast sliding in under a dialog, and two
   worlds in a page transition. Case and a check frame.
6. **Move Rae's own programs.** app3d, the qr package, the walkers, 106, 121's document scope,
   147, 91 and 94; then remove the old wrappers.
7. **The editor and the debug view.** 121's inspector and Layers list, `layerReport`, and the
   debug overlay's Layers page.
8. **Measure on a phone.** 106 and a walker with its HUD on the iPhone, before and after.

**Built (step 1, 2026-11-18):** `lib/ui/UiLayers.rae` and case 1533. What the code decided:

- A layer has a stable `id` given by the stack, because a layer added in a gap shifts every list
  position after it. `OnLayer`, `LayerDraw.layer` and a plan step name the id; `layerIdOf(stack, name)`
  finds it (-1 for none). `addUiLayer` takes a whole `UiLayer` (for its flags), `addLayer` a name and
  an order.
- `LayerDraw`s are kept in the order they were added, so a draw's index stays its own, with a
  separate list of indexes sorted by `z`. `setLayerDrawActive` hides one without a rebuild.
- `LayerContribution` is the world's roots and raised nodes, found as `collectPaintableRoots` finds
  them, sorted by (layer, `z`, the order the world lists them). `refreshLayerContribution` makes it
  again only when the stack's generation changed or the roots are not the ones it holds (an O(roots)
  check in reused lists). A root that moves to another layer while it stays mounted is not noticed
  by that check: `markLayerContributionStale` makes it again. `setRootZ` writes the `z` on the
  node's `LayerTag` (one with an empty id when it had none) and moves its entry in place.
- A world's layer names still come from the old API: a `LayerRef`'s `LayerRoot.id`, else the
  `LayerTag`'s id, else `hud`; "content" is `hud` and "back-button" is `dock`. A name the stack does
  not have goes in `hud` and is counted (`unknownLayers`); 121's own layers are moved in step 6.
- `buildLayerPaintPlan` refills the plan's list (no allocation once it has grown): layers in order,
  in each the draws and the worlds' entries merged by `z`, draws first at an equal `z`, then the
  world key, then the world's order. A layer with nothing has no steps; a layer the mode does not
  show has none either.
- The old `createStandardLayers`, `raiseNodeToLayer`, `setLayerById` and `collectPaintableRoots` are
  unchanged and still paint and hit; the plan is not painted by yet (step 2).

**Built (step 2, the first part, 2026-11-18):** `renderSystemGpu2d` paints by the plan; case 1534.

- `Gpu2dCanvas` has a UI layer push: `canvasPushLayer(canvas, opacity, offsetX, offsetY)` and
  `canvasPopLayer`. The opacity multiplies the alpha of every queued box (all four premultiplied
  channels, in `pushPremultiplied`, so arcs, curves and gradients too), image tint and glyph and
  outline color; layers nest and multiply. The offset is a stage (`canvasPushStage`), so clips and
  the backdrop blur move with it. A frame starts with neither.
- `ui/RenderSystem`: `paintSceneRootG` paints one root as a `sceneRoot` step (a layer entity has
  nothing, a raised node of a hidden scene is hidden); `paintLayerEdgeG` pushes and pops a layer at
  `layerBegin` / `layerEnd`; `renderLayerPlanGpu2d` walks a plan for one world. A program with
  several worlds walks the plan itself with those two.
- `renderSystemGpu2d` keeps its signature (its world, systems and resources are `view`, so it
  cannot keep a stack between frames). It builds the standard stack, the world's own old layers
  (`adoptWorldLayers`, at `stackOrderOfLegacy` of their order, so 121's document, guides and
  chrome keep their order among the standard ones) and the plan on each call, which allocates
  about as much as `collectPaintableRoots` did. A program that holds its stack does not.
- `collectPaintableRoots` sorts with a stable insertion sort, so the hit test walks the roots in
  the plan's order; the old selection sort could swap two roots of one layer.
- Not yet (queued as 2b, 2c, 2d): flat paint lists and `CustomPaint`; `LayerDraw` areas, the layer
  clip and safe rectangle, the depth-sorted world layer, `app3d/HudOverlays` on the plan with
  `ui/HudOverlay` removed; the GPU check. Page transitions still paint through `gpu2dLayerRange`.

**Built (step 3, the first part, 2026-11-18):** `lib/ui/LayerHit.rae`, case 1535.

- The hit walk is a loop the program drives, because its worlds are its own and Rae has no
  function references: `beginUiHitWalk(plan, x, y, skip)`, then `uiHitWalkNext` until it returns
  none, answering each scene root it hands back with `uiHitWalkRoot(walk, step, shown, hit)`
  (`sceneRootShown` and `uiHitTestEntity` in that root's world). The walk itself handles the
  layer edges, the draws and the modal layers. `uiHitAt` and `uiHitAtSkipping` are the loop for a
  program with one world.
- A layer "shows something" when a root in it is shown (not a layer entity, not a raised node of
  a hidden scene, not inactive, not hidden) or a draw in it is active. A modal layer that shows
  something and took nothing stops the walk (`blockedByModal`). An active modal draw stops it at
  the draw's place, so a surface above it in the same layer still takes a press.
- A draw with `hit` is a surface over its area (the whole layer when it has none).
- `walk.outside` lists, top first, the `dismissOnOutsideTap` layers that showed something and
  missed the press; the walk goes on under a non-modal one. Turning it into a `LayerTappedOutside`
  event is step 3b.
- The skip leaves out a root (a dragged node raised as its own root); a node deeper in a root is
  not skipped.
- The plan includes only active draws, so a program rebuilds it after showing or hiding one, as it
  does every frame.
- Not yet (queued as 3b and 3c): captures, `PointerCanceled`, the claims, the event, hover, wheel
  and drag by the walk; Rae's own input systems, dialogs, the camera rig, walker input and the
  touch stick on it.

**Built (step 4, the first part, 2026-11-18):** `lib/ui/ZOrder.rae`, `raiseTaggedNodes` in
`lib/ui/Layer.rae`, case 1536.

- `ZIndex { z }` is a scene key and a world table. A parent with a `ZIndex` child gets a runtime
  `ZChildOrder`: its children sorted by `z`, tree order for ties, with the `z` each had. The
  transform pass ends with `zOrderSystem`, which makes an order again only when that parent's
  children or one of their `z` values changed (checked against the stored `z`s, so a direct
  write to `ZIndex` is noticed too), reusing its lists; it removes the order when no child has a
  `ZIndex` left. `zIndexes` is in the transform entry's reads, so a `z` change wakes it.
- Paint (`paintChildrenG`, the hover passes included), the hit test (`uiHitTestEntity`), the hit
  index and the wheel route read children through `orderedChildAt`, which falls back to tree
  order when the order does not match the children yet. The hit index's key includes the orders'
  table generation.
- `raiseTaggedNodes` (run by the transform pass, before the z orders) puts every node whose
  `LayerTag` names a layer on it with `raiseNodeToLayer`, making the layer at
  `legacyLayerOrderOf(name)` when the world has none; a tag changed later moves the node again.
  "hud" is the old "content". A node tagged "dialog" inside a scene is raised and, through the
  hit walk, modal while shown, as a presented page. 106's own `layerTagSystem` does the same and
  stays until step 6.
- Not yet (queued as 4b): the raised-node opacity and clip rules in code, `reparentKeepingPlace`,
  `LayerScope`, a scene's own `layers`.

**Built (step 5, the first part, 2026-11-18):** in `lib/ui/UiLayers.rae`, case 1537.

- Each contribution entry carries `shown`, set on every refresh with `sceneRootShown` (moved here
  from `ui/LayerHit`), so a page shown or hidden needs no rebuild.
- `hidesBelow`: when such a layer shows something (a shown root or an active draw) in the mode
  being painted, `buildLayerPaintPlan` drops every step under it in place
  (`dropStepsBefore`). The standard `cover` layer has it.
- Layer motions live on the stack: `animateLayer(stack, id, property, to, seconds)` eases a
  layer's `opacity`, `offsetX` or `offsetY` from where it is (a cubic ease-out, `layerMotionEase`),
  replacing a running motion of the same property; `layerMotionStep(stack, deltaTime)` moves them
  and says whether any still runs, so the program keeps drawing frames. They are not the node
  animation system's tracks: a layer is not an entity.
- A toast under an open dialog needs nothing new: the toast layer is under the dialog layer.
- Not yet (queued as 5b): the blur per layer, isolated layers, page transitions on the plan, z at
  a motion's keys through the tracks, and the GPU check frame.

**Built (step 7, the first part, 2026-11-18):** `lib/ui/LayerReport.rae`, case 1538.
`layerReport(stack, contributions)` lists every layer bottom first with its order and flags
(modal, dismissOnOutsideTap, hidesBelow, safeArea, isolated, the mode it shows in, an opacity or
offset other than none), then its draws (kind, app kind, z, area, surface, modal, hidden) and each
world's roots (world key, entity, z, shown or hidden), and marks the empty layers. A root that has
never been laid out (inactive from its mount) has no world transform, so the paint, the hit test and
the report all leave it out. The editor's inspector and Layers list and the debug overlay's page
wait for step 6 (queued as 7b).

**Built (step 2b, 2026-11-18):** case 1539.

- `CustomPaint { kind, hit }` is a scene key and a world table. Rae paints nothing of such a node
  or its subtree. With `hit` it is a hit target in the tree's hit test and the hit index, as an
  `OnClick` node is.
- Each refresh, a contribution collects its world's `CustomPaint` nodes, grouped by the root they
  are painted with (`paintRootOf`: the first raised ancestor, else the top), each root's in paint
  order (children by `ZIndex` order; the hover passes are not followed). Only roots holding one
  are walked.
- The plan paints such a root as `sceneRootPart` steps (`fromNode` .. `untilNode`) around
  `customPaint` steps (the node, its root in `fromNode`, its kind in `appKind`).
- The flat paint list of the first design was not built. `paintSubtreeG` and `paintChildrenG`
  take a `PaintRange` instead: a part walks its root's tree as before, paints only the nodes
  after `from` up to `until`, and still pushes and pops every clip and mask on the way. A subtree
  holding a marker is walked whole, never cut off screen or taken from a cached picture, so the
  markers are always met. A root without `CustomPaint` is one walk as before; a split root is
  walked once per part.
- `beginCustomPaintG(world, uiSystems, canvas, node)` pushes the node's opacity (its ancestors'
  with its own) as a canvas layer, the clips of its ancestors that clip their children up to its
  root, and its own box as a clip, and returns its box. `endCustomPaintG` pops them. Rotation and
  mask shapes of its ancestors are not applied.
- The hit walk hands a split root back once, at its top part, and passes over `customPaint` steps:
  the tree's hit test finds a `CustomPaint` with `hit` in its place.
- `renderSystemGpu2d` skips `customPaint` steps, so only a program that walks the plan draws
  them. The plan walk and the custom-node frame are in `lib/ui/RenderSystem/RenderPlan.rae`, and
  the layer motion functions moved to `lib/ui/LayerMotion.rae` to keep the files under the cap.
- Not yet (queued as 2b-2): the qr package on `CustomPaint`, its release, 147 moved.

**Built (step 2b-2, 2026-11-18):** qr 1.2.0 on Rae 0.4.13, case 1540. `qrCodeTakePending` gives a
QrCode node a `CustomPaint` of kind `qrCustomPaintKind`; `qrCodePaintStep` paints it for that plan
step inside `beginCustomPaintG` / `endCustomPaintG`; `qrCodePaint` still works. Example 147 walks
its plan with `paintPlanStepRaeG` (ui/RenderSystem: one step as Rae paints it, false for a
program's) and `qrCodePaintStep`; 148 moves to 1.2.0 and keeps `qrCodePaint` until its overlays
are on the plan (2c). The flat paint lists were not built: 106 has no `CustomPaint` node, so
none of its roots is split and it walks each root once, as before; a split root costs one walk per
part, which 147 (one code) does not notice.

**Built (step 2c, 2026-11-18):** Rae 0.4.14, case 1202. `app3d/HudOverlays` draws by the plan:
`HudLayerPaint` holds the stack (the standard one, the world layer set to game mode with
`setLayerShows`), the UI world's contribution and the plan, with three draws: the world's labels
(world), the log strip (log, editor mode) and the debug readout (debug). `drawHudLayers` adopts the
world's own layers, refreshes, builds and walks the plan (`paintPlanStepRaeG` for Rae's steps, the
three draws by kind), then the shared nodes and the canvas guides, as `renderSystemGpu2d` does. The
walkers keep it on `WalkerRender.layerPaint`. `ui/HudOverlay` and `UiWorld.hudOverlays`,
`addStandardHudOverlays`, `drawRaeOverlay` and `drawOverlays` are gone. Not yet (queued as 2c-2):
draw areas as clips, the safe rectangle, the depth-sorted world layer.

**Built (step 2c-2, 2026-11-18):** `lib/ui/LayerFrame.rae`, case 1541.

- `setLayerStackFrame(stack, width, height, safe, reserved)` keeps the window and its safe insets
  (the OS's or simulated ones plus the host's reserved band) on the stack each frame.
  `layerRectOf(stack, id)` is the safe rectangle for a `safeArea` layer, else the window.
  `paintLayerEdgeG` pushes that clip at a safeArea layer's begin and pops it at its end, once the
  frame is set.
- `beginLayerDrawG` / `endLayerDrawG` push a `LayerDraw`'s area as a clip around its drawing,
  when it has one; `drawHudLayers` uses them for Rae's draws.
- World anchors: `clearWorldAnchors`, `worldAnchorDraw(stack, x, y, z, appKind, id)` and
  `sortWorldAnchors(stack, camera)` (an insertion sort in place, far first). The world layer's
  steps start with one `worldAnchor` step per anchor, in that order. Rae's world labels are
  anchors of kind `worldLabelAnchorKind`, added each frame by `drawHudLayers` and drawn one by one
  (`drawLabelAt`), so an app's bars added after them interleave by distance; the separate
  world-labels draw is gone.
- Moved: adopting a world's own old layers (`adoptWorldLayers`, `stackOrderOfLegacy`) to
  `lib/ui/LayerLegacy.rae`, which goes in step 6b.

**Built (step 3b, 2026-11-18):** `lib/ui/LayerCapture.rae`, case 1542. `UiCaptures` is a resource the
program keeps next to its stack. `capturesFrameBegin` (after the plan is built) clears last frame's
events and finds the topmost modal layer that shows something, or the topmost layer with an active
modal draw (`planModalPosition`, from the plan and the contributions' `shown`); every capture whose
hit is below it, and was not itself blocked by it, ends with a `PointerCanceled`. A toast is not
modal, so it cancels nothing. `capturePress(captures, pointerId, walk)` keeps the walk's hit for the
pointer (-1 the mouse, else the touch id) until `captureRelease`, and turns the walk's `outside` list
into `LayerTappedOutside` events. `uiClaimsPointer` / `uiClaimsTouch` are true while that press is
held and took a node, a surface or was blocked by a modal; `uiClaimsKeys` while a modal shows or the
program set `setUiTextFocus`. Hover and the wheel are the same walk at the pointer's place
(`uiHitWalkAt`), so they need nothing of their own. Rae's systems and dialogs move onto these in 3c.

**Built (step 3c, the first part, 2026-11-18):** case 1543.

- The walkers keep a `UiCaptures` on `WalkerInput`. Each frame, before the camera,
  `hudCapturesStep` (app3d/HudOverlays) runs `capturesFrameBegin` on the last drawn plan (the one on
  screen), captures the mouse and every new finger with `hudCapturePress` through the hit walk, and
  releases the ones let go. The camera does not drag with a press the UI claims, its touch turn
  does not take a claimed finger (`blockClaimedCameraFingers`), the touch stick lets go while a
  modal shows, and WASD, R and V wait while the UI has the pointer or the keys (`walkerKeysFree`).
  The "..." menu, raised to the dialog layer, is such a modal.
- A program without a plan keeps `uiHitTest`'s root walk, and `uiHitTestTarget` now ends with
  `dialogLayerHolds` (ui/InputSystem/DialogLayerHold): while a root on the dialog layer shows
  anything, a press lands only in it or on a layer above it; anything else gets nothing. The "..."
  menu still closes on a press elsewhere, because a press that lands on nothing is elsewhere.
- A root that is leaving (a dialog animating out after its close) takes no input and holds none:
  `sceneRootTakesInput` (shown and not `Leaving`) is what the hold and the hit walk ask.
- Not yet (queued as 3c-2): dialogs closed by `LayerTappedOutside`, the scroll root and the touch
  stick asking the claims themselves, a text field's focus as `setUiTextFocus`.

**Built (step 3c-2, 2026-11-18):** case 1544.

- `ui/DialogTapOutside` `dialogTappedOutsideSystem(world, captures, stack)`: a
  `LayerTappedOutside` of the dialog layer sends the top presented dialog's `dismissActionId` as a
  UI action, as its scrim's click does. A presented dialog's scrim takes the press whenever it has
  a dismiss action (the press is then inside the dialog, never outside), so the two never both
  fire; the event closes a dialog with no clickable scrim.
- `touchStickSystemBlocking(world, uiSystems, blockedIds)` leaves the fingers the UI holds alone
  (`claimedPointerIds(captures)`; -1 the mouse). `touchStickSystem` is it with none blocked. The
  walkers pass theirs through `updateTouchControls(..., blockedIds)`.
- The scroll root leaves a finger alone under any shown root of the dialog layer
  (`dialogLayerCovers`, from ui/InputSystem/DialogLayerHold), the "..." menu and a scene's own
  dialogs included, as well as under a presented page (`presentedPageBlocks`).
- `feedUiTextFocus(captures, world)` sets the text focus from the world's `TextField.focused`;
  the walkers call it each frame in `hudCapturesStep`.

**Built (step 4b, 2026-11-18):** `lib/ui/Reparent.rae`, case 1545.

- A raised node takes no opacity from its ancestors: the transform pass gives it 1.0 as the
  inherited alpha, and its layer's opacity is applied when its layer is painted. It is still hidden
  with them (`visible` is inherited). Its parent's clip never cut it (it is painted as a root of its
  layer), and its layer's clip does (2c-2).
- A raised node keeps a ScaleToFit stage's scale and place without anything new: ScaleToFit scales
  its subtree's layout (`fitScaleSubtree`), so the raised node's rectangle and world position
  already carry it.
- `reparentKeepingPlace(world, uiSystems, node, newParent)` moves a node under another parent: when
  the new parent has no Layout, its Rect is set so its world position stays (through the new
  parent's world position and scale); in a layout container it takes the container's place. Its
  LayerTag and ZIndex stay.
- Not yet (queued as 4c): `LayerScope` and a scene's own `layers`.

**Built (step 4c, 2026-11-18):** `lib/ui/SceneLayers.rae`, case 1546.

- `LayerScope { clip }` is a scene key and a world table. It is not a second paint plan inside the
  node: a `LayerTag` below a `LayerScope` is simply not raised to the window's stack
  (`raiseTaggedNodes` skips it, and lowers it again if it was raised), and the scope node paints its
  tagged descendants after the rest of its subtree, sorted by their layer's standard order and `z`,
  clipped to its box (`paintScopedTaggedG`). The hit test and the hit index test them first, in the
  reverse order, and every child walk skips them (`isScopedTagged`). A nested scope keeps its own.
  For 121's edited document this is what matters: the document's dialog stays inside the view, over
  the document's page and under the editor's own layers. A document's modal rule (input under its
  dialog) is left to the document view.
- `sceneLayerDefs(scene)` reads a scene's top-level `"layers": { "minimap": { "order": 250 } }` from
  its document (no parser change), and `addSceneLayers(stack, scene)` adds the ones the stack does
  not have, in the stack's own orders.

**Built (step 5b, 2026-11-18):** `checks/uiLayerEffects`.

- The backdrop blur's copy is per UiLayer: `paintLayerEdgeG` starts a new backdrop layer at each
  `layerBegin` (`canvasBackdropLayer`), no longer at each root, so every BackdropBlur in a layer
  shares one copy of what the layers under it drew, every world included; the copy is still reused
  while nothing under it changed.
- An isolated layer is drawn at full strength into a frame-sized paint-cache picture
  (`canvasCacheBegin` with key `isolatedLayerKey`) and composited once at its `layerEnd` at the
  layer's opacity. When the picture cannot be had (the canvas holds one capture at a time, so a
  PaintCache node is capturing), the layer falls back to fading draw by draw.
- `pageTransitionDraw` paints the current world and the leaving ones by one plan: every world joins
  one stack, going forward the current world's key is the highest (its pages over the leaving one
  in every layer they share), going back the leaving one's is; layer edges are painted once. The
  frames are byte for byte the old ones (checks/uiPageTransitions forward and back at half way,
  against the commit before).
- Not yet (queued as 5c): `z` set at a motion's keys through the animation tracks.

**Built (step 5c, 2026-11-18):** `lib/ui/ZKeys.rae`, case 1547. The animation system's tracks are
springs over float channels, with no timed keys, so `z` is not a channel: it is keyed beside them.
`keyNodeZ(world, node, keys)` sets a node's `z` at each key's time and holds it between keys;
`zDuringFlight(world, node, raisedZ, landedZ)` raises it now and lowers it when its animation
track ends (or at once when no motion starts within 3 frames). `setNodeZ` writes the `ZIndex` and,
when there is one, the `LayerTag`'s `z`, so `ZChildOrder` and the layer contributions re-sort only at
a key. `zKeySystem` runs at the start of `animationSystem`, and pending keys keep
`animationInFlight` true.

**Built (step 6, 2026-11-18):**

- `app3d/ModeMenu` tags its menus for the dialog layer with a `LayerTag` (`tagForDialogLayer`); the
  transform pass raises them.
- `app3d/EditorPanel` (134, 135, 138) keeps a `HudLayerPaint` and draws by its plan
  (`drawPanelLayers`): its chrome's roots and layers, then the debug readout as the debug layer's
  draw.
- 106 drops its own `layerTagSystem`; the transform pass's `raiseTaggedNodes` does the same. Its
  scenes keep the old names ("content", "back-button"): the stack already reads them as `hud` and
  `dock`, and the names go with `LayerRoot` in 6b.
- 121's edited document has a `LayerScope` on its page root, so the document's own tagged nodes
  stay inside its view.
- 91 and 94 keep a layer stack each; their boards are a program draw in hud at `z` -1, under the
  scene UI's roots (94's in `Layers.rae`, to keep `Main.rae` under the cap).
- `SettingsDialog` needed nothing: it is a scene instance in the panel world, painted with it.
  `Minimap` has no program in this repository; game proto2 moves it in its own queue.
- Checked by eye on headless frames: 91, 94, 106, 121, 134 (and 136, 147 earlier).

**Steering (2026-11-18, after steps 1 to 5 in part):**

- **`LayerTag` is the one link from a node to its layer.** The runtime `OnLayer` of §4.2 is not
  built: `LayerTag { id, z }` plus the stack's lookup by name does its work. `LayerRoot`,
  `LayerRef`, `collectPaintableRoots` and adopting a world's own layers go in step 6b, once every
  program has moved (step 6).
- **Hit testing follows the plan where a program keeps one.** A program that keeps a plan (the
  walkers' `HudLayerPaint`) hits through `ui/LayerHit`'s walk. A one-world program that only calls
  `renderSystemGpu2d` keeps `uiHitTest`'s root walk, which is in the plan's order since step 2,
  until 6b replaces it with a one-world plan (3c).
- **The remaining order of work** is 2c-2, 2d, 3b, 3c, 4b, 5b, 6, 6b, 7b, 8, then the release.

**Built (6b, Rae 0.4.17):** `LayerTag` is the one link from a node to its layer. `LayerRoot`,
`LayerRef`, `createStandardLayers`, `createLayerEntity`, `findLayerEntity`, `entityLayerOrder`,
`collectPaintableRoots`, `raiseTaggedNodes` and `ui/LayerLegacy` are gone. `setLayerById` and
`raiseNodeToLayer` set the tag (keeping its z); `isRaisedNode` is a node with a parent whose tag
names a layer, not below a `LayerScope`. Names are looked up in the window's stack only ("content"
and "back-button" are no longer read as hud and dock; 106's back button says "dock"). 121's
document, guides and chrome are on the standard hud, hudOver and dock, since it paints by
`renderSystemGpu2d`'s standard stack. `uiHitTest`, the wheel route and the hit index walk
`rootsInPaintOrder` (the standard stack's plan with the one world). A contribution checks its
entries against the LayerTags when the tag table changed, so a tag that moves its node to another
layer is seen without `markLayerContributionStale`. `sceneRootShown` and `sceneRootTakesInput`
moved to `ui/Layer`.

## 15. The review (2026-11-18)

The first version was checked against SUMU, the media editor, game proto4 (its scenes and its plans:
it has no UI code yet), game proto2, and the examples here, the Featured ones (106, 121, the
walkers, 91, 94) and 147 among them. What changed because of it:

| Found | Change |
|---|---|
| SUMU paints five UI worlds and two during a transition; the 3D editor panel is a second world | the stack belongs to the window; worlds join it and merge (§4.1) |
| notifications must go over dialogs; a screensaver takes its first press and hides what is under it | `notification` and `cover` layers, `hidesBelow` (§4.3) |
| QR codes in buttons, the media editor's canvas, a trail between cards, a hosted guest | `CustomPaint`, painted in the node's place (§5.1) |
| bars and names from two sources overlap wrongly | the world layer sorts anchors by depth (§5.3) |
| a compass and a feed must sit under the HUD's buttons | at an equal `z`, draws paint before roots (§4.4) |
| hundreds of drawn targets that change with the zoom | surfaces with the program's own fine hit test (§7) |
| shortcuts and movement keys fire under a dialog | `uiClaimsKeys` (§7) |
| a drag must survive a toast and end when a modal opens; a drop must hit what is under the dragged card | captures and their cancel, `uiHitAtSkipping` (§7) |
| popovers and a radial menu close on a tap outside | `dismissOnOutsideTap`, apart from `modal` (§4.1, §7) |
| cards change `z` on every grab and flight | cheap bucket re-sorts, `z` set at a motion's keys (§6, §9) |
| cards raised out of a scaled table; cards moving between parents; dialogs that are scene nodes | the stage's transform kept, `reparentKeepingPlace`, `LayerTag` dialogs modal (§6) |
| 121's edited document has layers of its own | `LayerScope` (§6) |
| 106 keeps hidden full-screen pages mounted on the dialog layer | "shows anything" means visible (§7) |
| the scene key `LayerTag` already exists | it stays and gains `z` (§4.2, §12) |
| a top bar band kept by the host | `safeArea` includes the reserved insets (§10) |
| a frame capture for QR scanning | at a `layerEnd`, so the program picks the layers (§4.4) |

## 16. Not in this design

- 3D-in-UI and UI-in-3D (a screen in the world): the `world` layer is only for 2D drawn over the
  3D view at a projected point.
- A depth-tested 2D: §3, D.
- Several windows: each window has its own stack, and nothing is shared between them.
