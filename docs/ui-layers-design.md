# UI layers: one paint and input order for everything 2D

Status: design (2026-11-18). Implementation steps are queued in `QUEUE.md`, right after the
design task, each named "ui layers: ...". Breaking changes are allowed; the UI stays ECS.

## 1. The problem

Rae has two orders today, and they do not meet:

1. **Inside the scene UI**, `ui/Layer`: `LayerRoot { id, order }` entities (`createStandardLayers`:
   content 0, dock 100, back-button 200, dialog 300, debug 400), a `LayerRef` on a root, and
   raised nodes (`raiseNodeToLayer`, used by `app3d/ModeMenu`'s "..." menu). `collectPaintableRoots`
   sorts the roots by their layer's order every frame, and the paint and the hit test walk that.
2. **Between whole blocks**, `ui/HudOverlay`: `HudOverlay { draw, order, shows }` entities (world
   labels 10, the whole scene UI 20, log strip 80, debug readout 90, `OverlayDraw.app` for an
   app's own), drawn by `app3d/HudOverlays.drawOverlays`.

The whole scene UI, with its dialog layer, is ONE overlay. So anything an app paints on the
`Gpu2dCanvas` itself (a compass, a kill feed, health bars, a reticle, a full-screen map, a QR code
over a button) is either under every scene node or over every scene node, dialogs included.
Game proto2 had its kill feed and compass over the open "..." menu; it now draws them at overlay
order 15, under the whole scene UI. Its full-screen map, and a QR code drawn over a button, still
cannot be placed right, because they must be over some scene nodes and under others.

Input has the same split. The scene's hit test walks the sorted roots top first, so a dialog takes
the taps over the scene's own nodes. But the camera's touch turn, the touch stick and an app's
own drawn buttons do not ask the scene. Each checks its own things (`presentedPageBlocks`, the
stick's zone), and an app's immediate drawing has no hit areas at all.

## 2. What the canvas already gives

`Gpu2dCanvas` is an immediate, painter's-order stream. Every draw call queues a box, an image or a
text glyph run. `canvasKeepOrder` flushes only when an item would land under what is already
batched: one flush draws its boxes, then its images, then its text, font by font. So:

- **The order of the calls IS the paint order**, across everything that draws on the canvas in a
  frame: the scene renderer, world labels, the log strip, an app's compass.
- Two draws of the same kind in a row share a batch, whoever made them. A layer boundary costs
  nothing by itself; only the kind changing costs a flush.

The design builds on this. Nothing in it needs a depth buffer or a render target per layer.

## 3. Decision: one stack of layers, one paint plan, painted in order

Alternatives weighed:

| Approach | Verdict |
|---|---|
| **A. One stack of layer entities; per frame a paint plan in layer order; each step draws on the shared canvas in order** | **Chosen.** Uses what the canvas does now. Alpha blending is right because the order is right. Batches carry across layer boundaries. No new GPU state. |
| B. One sorted draw list `(layer, z, clip, pipeline)`, collected from every source, sorted, then drawn | Sorting by pipeline inside a layer reorders overlapping translucent items, which is wrong without depth. Every source would have to emit into a retained list instead of drawing: a second API for apps. The canvas already batches in order, so the win is small. |
| C. An offscreen target per layer, composited | One full-screen RGBA target per layer is about 30 MB at 2556x1179 for eight layers, plus a composite pass each. Too costly on a phone. Kept only as an opt-in for one layer (§7, `isolated`). |
| D. A depth buffer, z per item | Translucent UI (text edges, shadows, scrims) still needs back-to-front order. Depth would only help opaque boxes, which are not the cost. |

Rae has no function references, so the app cannot hand a callback to a layer. As with a
`Schedule` and with today's `overlaysInOrder`, **Rae owns the order and the app calls the draws**.
Each frame `layerPaintPlan` returns the steps in order. The app walks them. A Rae step
(`paintStepRae`) draws itself, and an app step comes back to the app with its kind. The 3D apps' shell
(`app3d/HudOverlays`) does this walk for the standard kinds.

## 4. The model

### 4.1 Components (lib/ui/UiLayers, replacing ui/Layer's LayerRoot and ui/HudOverlay)

```
type UiLayer {            # on a layer entity in the UI world
  name: String            # "hud", "dialog", an app's "minimap"
  order: Int              # lower paints first; standard layers are 100 apart
  modal: Bool = false     # while it shows anything, input below it is blocked (§6)
  shows: LayerShows       # always | gameMode | editorMode (was OverlayShows)
  opacity: Float = 1.0    # multiplies every draw of the layer (§7)
  offsetX: Float = 0.0    # moves every draw of the layer, in design units (§8)
  offsetY: Float = 0.0
  safeArea: Bool = false  # its roots are laid out inside the safe area (§9)
  isolated: Bool = false  # opt-in: drawn to an offscreen target, composited with opacity
}

type OnLayer {            # on a scene node: which layer it is painted and hit in
  layer: EntityId         # (replaces LayerRef)
  z: Int = 0              # order inside the layer (§5)
}

type LayerDraw {          # one non-scene thing drawn in a layer
  layer: EntityId
  kind: LayerDrawKind     # worldLabels | logStrip | debugReadout | app
  z: Int = 0              # order among the layer's draws and its scene roots
  appKind: Int = 0        # an app tells its own draws apart (its own enum, as an Int)
}

type HitArea {            # an app's immediate drawing that takes input (§6)
  layer: EntityId
  x: Float  y: Float  w: Float  h: Float   # design units, screen space
  radius: Float = 0.0     # a rounded or round area
  z: Int = 0
  id: Int                 # the app's own meaning
  active: Bool = true
}
```

`UiLayer` entities live in the `UiWorld` with the scene nodes, as `LayerRoot` does now. A layer is
looked up by name once (`layerNamed(world, "dialog")`) and then held as an `EntityId`.

### 4.2 The standard stack

`createStandardLayers(world)` makes these. An app adds its own between them with an order in a gap
(`addLayer(world, "minimap", order: 250)`), and never needs to know the others' numbers beyond
the one it sits next to (`layerOrderAbove(world, "hud")`).

| order | name | holds | modal |
|---|---|---|---|
| 100 | `world` | world-anchored text and bars (app3d/WorldLabel), drawn over the 3D view | |
| 200 | `hud` | the scene's default layer: page content, a game's HUD nodes, the touch stick | |
| 300 | `hudOver` | over the HUD: a full-screen map, a QR code over a button | |
| 400 | `dock` | the dock, toolbars, the back button (was `dock` and `back-button`) | |
| 500 | `popover` | non-modal popovers, tooltips, dropdowns | |
| 600 | `toast` | toasts and notifications (under a modal dialog) | |
| 700 | `dialog` | dialogs, sheets, context menus, the "..." menu | yes |
| 800 | `log` | the log strip (editor mode) | |
| 900 | `debug` | the debug readout, the debug button and menu | |

A scene node with no `OnLayer` is in `hud`, as a root with no `LayerRef` is in `content` now. 106's
"content" becomes "hud": the name stays generic ("the main layer"), not a game term.

### 4.3 The paint plan

`layerPaintPlan(world, uiSystems, editor)` returns `List(PaintStep)`:

```
type PaintStep {
  layer: EntityId
  kind: PaintStepKind     # sceneRoots | worldLabels | logStrip | debugReadout | app | layerBegin | layerEnd
  entityId: EntityId      # the scene root (sceneRoots) or the LayerDraw entity
  appKind: Int
}
```

Within a layer the steps are sorted by `z`, scene roots and `LayerDraw`s together, with ties kept
in a fixed order: mount order for roots, then creation order. `layerBegin`/`layerEnd` frame each
layer that has anything, and set the layer's opacity, offset and clip on the canvas
(`canvasPushLayer` / `canvasPopLayer`, §7). The plan is cached on the world and rebuilt only when
something changes its shape: a layer, an `OnLayer`, a `LayerDraw`, a mount or an unmount (a
`layersGeneration` counter, as the theme has). So a still frame does not allocate (§10).

The app's frame then reads:

```
loop let step: view PaintStep in layerPaintPlan(world: world, uiSystems: uiSystems, editor: editor) {
  if step.kind is PaintStepKind.app {
    match step.appKind { case compassDraw { drawCompass(...) } case killFeedDraw { ... } }
  } else {
    paintStepRae(step: step, ...)        # scene roots, world labels, log strip, debug readout
  }
}
```

`renderSystemGpu2d` stays as the one-call form for an app with only scene nodes: it walks the plan
and draws every Rae step.

## 5. Order inside a layer, and raised nodes

- **Roots** of a layer are ordered by `OnLayer.z`, then by mount order. A dialog presented later
  is over one presented earlier because `presentPage` gives each a rising `z`
  (`PresentedPage.order` already counts this). It does not depend on where its entity happens to
  sit in the alive list.
- **Inside a subtree**, children paint in tree order, as now. A child may carry `ZIndex { z }`
  to paint over or under its siblings: siblings are sorted by `z`, keeping tree order for ties.
  Hit tests walk the same order backwards. `z` never takes a node out of its parent's clip.
- **A raised node** (a node with an `OnLayer` and a parent) is laid out and placed by its parent,
  as now, and painted and hit as a root of its layer, at its `z`. The rules, now written down:
  - Layout: unchanged. Its rectangle comes from its parent's layout.
  - Transform: its parent's world transform applies, so it moves with its parent.
  - Visibility: an inactive or hidden ancestor hides it (`raisedNodeShown`).
  - Opacity: its ancestors' opacity does NOT multiply into it. It takes its layer's opacity
    instead, so a dimmed page does not dim the menu raised out of it.
  - Clip: its parent's clip does not cut it. The layer's own clip does (the window, or the safe
    area when `safeArea` is on).
- `raiseNodeToLayer(world, entityId, layerName, z)` stays the one call. The `.raescene` spelling is
  in §11.

## 6. Input in the same order

One hit test, `uiHitAt(world, uiSystems, x, y, kind)`, walks the paint plan from the top step
down. It returns a `UiHit`:

```
type UiHit {
  entityId: EntityId      # the scene node hit, or noEntity()
  hitAreaId: Int          # the HitArea's id when an app's area was hit, else -1
  layer: EntityId         # the layer it was in
  blockedByModal: Bool    # nothing was hit, but a modal layer above took the press
}
```

- **Scene roots** are hit as now (the hit index, reversed paint order inside the root).
- **HitAreas** are the hit targets of an app's own drawing: the app keeps one entity per button or
  map it draws, and moves it when the drawing moves. They sort with the scene roots by `z`, so a
  map in `hudOver` hit over a scene button in `hud` is the map's.
- **A modal layer that shows anything** stops the walk. A press inside its content goes to that
  content. A press outside goes to nothing (`blockedByModal: true`), and the layer gets a
  `LayerTappedOutside { layer }` event. `dialogSystem` answers it with the dialog's
  `dismissActionId`, which replaces the scrim's own hit node, and an open context menu closes the
  same way.
- **Everything else asks the same question.** `uiClaimsPointer(input)` and
  `uiClaimsTouch(input, touchId)` are true for a press that began on a UI hit or under a modal
  layer, until it is let go. The camera rig's touch turn, the walker's click-to-move, the touch
  stick and an app's own gestures check this, instead of each knowing about dialogs. The stick
  and its zone become one more claimant: the stick's node is a scene node in `hud`, so a dialog
  over it takes the touch first.
- Hover, scroll and drag targets use the same walk, so a wheel over an open dialog scrolls the
  dialog, never the page under it.

## 7. Transparency and effects

- **Alpha blending in order** comes from the order: premultiplied alpha, as the canvas does now.
- **A layer's opacity** multiplies into every draw while the layer is pushed. The canvas already
  multiplies a chain alpha into each node's colors (`chain255`), and `canvasPushLayer` sets a
  canvas-wide factor that the box, image and text paths read. Overlapping translucent items in a
  faded layer then show through each other, as in CSS without isolation. A layer that must fade
  as one picture sets `isolated`: it draws to a pooled offscreen target the size of its content
  and is composited once. This costs a target, so only a layer that needs it pays.
- **Dim and blur behind a dialog**: the scrim is the dialog layer's first root at the lowest `z`,
  as now. `BackdropBlur` reads "what the frame has drawn so far", so a blur in the `dialog` layer
  blurs every layer under it, and nothing over it. The backdrop's one-blur-per-layer sharing keys
  on the `UiLayer` entity instead of the old root order.
- **Shadows** stay a node's own paint (the box pipeline's soft edge), so they sort with the node.
- **Clipping**: each layer pushes its clip at `layerBegin`, the window or the safe area. Node clips
  nest inside it as now.

## 8. Animation

- `UiLayer.opacity`, `offsetX`, `offsetY` are plain fields, so the animation system's tracks
  (docs/ui-animation-design.md) can drive them like a node's. A whole HUD fades out for a cut
  scene with one track on the layer.
- Dialogs and toasts keep their own node motion (`Appear`/`Disappear`, `dialogMotion`). A toast
  sliding in is a node in the `toast` layer with an offset track. Order never changes during a
  motion, because the layer and `z` decide it, not the transform.
- A scale on a layer is not offered: scaling every draw about a point is a node transform's job
  (the root's `Transform`), and a layer has no single middle.

## 9. Layout stays apart from paint

Layout reads the tree, the window and the safe area, never a layer's order or `z`. Moving a node
to another layer changes where it is painted and hit, not where it is laid out. The one thing a layer
adds is `safeArea` for its roots that fill the screen: their rectangle is the safe area instead of
the window (a toast column, a dock), as `applySafeAreas` does for named nodes now.

## 10. Performance

- **No extra GPU work for layers.** The canvas batches across layer boundaries. A layer adds a flush
  only when its first draw is of a lower kind than the last draw of the layer under it (text, then
  a box). That is the same cost a node boundary has now.
- **No per-frame allocation.** The plan, the per-layer root buckets and the sorted `z` lists are
  rebuilt on `layersGeneration` changes, not every frame. Today `collectPaintableRoots` scans
  every live entity and sorts the roots by selection sort (O(roots^2)) twice a frame, once for
  paint and once for the hit test. The buckets remove both.
- **Culling and dirty tracking stay.** `RenderCull` and the paint cache work per root, and a root
  is still a root, only found through its layer.
- **Measured on a phone** (step 8 in §13): frame time and draw calls of 106 and of a walker with
  the HUD, before and after, on the iPhone. The bar is "no worse than today".

## 11. Developer experience

- **A `.raescene` says a node's layer and z**: `"OnLayer": { "layer": "hudOver", "z": 2 }`, on a
  root or on a node inside a scene (raised). `"ZIndex": { "z": 1 }` orders a node among its
  siblings. A scene may declare its own layers at the top: `"layers": { "minimap": { "order":
  250 } }`, made when the scene is mounted if the world has none of that name.
- **One concept.** `ui/HudOverlay` and the `LayerRoot`/`LayerRef` half of `ui/Layer` are removed.
  An overlay is a `LayerDraw` in a layer. "The whole scene UI" is no longer a thing that is drawn
  as one piece.
- **The scene editor (example 121)** shows a node's layer and z in the inspector, edits them, and
  has a Layers list: each layer, its order, and what is in it.
- **A debug view**: `layerReport(world)` lists every layer in order with its roots, its
  `LayerDraw`s, its hit areas and whether it is modal and showing. The debug overlay gets a
  "Layers" page, and the devtools page can show the same text.

## 12. What moves to it

- `lib/ui`: `Layer.rae` becomes `UiLayers.rae` (§4). `HudOverlay.rae` goes. `RenderSystem` paints
  per step. `InputSystem` and `HitIndexQuery` hit through the plan. `dialogSystem` uses `z` and
  `LayerTappedOutside`. `touchStickSystem` claims through `uiClaimsTouch`.
- `lib/app3d`: `HudOverlays` walks the plan (world labels in `world`, the log strip in `log`,
  the debug readout in `debug`). `ModeMenu` raises its menu into `dialog`. `SettingsDialog` is a
  dialog already. `CameraRig` and `WalkerInput` ask `uiClaimsPointer`/`uiClaimsTouch`. `Minimap`
  draws as a `LayerDraw` in `hudOver`.
- Examples: 106 (its layer names), 121 (the editor: §11), and the walkers 136, 137, 143, 148.
- game proto2 moves its compass and kill feed to `hud` `LayerDraw`s, its map to `hudOver`, and its
  QR code to a `LayerDraw` with a `HitArea` (that is its own repo's task).

## 13. The order of work (queued in QUEUE.md)

1. **The stack and the plan** (lib/ui/UiLayers): `UiLayer`, `OnLayer`, `LayerDraw`, the
   standard stack, `layerPaintPlan` with cached buckets and `z`. The old `createStandardLayers`,
   `raiseNodeToLayer`, `setLayerById` and `collectPaintableRoots` are kept as wrappers over it for
   now. Case: the plan's order (layers, z, mount order, a raised node, `shows` by mode, an app's
   layer between two standard ones).
2. **Paint by the plan**: `RenderSystem` draws one root at a time from the plan, with the layer's
   push and pop on the canvas (opacity, offset, clip). `app3d/HudOverlays` walks the plan, and
   `HudOverlay` is removed. A check frame (checks/) shows an app's canvas drawing between two scene
   layers, read back by pixels.
3. **Input by the plan**: `uiHitAt`, `UiHit`, `HitArea`, modal layers with
   `LayerTappedOutside`, `uiClaimsPointer`/`uiClaimsTouch`. The camera rig, walker input, touch
   stick and dialog system use them. Case: under an open modal, presses, taps and drags on
   scene nodes, hit areas, the stick and the camera reach nothing, and a tap outside closes it.
4. **Order inside a layer**: `ZIndex` among siblings, the raised-node rules of §5 (no ancestor
   opacity, the layer's clip), and the `.raescene` keys `OnLayer`, `ZIndex` and `layers`. Case.
5. **Layer effects and motion**: layer opacity and offset as animatable fields, the backdrop blur
   per layer, `isolated` layers, a toast in `toast` sliding in under a dialog. Case and a check
   frame.
6. **Move the 3D apps and the examples**: app3d's ModeMenu, SettingsDialog, Minimap, the touch
   stick and the walkers (136, 137, 143, 148), and 106. Then remove the old wrappers and
   `LayerRoot`/`LayerRef`.
7. **The editor and the debug view**: 121's inspector and Layers list, `layerReport`, and the
   debug overlay's Layers page.
8. **Measure on a phone**: 106 and a walker with the HUD on the iPhone, before and after.

## 14. Not in this design

- 3D-in-UI and UI-in-3D (a screen in the world): the `world` layer is only for 2D drawn over the
  3D view at a projected point.
- A depth-tested 2D: §3, D.
- Several windows: one UI world draws in one window, as now.
