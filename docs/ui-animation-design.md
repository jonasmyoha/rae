# UI animation in lib/ui: design

Written 2026-11-06. How every Rae UI gets appear and disappear animations,
staggered lists, interruptible page transitions and animated dialogs, declared
in `.raescene` files as components and bundles.

The goal is that Rae UI is the best system to animate a UI in. In practice:

- **A scene author writes no code.** One bundle on a node gives it good motion.
- **Everything can be interrupted.** A tap during a transition works at once, and
  the motion turns around smoothly from where it is, at the speed it has.
- **An idle UI costs nothing.** No animation in flight means no system runs and
  nothing draws. This keeps the event-driven loop (docs/event-driven-ui-loop-plan.md).
- **Motion never reruns layout**, except for the one layout change that started it.
- **It can be tested.** Headless runs can hold every animation at a chosen point
  and take a screenshot of it.

The first user is SUMU, an iPhone app on lib/ui (section 13). The style is inspired
by iOS: springs with no bounce for movement, a little bounce for things that pop,
short fades, and a list that arrives from the side the thumb is on.

## 1. What exists today

| Piece | Where | What it does | Limit |
| --- | --- | --- | --- |
| `HoverScale` | animationSystem | Eases a scale at rest, hovered, pressed | Exponential ease only; one value |
| `AnimFrames`, `WobbleFx`, `BackgroundPan`, `SmokeFx`, `Carousel` | EffectSystems | Looping effects | Each writes `RuntimeOffset`, `Opacity` or `TransformFx` itself |
| `HeroTransition` | HeroTransitionSystem | Moves a sprite between two pages | Sprites only; pages in one world only; cannot be interrupted |
| `listMotionSystem` | listMotionSystem | Rows glide to a new order | Moves only; no row enter or leave; own rate |
| `ButtonHoverFade`, `SwitchSystem` | their systems | Fade a glow; move a knob | Each has its own ease |
| `lib/Easing` | stdlib | Easing curves | Not connected to any component |
| `springStepVelocity` | ScrollPhysics | One spring step | Only for scrolling |

Five gaps stop the goal above:

1. **Scale does not work on a subtree.** The transform pass still assumes identity
   scale (the comment over `transformSystem`). A node's own box grows about its
   pivot in `RenderPaintBox`, but its children fold from the parent's top-left, and
   hit testing uses the top-left with no pivot. A card that zooms would come apart.
2. **Systems write the same fields.** `listMotionSystem`, the reorder system and the
   effect systems all write `RuntimeOffset`. `widgetStyle` writes `Opacity`. A
   new animation that writes them too would fight these.
3. **Nothing can leave slowly.** `Active: false` and `closePresentedPage` end a node
   at once. Layout closes the gap in the same frame.
4. **There is no appear.** A node has no way to say how it arrives.
5. **A page transition needs both pages.** `HeroTransition` needs two pages in one
   world. An app that makes a new `UiWorld` per screen, as SUMU does, destroys the
   old page before the new one is drawn.

**Done in 0.3.29,** so that one spring and one ease remain in `lib/ui`. The spring is `lib/Spring`, and the curves are `lib/Easing`,
both reached through a channel or `springStep`. The ease is `easeTowards`, an exponential for what has a rate and no end. No scene changes.

| Piece | Now | Why not a track |
| --- | --- | --- |
| `HoverScale` | Takes an optional `motion`, a spring that keeps its speed through a change of target. With none, `speed` is the ease it always was | Its value is read by the painter and multiplies a sprite: it is not a pose. `pressable` uses `quick` |
| `SwitchSystem` | `SwitchMotion` runs on the spring, 0.18 s as before. The knob's paint curve is gone, as the spring is that curve | A knob is not a node: the painter draws it from the position |
| `Carousel` | Its offset runs on the spring, `transitionDuration` its response, and starts at the finger's speed | A finger writes the offset directly, and it moves the pages by a runtime offset |
| `ButtonHoverFade` | As it was | A constant rate has no curve or speed: turning back is already continuous, and takes only as long as the way back |
| Reorder | Uses `easeTowards` | The rows follow a finger that keeps moving, so there is no end to retarget to |
| Scroll's rubber band | Runs on the spring, and `springStepVelocity` is gone | A finger writes the position directly. Scrolling runs on seconds since 0.3.30, so a fling and a spring-back take the same time at 60 and 120 Hz |

## 2. The model in one page

```
Theme            "motion" tokens: named springs and curves
Bundles          appearRise, appearPop, disappearFade, staggerFromEnd, ...
Components       Appear, Disappear, Stagger, AnimateLayout, AnimateText,
                 PageTransition, SharedElement (authored in scenes)
Runtime tables   AnimationTrack (what is moving), AnimationPose (the result)
Systems          animationSystem (the only writer of AnimationPose)
                 pageTransitionSystem, dialogSystem, listMotionSystem (they ask
                 animationSystem for motion, they do not write the pose)
Transform pass   folds AnimationPose into WorldTransform2D and WorldVisual
```

**The decision that matters most: one new channel, `AnimationPose`.** A pose is a
component with `opacity`, `scale`, `x`, `y` and `rotation`, relative to the node's
laid-out place. Only `animationSystem` writes it. The transform pass multiplies it
in after `Opacity`, `TransformFx` and `RuntimeOffset`, so a row can scroll, be
dragged and fade in at once with no system knowing about the others. An entity at
rest has no pose, so a page at rest pays nothing.

**One system or several?** One `animationSystem` does all the motion: it owns the
tracks, steps every spring and curve, and writes the poses. The systems that know
*when* something moves stay separate, as they need different inputs: pages, dialogs
and lists each send requests through its inbox. This follows
docs/ecs-systems-own-their-tables.md: one owner per table, and systems talk through
data.

## 3. Motion tokens

A new section in the theme, parsed like `palette` and `space`, imported the same way
and redeclared by name:

```json
"motion": {
  "quick":  { "kind": "spring", "response": 0.24, "damping": 1.0 },
  "smooth": { "kind": "spring", "response": 0.42, "damping": 1.0 },
  "lively": { "kind": "spring", "response": 0.46, "damping": 0.76 },
  "fadeIn":  { "kind": "curve", "curve": "easeOutCubic", "seconds": 0.20 },
  "fadeOut": { "kind": "curve", "curve": "easeInCubic",  "seconds": 0.14 }
  # also built in: "livelySoft", a response of 0.46 at damping 0.9, for a page's zoom, and
  # "keyboard", easeOutQuart for 0.25 s, what a layout moves on with the on-screen keyboard
}
```

- **A spring** has `response` (seconds to get about there) and `damping` (1.0 has no
  bounce, lower bounces). These are the two numbers iOS uses, and they stay readable.
  The step is analytic, not Euler, so it does not depend on the frame rate. It keeps
  its velocity, which is what lets a motion turn around smoothly.
- **A curve** has a name from `lib/Easing` and a length in seconds. A curve is for
  opacity, where a spring's overshoot means nothing.
- lib/ui has these five as built-in tokens, so a scene with no theme still works.
- An unknown name is a scene diagnostic (`make scene-check` fails), never a silent
  default. The pure spring math goes in `lib/Spring.rae`, unit-tested on its own.

## 4. The components

All are authored, serialized and shown in the UI editor. Field names are camelCase;
the fields not written take the defaults shown.

**`Appear`**: how a node arrives. The numbers say where it starts from; it ends at
its normal place.

```json
"Appear": { "motion": "lively", "opacityMotion": "fadeIn",
            "opacity": 0.0, "scale": 1.0, "x": 0, "y": 16, "rotation": 0,
            "delaySeconds": 0.0 }
```

It plays when the node is mounted and when `Active` goes from false to true. The
start pose applies before the first paint, so a node never flashes at its end place.

**`Disappear`**: how a node leaves. The numbers say where it ends.

```json
"Disappear": { "motion": "quick", "opacityMotion": "fadeOut",
               "opacity": 0.0, "scale": 0.96, "x": 0, "y": 0, "rotation": 0 }
```

**`Stagger`**: on a parent or a list. Its children's `Appear` and `Disappear` start
one after another.

```json
"Stagger": { "stepSeconds": 0.028, "maxSeconds": 0.22,
             "appearFrom": "end", "disappearFrom": "start" }
```

`appearFrom` and `disappearFrom` are `start`, `end`, `center` or `none`. `maxSeconds`
caps the total, so a long list never makes a person wait: the step shrinks to fit.
It counts visible children only, and a row outside the window starts at once.

**`AnimateLayout`**: when layout moves this node, it glides from its old place to
the new one. The rest of this document calls this a layout glide.

```json
"AnimateLayout": { "motion": "smooth" }
```

Layout runs once and gets the true places. The system compares each node with its
last place, sets the pose to the difference, and lets it spring to zero. Later
frames run no layout. Only position glides; size changes at once in this design
(section 15).

**`AnimateText`**: when its text changes, the old text goes as the new one comes.

```json
"AnimateText": { "kind": "fade", "motion": "quick" }
```

`kind` is `fade` or `roll` (the new text comes from below, for a clock or a counter;
`rollFrom: "above"` for one that goes down, `rollFrom: "auto"` for one that goes both ways:
from below when the new text sorts after the old one, numbers read as numbers).

**`PageTransition`**: on a page's root. It says how this page arrives and how it goes
(section 8).

```json
"PageTransition": { "kind": "push", "motion": "smooth" }
```

**`SharedElement`**: a node that is on both pages with one `key` (section 8.3).

```json
"SharedElement": { "key": "bottomBar", "mode": "stay" }
```

`HoverScale` stays as the way to react to a press. It gets an optional `motion` token
in place of `speed`, so a press can use a spring.

**`FocusStyle`** (built in 0.3.39): a field's look while it has focus. On a node with a
`TextField` and a `Shape`, the edge takes `stroke` (a palette slot or `#hex`) at `strokeWidth`
(below 0: the Shape's own) while the field has focus, and fades there and back on `motion`. The
fade is a channel in `focusFades` only while it runs, so a resting field costs nothing: the painter
then reads `TextField.focused`. A field with no edge of its own fades one in from transparent.
Case 1384, `checks/uiFocusField`.

```json
"FocusStyle": { "stroke": "accent", "strokeWidth": 2.5, "motion": "quick" }
```

### Bundles

Built into lib/ui (`ui/BuiltInBundles`), so a node gets them by name, and any theme may
redeclare them. Bundles already merge field by field and the node wins, so a scene can
change one number: `"bundles": ["appearRise"], "Appear": { "y": 40 }`.

| Bundle | What it holds | For |
| --- | --- | --- |
| `appearFade` | `Appear` opacity only | Text, status |
| `appearRise` | `Appear` opacity, y 16, `smooth` | Rows, sections |
| `appearPop` | `Appear` opacity, scale 0.86, `lively` | Icons, tiles, chips |
| `appearDrop` | `Appear` opacity, y -24, `lively` | Things from the top |
| `disappearFade` | `Disappear` opacity only | Most things |
| `disappearShrink` | `Disappear` opacity, scale 0.94 | Cards, tiles |
| `staggerFromEnd` | `Stagger` from the end | Lists that fill from the bottom |
| `staggerFromStart` | `Stagger` from the start | Lists read from the top |
| `pressable` | `HoverScale` pressed 0.96, `quick` | Every button |
| `dialogMotion` | `Appear` scale 0.9 `lively`, `Disappear` scale 0.96 | Dialog cards |
| `glideOnChange` | `AnimateLayout` `smooth` | Anything that reflows |

## 5. animationSystem

Folder `lib/ui/animationSystem/`. New files: `AnimationSystem.rae` already exists and
keeps `HoverScale`; the new work is `AnimationTracks.rae` (the type, create, update),
`AnimationRules.rae` (pure, unit-tested: pose math, stagger delays) and
`AnimationTriggers.rae` (finding what started).

```
type AnimationTrack {          # one per moving entity, in the system's own table
  opacity: AnimationChannel    # current, velocity, target, motion, elapsed
  scale, x, y, rotation: AnimationChannel
  delayLeft: Float
  phase: AnimationPhase        # appearing, moving, leaving
  onEnd: AnimationEnd          # stay, deactivate, destroy, destroySubtree
}
```

**Each channel is separate.** Opacity runs a curve while position runs a spring, and
they finish at different times. A track ends when all of its channels are at rest, and
then the track and the pose are removed.

**The inbox.** Others ask with `animationPlay(...)`, `animationTo(...)` and
`animationLeave(...)`, which put commands in `Queue(AnimationCommand)`. The update
drains it. A command on an entity that already has a track retargets it from the
current value and velocity. That one rule gives interruption everywhere.

**A fling.** `animationFling(uiSystems, entityId, from, to, velocity, motion, opacityMotion, onEnd)`
is for a host's own gesture code, as a card or a sheet a finger lets go: the track starts at
`from`, where the finger left the node, even over a running motion, with each channel at the
`AnimationVelocity` given (pose units a second: points for x and y), toward `to`. A spring
carries the speed on, so the node does not stop and start again, and a fast one passes `to` and
comes back. It brakes as it pulls home: from 1200 points a second `smooth`'s first 1/120 s runs at
about 1000. A curve (an opacity fade) runs its own way from `from`. `onEnd` other than `stay` makes it
a leave, as a card swiped away that ends switched off. Built in 0.3.38; case 1383.

**Starting and ending from data.** The system compares what it knows:

- A node with `Appear` and no record of being seen has just been mounted.
- `setActiveAnimated(world, entity, active)` and `destroyAnimated(world, entity)`
  replace the direct calls wherever a node may have `Disappear`. With no `Disappear`
  they act at once. With one, the node stays and gets the marker `Leaving`. `Leaving`
  takes no hits and no focus. When its track ends, the system does the real
  deactivate or destroy, and layout then closes the gap (`AnimateLayout` glides it).
- `applyOverride` and `activeEdit` for `Active` go through `setActiveAnimated`, so an
  app that only sets `Active` gets the motion from the scene alone. That is what
  lets a hosted WASM app animate with no new contract.

**Schedule.** A new `animation` entry in `lib/ui/Pipeline.rae` between `fit` and
`transform`: layout must be done, so `AnimateLayout` sees the new places, and the pose
must be written before the transform pass reads it. The `transform` entry adds
`animationPoses` to its reads, so its dirty check still works.

**In flight.** `animationInFlight(world, uiSystems)` is true while a track exists or a request waits, and is fed
to `animating` in the host's wait (lib/ui/EventLoop.rae says a source that forgets this
flag starves). When it is false, no animation code runs.

**Time.** Each world steps from the real frame time, clamped to 1/20 s, so a long frame
slows a motion and never skips it. It never reads the wall clock, so `RAE_NOW` does not
affect it.

## 6. The transform pass must tell the truth

The first task, as nothing can zoom without it.

- A node's `TransformFx.scaleX/Y` and pose scale act about `TransformFx.pivot`, with
  0.5, 0.5 as the default for a pose. `buildLocalTransforms` moves the origin for scale
  the way it already does for rotation.
- Children fold through it, so a scaled card carries its text, icons and shadows.
- **Everything that reads a rectangle reads the same one.** Shape, sprite, text, caret,
  clip rectangle, soft shadow, gradient, hit test and `visualBounds` take one helper,
  `worldRectOf`. `RenderPaintBox`'s own pivot move goes, or it would apply twice.
- The hover multiplier stays a separate paint-only value.

**Opacity of a group.** The alpha chain multiplies down the tree per draw. So a faded
card shows its background through its text, and overlapping children show each other.
At the speed and the sizes used here this looks like a normal fade, so offscreen group
rendering is not in this design (section 15).

Done in 0.3.17 for `TransformFx`; the pose comes with the core. `worldRectOf` is in
lib/ui/Transform.rae. A shadow's blur and a text shadow's offset scale with the box, and a
hover grows a blur and a stroke by its small factor. `visualBoundsAt` scales its cached
measure, so a zooming card does not measure its text every frame.

**A stroke zooms too** (0.3.18), its width, dash and gap, unless its `Shape` says
`strokeFixed: true`, which keeps them that wide on screen at any zoom, like SVG's
non-scaling-stroke. The UI editor's frame and safe-area guides say so: they sit inside its
camera and would nearly vanish at a low zoom. Its outlines and marquee sit outside the
camera, in screen units, and do not need it. Case 1364 holds the rectangles and the hit test, `checks/uiZoom` paints it, and
at scale 1 example 106 is the same to the pixel. Pictures: docs/images/ui-zoom.

## 7. Lists

`listMotionSystem` stays the owner of what a row does, and asks `animationSystem` for
the motion. Rows must be known by key (`itemKeyField`). A list with no key gets the
first fill only, and a log line says so once.

| Event | What happens |
| --- | --- |
| First fill of a list | The rows in the window play the row root's `Appear`, in `Stagger` order |
| A row comes into the window by scrolling | **Nothing.** It is simply there. A list must never animate because of a scroll |
| A new key | That row plays `Appear`; the rows around it glide |
| A key is gone | A *ghost* row plays `Disappear`; the rows around it glide into the gap |
| A key moves | The row glides (as today, now through the track) |
| A row is held by drag | Its motion stops, as today; the reorder system places it |
| A list is rebuilt with the same data | Nothing |

**A ghost row.** The pool would give the row's entity to other data at once. So the
row is taken out of the pool for the length of its `Disappear`, keeps its last bindings
and its place, and the pool gets a new entity. Then it is destroyed. A ghost takes no
hits. In `Flow` mode the keyed row entity simply stays, with `Leaving`.

**Sideways change.** Going to another day replaces the data and every key. The
list then needs a direction, not a stagger of new keys: the edit `listRowsEdit` gets an
optional `change: "forward" | "back"`, and all old rows leave to one side as new rows
arrive from the other, slightly staggered. Without the field, the table above holds.

**The old row-jump check.** A host may check that rows do not jump between frames.
Such a check reads laid-out places or world places; section 11 says which to read.

**Fixed in 0.3.35:** an app that scrolls a new row into view (`requestScrollToRow`) in the frame the row comes moved the
scroll in one step, so on a list longer than its window every row jumped by that scroll (case 1380). `scrollToRowSystem`
now hands the step to the list's motion state, and the rows that keep their key glide from where they were drawn, the
scroll included. A scroll with no data change still moves nothing. A row that falls far behind because rows are added
faster than a glide ends (a test adding a task every few frames) moves fast, but it is never drawn out of place.

**Built in 0.3.22:** all of the above, in `lib/ui/listMotionSystem/` and
`lib/ui/listViewSystem/ListViewFlow.rae`. What the code decided:

- `listViewSystem` calls list motion itself, because a ghost must be taken before its row is rebound, and only that
  system sees that moment. So there is no host call any more: `listMotionSystem`, `listMotionNeedsRun` and
  `createListMotionSystem` are gone, and a host only runs `animationRun`. A host that called them removes the calls
  and uses `animationInFlight`.
- A list takes part when it has `animateMoves` or its row root has `Appear` or `Disappear`; `animateMoves` is what makes
  rows glide. Any other list does what it did. A change with `listRowsChange` works on every list.
- A virtual list chooses which pool row sits in each slot, so a row keeps its entity across a data change and across a
  scroll, and its track stays with it. Slot s is still child s: a ghost is at the tail of the children, outside the pool.
- "In the window" for a stagger is by index and scroll (`listRowVisible`), with the overscan rows out. Those start at once.
- A list filled while hidden plays through the triggers when it opens, as any node does. Their test for the window is
  the parent's box, so such a list opened scrolled has no stagger.
- In the Rae API a sideways change is `listRowsChange(world, listEntityId, direction)`, said together with the new data, and
  used once. It does not need keys. `listRowsEdit`'s `change` field is SUMU's SDK calling it.
- In a sideways change a flow row gives up its layout place at once (`Leaving.holdsPlace` false), or the old and new rows
  would stack. It stays where it was last laid out.

## 8. Page transitions

### 8.1 Two worlds, or one

A host has one of two page models, and both are supported by one system,
`lib/ui/pageTransitionSystem/`:

- **Resident pages in one world**, such as example 106: both roots are alive; nothing
  more is needed.
- **A new world per screen**, such as SUMU. `pageTransitionBegin` *takes ownership* of
  the old `UiWorld` and `UiSystems`, and keeps them for the length of the transition.
  The host builds the new world as today, and calls `pageTransitionDraw`, which paints
  the old one under or over the new one. The old world gets no input and no app. Its
  only running system is `animationSystem`, and it is dropped at the end.

Drawing both live is chosen over a snapshot to a texture: it needs no render target,
text stays sharp under zoom, and a transition can turn around with both pages intact.
Fonts and textures are shared resources, so no second load happens.

If a second navigation comes during a transition, the page still leaving is dropped at
once, and the current page becomes the one leaving, from its current pose. A Back
during a push is the same push run backward, so a person who tapped by mistake sees the
page return along the path it came.

### 8.2 The kinds

| Kind | Arriving page | Leaving page | Over it |
| --- | --- | --- | --- |
| `push` | From the right edge, `smooth` | Moves left by 30% and fades out | Back runs it in reverse |
| `sibling` | From 40 points to one side and fades in | Moves 40 points to the other and fades out | Direction from the order of the pages |
| `zoom` | Grows from a source rectangle to full, `lively` at low bounce; content fades in | Grows a little and fades | Back shrinks it into the same rectangle |
| `sheet` | From the bottom edge, `smooth` | Scales to 0.95 and dims | Back goes down |
| `fade` | Fades in | Fades out | What a page with no `PageTransition` gets, unless its host sets another default |
| `none` | At once | At once | For a screen that must not wait, such as a failure |

The leaving page also plays its own nodes' `Disappear`, in `Stagger` order, in the
first 40% of the transition. The arriving page plays `Appear`, starting at 25%. So a
list leaves row by row and the new one arrives row by row, with the page motion under
both. Their timing comes from the kind, so a scene needs nothing more.

**See-through pages.** On an iOS push, opaque pages hide each other. A UI whose pages
are transparent over a shared background (SUMU's are) would show two sets of text
over each other. So the leaving page's opacity always reaches zero by 60% of the
transition, in every kind, and the background and any bar in another world stay still.

**Built in 0.3.24:** sections 8.1 and 8.2, in `lib/ui/pageTransitionSystem/`. What the code decided:

- A new world per screen takes two calls, `pageTransitionBegin` before the host builds the new world and
  `pageTransitionArrive` after, as the kind is the new page's `PageTransition`, or going back the old page's. A host
  that never calls the second has its old world dropped on the next `pageTransitionStep`. The host also needs a laid-out
  new page before `pageTransitionArrive`, for the page's size.
- "The transition" has a length to hang the shares on: a spring's response (98.6% of the way when critically damped), or
  a curve's seconds. The leaving opacity is a curve that takes 60% of that, whatever the motion.
- `Disappear` and `Appear` have their own motions, so the shares say when each *starts*: within the first 40% (a `Stagger`'s
  `maxSeconds` is capped there), and from 25%. A `Disappear` may run past 40%, and the page's fade hides it by 60%.
- `zoom` runs on a new built-in motion, `livelySoft` (`lively` with damping 0.9): the "low bounce" of the table.
  With no rectangle said, it grows from the middle of the page, at half size. `pageTransitionSetSource` is remembered
  for Back.
- Going forward, the new page is drawn over the old. Going back, the old page is drawn over the new, going away.
- `RAE_ANIMATION_HOLD` holds the old world too, so it is not dropped, and a picture has both pages. The share is linear,
  so a held leaving page at 0.7 has opacity 0.3, even though in time it would be zero by then.
- Nothing is loaded: `res` is a `view` of the host's fonts. A trace of file opens through `checks/uiPageTransitions` shows each
  font and the theme opened once, in a run with two worlds.

### 8.3 What stays

`SharedElement { key, mode }` on a node of each page:

- `mode: "stay"`: if the node is on both pages and at the same place, it does not take
  part. The new one shows from the first frame, the old one is hidden, and the pose of
  the page does not move it. It is drawn in place as the page moves under it. A bottom
  bar or a header stays still, and its selected slot changes by its own motion.
- `mode: "morph"`: the node moves and resizes from its old rectangle to its new one
  with a spring. It generalizes `HeroWidget` from a sprite to any node, across worlds.
  `HeroTransitionSystem` becomes this, and `HeroWidget` stays as an alias so that
  existing scenes load.

**Built in 0.3.26:** in `lib/ui/animationSystem/SharedElements.rae`, for both page models. What the code decided:

- A morph is the new node with a pose, not a copy flying above it. So it can be any node, with whatever is under it, and it
  takes taps where it is drawn. The pose gets a second axis for this, `stretchY`, as two boxes need not have one shape.
- A node doing this is a `SharedNode`. The transform pass takes its page's pose back out of its local place, and its alpha skips
  the page's opacity. The painter leaves it for a last pass, over its page and outside its clips, which is what the overlay layer
  used to give the hero. Going back, that pass runs after the old page, as the old page is drawn over the new one. Taking the
  page's pose out assumes the page's pose does not rotate and that the nodes between have no scale of their own.
- `stay` needs the same place and size on both, to a point. A node with the key at another place simply goes with its page.
- A source off screen makes no morph. The old node is hidden by a pose with opacity 0, so nothing of layout changes and the end of
  the page clears it, and it plays no `Disappear`.
- A morph carries the corner radius by writing the node's `CornerRadius` divided by the pose's scale, so it reads as the old
  radius and then its own, and the end writes its own back. A morph's motion is `SharedElement.motion`, `smooth` by default.
- `HeroWidget` is a morph, and its table stays. `spawnHeroTransition` starts one with no page transition (example 106). It is
  a spring now, in place of 0.3 s of ease-in-out, and the overlay entity, `HeroTransition` and `heroTransitionSystem` are gone. In 106
  the resting picture is identical byte for byte to the one before.
- A shared node is done when it and its page have stopped.

**Built in 0.3.37:** a marker that moves inside one world, in `sharedMovesInWorld` (`SharedElements.rae`, case 1382, the week row
in `checks/uiLayoutText`). What the code decided:

- A `SharedElement` morph node that hides, and one with its key that shows in the same frame, on the same page (the nearest
  `PageRoot` above each, or none for both), make the one that shows come from where the other was last drawn, by the same pose
  morph a page uses, with no layout. A chosen row's fill bound to the list's data, or a chosen cell's in a grid row, moves so.
- A node seen for the first time plays nothing, nor does a key that only shows or only hides, nor one whose old node was off
  screen. Pairs are by key, first found first; a page's own arrival keeps its nodes, as `sharedArrive` has them first.
- A page's morph is drawn last, over its page. This marker is drawn under the nearest node that holds its old and new place (the
  list or the row of cells), right after that node's own box and before any of its children, so it passes under every row's text.
  `SharedNode.paintUnder` says which; none is the old last pass.
- It is looked for only when `Active`, the hierarchy or the shared elements changed, with the other triggers.

### 8.4 A swipe from the edge

`pageTransitionDrag(progress, velocity)` runs a transition from a finger, not from
time: a right swipe from the left edge drags the page back. On release, the page
continues, or springs back, from the finger's velocity. Since a track retargets from
the current value and velocity, this needs only a paused state, and no new motion
math. It is one thumb's natural Back. The bar's Back stays.

**Built in 0.3.27:** `lib/ui/pageTransitionSystem/EdgeSwipe.rae` finds the gesture, and `pageTransitionDrag` drives the
transition. What the code decided:

- The finding and the driving are apart, as only the host knows what Back means. `edgeSwipeStep` runs after the input step and
  before the scroll root, and says `started`, `moved` or `released`. On `started` the host calls `pageTransitionDragStart`
  and goes back with the ordinary calls, so the same page models work, and mid-flight is the ordinary second navigation.
- The press is the swipe's once it has moved 10 points, a scroll's distance, along x and more than along y, inside
  `uiPressClaimed` as a scroll's is. Moving along y first leaves it to the scroll. The edge is 24 points wide.
- `progress` is 0 to 1 from where the pages were when the finger took them. It places them with `channelHold` on a paused
  track, the leaving page's opacity clear by 60% of the way as always.
- A release finishes if `progress + velocity * 0.15 s` is at least 0.5. Each channel takes the finger's speed along its own path.
  Opacity does not fling: it runs on from where it is.
- Springing back makes the old page the page again at once: no `Leaving`, and its nodes return to rest. Resident, the arriving
  page deactivates at the end. With two worlds, the host calls `pageTransitionTakeBack` each frame after `pageTransitionStep`, and
  on the frame it says true the old `UiWorld` and `UiSystems` are its own again, and it rebuilds what is tied to them, such as its schedule.
- A morph needs time and a finger has none, so a swipe does none: such a node goes with its page. `stay` works as usual.
- Only the page's root follows the finger: its nodes' `Disappear` and `Appear` run on the clock.
- With `RAE_ANIMATION=off` or kind `none` there is no motion for a finger to hold, so the transition is at its end at once.

## 9. Dialogs and other layers

`presentMountedPage` plays motion in the same way as a page, on the "dialog" layer:

- The scrim is a node with `appearFade` and `disappearFade`.
- The card plays its root's `Appear` and `Disappear`. A dialog with neither gets
  `dialogMotion`, so every existing dialog animates with no scene change.
- `closePresentedPage` becomes a two-step close. The host stays with `Leaving` and takes
  no input; the page is destroyed when its tracks end. `presentedPageCount` does not
  count a closing page, so the app sees it as closed at once, and a new one can open.
  The scrim of the closing page goes as the new one comes, with no flash.
- A dialog over a dialog: the one under it scales to 0.97 and comes back.
- `presentation: "page"` follows `PageTransition`, with `sheet` as the default.

**Built in 0.3.23:** the dialog side of this, in `lib/ui/dialogSystem/DialogSystem.rae`. What the code decided:

- A close removes the page from `presentedPages` at once, so `presentedPageCount`, `topPresentedPage` and the blocking of
  the screen all see it as gone, and its node ids go at once too. A new page can use the same page id. The host is the one
  thing that stays, with `Leaving`, and `animationChildMoving` now looks at the whole subtree, as the card is inside a slot.
- Only a card that has been seen has anywhere to leave from. One closed before `animationTriggers` ever looked at it is
  destroyed at once, and so is a page in a host that runs no animation, which is what those hosts did before.
- A second close of a closing host does nothing.
- A dialog steps back only under another dialog: a full page hides it. It comes back when the top one closes, and closing
  the one under leaves the top one alone. Two dialogs presented in one frame, before the first has a track, do not step back.
- A full page follows its root's `PageTransition`, a `sheet` by default (0.3.25). Only the page moves: the screen under it
  stays still, so the "leaving page" half of a kind is not used. It closes by going back out the way it came. Its opacity is
  zero by 60% as always, so a `sheet` going down fades as it goes. `none` opens and closes at once.
- A full page's arrival only waits in the inbox when it is presented. A host that runs no animation never takes it, so it
  shows the page in place, and a close that finds the arrival still waiting destroys the page at once.
- With no rectangle to come from, a presented `zoom` grows from the middle of the page.
- Placement is layout's and the pose scales about the card's middle, so the middle is where placement put it at every scale.
- A closing scrim and a new one together never dim below 98% of one scrim (case 1371), which is the "no flash".

## 10. What a person feels

- **Input never waits.** Taps go to the arriving page from its first frame. Hit testing
  uses the world rectangle, so a moving button is hit where it is drawn. A `Leaving`
  node takes nothing.
- **Reduced motion.** `Gpu2d` gets `reduceMotionEnabled()` (iOS
  `UIAccessibility.isReduceMotionEnabled`, macOS likewise). With it on, position, scale
  and rotation channels are skipped, and everything is a short fade. The app has no
  code for this.
- **Frame rate.** A spring at 120 Hz looks much better than at 60 on a ProMotion
  phone. `RAE_IOS_HIGH_REFRESH=1` (tools/ios/gen-ios.sh) already permits it. An app
  uses it for frames with motion in flight. Idle stays at zero.
- **The budget.** A 30-row page transition must hold the frame rate on an iPhone 15 Pro,
  with a measured line in the task's report. `transform` walks every node while anything
  moves, and that is fine at the sizes known. If the measure says otherwise, the fold
  becomes per-subtree, and no API changes.

**Built in 0.3.28:** reduced motion and the budget's check. What the code decided:

- `reduceMotionEnabled()` is in `lib/ReduceMotion.rae`, as `Gpu2d.rae` is at its line cap and every UI program then needs no renderer
  to ask. It is read whenever a batch of motion starts, so a change in the system's settings counts from the next motion, and an idle UI asks
  nothing. `RAE_REDUCE_MOTION=on|off` fixes it, for a test.
- One rule in `animationApplyCommand` covers every kind of motion: a node leaving stays where it is, any other is at its place at once. What
  would have arrived by moving fades in, what would have left by moving fades out, on `fadeIn` and `fadeOut`, with no delay. A dialog
  stepping back is at 0.97 at once with no fade, a layout glide does not happen, and a text roll is a fade.
- 120 Hz needs nothing new: a host busy-renders while `animationInFlight`, and waits when not, so `RAE_IOS_HIGH_REFRESH=1` is all that
  lets those frames go past 60, and idle stays at zero frames.
- The budget is `checks/uiPageBudget`: a page of 30 rows, 123 nodes, a new world each way. A still or a moving transform pass takes
  about 2.7 µs on an M1 Max, so it is nowhere near over budget there, and the fold stays as it is for now. The measure on an
  iPhone 15 Pro that this section asks for is below.

**Measured on an iPhone 15 Pro (2026-11-07, 0.3.33, `checks/uiPageBudget` with `RAE_IOS_HIGH_REFRESH=1`):** 16 transitions, 754
frames with motion in flight, 123 nodes.

| what | p50 | p95 | max |
|---|---|---|---|
| frame to frame | 8.33 ms | 8.86 ms | 28.7 ms |
| systems (pipeline and the old world's step) | 0.09 ms | 0.11 ms | 0.65 ms |
| paint recorded, both pages | 2.55 ms | 3.49 ms | 6.79 ms |
| transform pass alone | 4.5 µs | 5.0 µs | 9.7 µs |
| building the new world (once per transition) | 7.46 ms | 8.93 ms | 8.93 ms |

- Frames with motion hold 120 Hz: the median is one refresh, and 95% are within 0.53 ms of it. The check's "over 8.33 ms" count
  (368) is the refresh's own jitter around 8.33 ms, not missed frames.
- The transform pass is 5 µs, so the fold stays whole-tree.
- Building the arriving page's world takes about 7.5 ms. The page is already built on the frame it starts on, and that frame
  paints the start pose with no time passed, so the motion's first step is on the next frame, as a tap's frame should be.

**Measured again (2026-11-07), with the check counting only the transitions:** the 21 frames over 16.67 ms above were not the
transitions. They were the launch: the first page plays its own `Appear` while iOS is still finishing the app's launch, at 60 Hz,
and the check counted those frames as motion in flight. The check now waits 2 s before the first transition, and gives the launch
and the frame a page is built on lines of their own. On the iPhone 15 Pro, 16 transitions:

| what | p50 | p95 | max | over 16.67 ms |
|---|---|---|---|---|
| frame to frame, transitions | 8.33 ms | 8.66 ms | 10.44 ms | 0 of 700 |
| the frame a page is built on | 10.71 ms | 11.32 ms | 11.32 ms | 0 of 16 |
| the launch's own Appear | | | | 21 of 61 |

So a transition holds 120 Hz from its first moving frame, and nothing in lib/ui changed. The frame a page is built on comes one
refresh late, which delays the start by about 8 ms after a tap and cuts no motion; spreading the build over frames would only
delay it more.

**The zoom out of an icon in SUMU (iPhone 15 Pro, 2026-11-07, Rae 0.3.40, `RAE_IOS_HIGH_REFRESH=1`).** SUMU measured
its launcher-to-app zoom at a median 15 ms a frame. Timed inside SUMU's frame (temporary timers, the `motion` self-test,
the 40 frames after the arriving world is built):

- The CPU work is small: input and update 0-1 ms, and in the render the background 0.2-0.6 ms, both pages 1.0-2.5 ms,
  the overlays 0.4-1.3 ms and the frame end 0.8-2.4 ms. List rows binding and text measuring do not show.
- The rest of the frame is `beginFrame` waiting for a drawable: 2-5 ms on one frame and 11-15 ms on the next, in turn,
  so every other frame takes a whole extra refresh.
- Frames over 9 ms of 39, with one part of the drawing left out each run: everything drawn 18-20; no background 19;
  no leaving page 9; no arriving page 16; no overlays 17; neither page 7. So the leaving page costs most, and even
  with neither page drawn some frames still wait a whole refresh.
- A CADisplayLink asking for 120 Hz (`preferredFrameRateRange` 80-120, preferred 120) changed nothing (16 of 39).
- Before the zoom, while the scripted finger held the icon, the frames came every 16.7 ms (60 Hz), and at 8.3 ms
  after it lifted.

**The GPU's time, measured (0.3.41, `RAE_GPU2D_TIMING=1`, two runs of the same `motion` self-test):** every timed frame
of the zoom out of the icon reads 0 (45 frames timed of the 80), and of the zoom back all but one (1.35 ms). On this
phone a 2D pass under about a millisecond reads 0: in `checks/gpu2dTiming` 4 full-screen boxes read 0 where a Mac reads
0.25 ms, and 40 read 1.7 ms. So the GPU draws the zoom in under a millisecond a frame, the CPU's work is 2-5 ms,
and the frame still takes 17 ms every other frame (median 9-11 ms, 90% 17 ms). The time is lost waiting for the
drawable, in how the frames are paced, not in drawing: that is where "[ui animation: page transition frame time 2]"
looks next.

**The pacing, looked at (2026-11-07, the same phone and test, `RAE_FRAME_TIMING=1` split per frame):**

- Each frame is CPU, then a wait to acquire the drawable; the present and the GPU wait are under 1 ms. In the zoom
  out the frames go 8, 17, 8, 17 ms (80 a second on average), each 17 ms frame mostly the acquire (11-13 ms).
- The zoom out's CPU is a median 4 ms and a 90% 6 ms a frame; the zoom back's 3 and 4 ms, and the zoom back holds
  8 ms (median 8, 90% 9). The first frames of the zoom out, at 3-4 ms, hold 8 ms too, and the alternation starts as
  the CPU reaches 5-6 ms.
- No change from: drawing offscreen and acquiring the drawable late (`RAE_DIRECT_PRESENT=0`); one frame in flight
  (`RAE_FRAMES_IN_FLIGHT=1`, worse); a CADisplayLink asking for exactly 120 Hz (min = max = preferred = 120), made on
  the first frame. Even with it, the frames while the scripted finger holds the icon come every 17 ms.
- The scripted finger is not a touch, and iOS raises the refresh rate for a real touch. So the test may be harsher
  than a person's tap.

What is left: less CPU in a 2D frame ("[ui 2D canvas: less CPU a frame]"), and a person's tap on the phone
("[ui animation: the zoom with a real finger]").

**Less CPU a frame, tried (0.3.42).** The 2D canvas no longer flushes after every node: it flushes only where an item
would otherwise draw under one already batched (a box after an image or text, an image after text, text in another
font; `canvasKeepOrder` in lib/Gpu2dCanvasFrame.rae). In 106's home screen the flushes that draw went from 47 to 14 a
frame, with the reference pictures of 104, 105, 106 and 121 unchanged; its CPU a frame on a Mac fell by about 2%. On
the phone it changed nothing measurable: the zoom out still had 18 of 40 frames over 9 ms. And one run's zoom back had
22 of 40 over 9 ms at a CPU of a median 2 ms and a 90% 3 ms a frame, so the CPU a frame is not what sets the pacing
either. What is left is how the frames are presented: "[ui animation: present pacing on ProMotion]".

**How the frames are presented (measured 2026-11-07, iPhone 15 Pro, SUMU's motion self-test, 40 frames of the zoom
out).** The CAMetalLayer that wgpu-native makes has `maximumDrawableCount` 3, `presentsWithTransaction` off and
`allowsNextDrawableTimeout` off. The base run had 18-21 of 40 frames over 9 ms, with the drawable acquire at a median
6 ms and a 90% 14 ms. Each change below was made in temporary code and then removed:

| change | zoom out frames over 9 ms | frame time |
|---|---|---|
| none (the base) | 18-21 of 40 | 8 and 17 ms, alternating |
| `desiredMaximumFrameLatency` 3 (`WGPUSurfaceConfigurationExtras`) | the same as the base | the same |
| `desiredMaximumFrameLatency` 1 | 39 of 40 | median 25 ms |
| `maximumDrawableCount` 2 | 39 of 40 | median 25 ms |
| each frame waits for the tick of a CADisplayLink at 120/120/120 Hz | 17 of 40 | median 9 ms, 90% 17 ms; acquire median 1 ms |

- The defaults are already the best of these. Fewer drawables or a lower frame latency make every frame wait for the
  display.
- A CADisplayLink only ticks on the run loop of the thread that renders, in the default mode. When it was added to
  the main run loop in the common modes it never ticked, and each wait ran out. That may also be why the earlier
  120 Hz link changed nothing. With the link on the right run loop it ticked 119 times a second. Waiting for its tick
  moved the wait out of the drawable acquire (acquire median 1 ms), but the frames still came 8 and 17 ms apart. So
  the frame does not start late: a frame that was ready in time is still shown one display refresh later.
- wgpu-native's `wgpuSurfacePresent` takes only the surface. It cannot present at a time or after a minimum
  duration (Metal's `presentDrawable:afterMinimumDuration:`). Doing that needs the drawable presented outside
  wgpu-native.
- What is left to try is in "[ui animation: present the drawable at a time on iOS]".

**When each drawable reaches the display (measured 2026-11-07).** wgpu-native's Metal backend presents by calling
`presentDrawable:` on a small command buffer of its own, and its API never hands out the `CAMetalDrawable`. So the
runtime C cannot present the drawable itself with one call. For the measurement, temporary code replaced
`presentDrawable:` on the Metal command buffer class (`AGXG16PFamilyCommandBuffer`) at run time. It added
`addPresentedHandler:` to each drawable (its `presentedTime`), the scheduled and completed handlers to the command
buffer, and optionally called `presentDrawable:afterMinimumDuration:` instead. Results from SUMU's motion self-test:

- **The display goes to 80 Hz in the zoom out.** The first ~20 frames of the zoom out are shown 8.3 ms apart. Then
  every frame until the end of the zoom is shown 17, 8, 17, 8 ms apart: 80 frames a second, a fixed ProMotion rate.
  A zoom back is mostly shown 8.3 ms apart, and in some runs it alternates too. Only the first 13 frames after launch
  are dropped (never shown).
- **A frame reaches the screen about 21 ms after it is handed to Metal.** The present's command buffer is scheduled at
  once and completes 4-8 ms later (after the frame's render work ahead of it on the queue). The frame is shown about
  15 ms after that, two refreshes at 120 Hz, which is iOS's compositor. When a zoom back holds 120 Hz, a frame is shown
  13-14 ms after it is handed over.
- **That latency is what makes the pattern.** A drawable is in use from its acquire, through those ~21 ms, until the
  next frame replaces it on the screen: about 30-35 ms. iOS allows at most 3 drawables, and 3 drawables used 30-35 ms
  each give 80-100 frames a second, not 120. So the acquire waits, and the frames come 8 and 17 ms apart.
- **Presenting at a time cannot help.** `presentDrawable:afterMinimumDuration:` with 1/120 s changed nothing (17-19
  frames over 9 ms, the same 8/17 display pattern). It can only hold a frame back, and the frames are already late.
- **A CADisplayLink asking for 120 Hz changed nothing either.** With the link added to the rendering thread's run loop
  (it ticked) and the loop not waiting on it, the display still went to 80 Hz, so the refresh rate is not a vote the
  app loses. Rendering the offscreen and acquiring the drawable only at present (`RAE_DIRECT_PRESENT=0`) was already
  tried with no change.
- Instruments' Metal System Trace could not be recorded: over WiFi, `xctrace record --device` waits for the device "to
  boot" and times out. It needs the phone on USB.

What is left is the 4-8 ms before the GPU completes a frame whose passes take under 1 ms, and the two refreshes the
compositor takes: "[ui animation: why a frame takes 21 ms to reach the iPhone's screen]".

**Built in 0.3.19:** lib/Spring, the motion tokens, `AnimationPose`, the tracks and their inbox
(`animationPlay`, `animationTo`, `animationLeave`), end actions, the `animation` entry
(`animationRun`), `animationInFlight` and the testing settings, in `AnimationRules.rae` and
`AnimationTracks.rae`. A track that ends away from the node's place keeps its pose, so a node can stay
somewhere else; its next motion starts from there. Under a hold a track is not in flight, so a host's
settle does not wait for it. The hosts that run `animationRun` and feed the wait are app3d/UiShell, 106,
121, 104 and the walkers. 105 and app3d's other hosts join when a scene of theirs first animates.
Nothing starts a track on its own yet: `Appear` and the triggers are the next step.

**Built in 0.3.20:** `Appear`, `Disappear`, `Stagger`, `Leaving`, `setActiveAnimated`, `destroyAnimated` and
the bundles, in `ui/AnimationComponents` and `AnimationTriggers.rae`. What the code decided:

- `Appear` plays when a node comes to be *shown*, by its own `Active` or a parent's, and not while a parent is
  shut, so a page's rows play when the page opens.
- `setActiveAnimated` takes only the world, as written above, so an override can call it: the requests wait on the
  world, and `animationInFlight` takes the world too and counts them. A node not yet looked at has just been mounted,
  and acts at once.
- "Visible" in a stagger means overlapping the parent's box. A scrolling list's rows overlap their tall content
  parent, so the list step has to say what its window is.
- `pressable` is `pressedScale` only until `HoverScale` has its motion token (step 11).

**Built in 0.3.36:** the roll's box and `rollFrom: auto` (`textRollClip` in `renderSystem/RenderText.rae`, `textSortsAfter` in
`AnimationRules.rae`, case 1381, the chip in `checks/uiLayoutText`). What the code decided:

- The roll was always clipped to the text node's own box. A label that fills a pill-shaped chip then rolled through the
  pill's round ends, outside the chip as a person sees it. The clip is now the text's box inside the nearest shape it sits
  on (itself or an ancestor), with that shape's corner radius when the text covers the whole shape. A parent's
  `Overflow: Clip` was already on the canvas and still cuts it too.
- `auto` decides once, when the text changes, and the old text keeps the side (`TextGhost.rollFrom`). A change in the middle
  of a roll compares the new text with the text that was coming. Each run of digits is read as a number, anything else byte
  by byte, and a text that is the other with more after it sorts after. The same text rolls from below.

**Built in 0.3.21:** `AnimateLayout`, `AnimateText` and `glideOnChange`, with the rules in `AnimationTriggers.rae`
and `AnimationRules.rae` and the painting in `renderSystem/RenderText.rae`. What the code decided:

- A node's place counts `ComputedRect`s only, up to its nearest ancestor with `AnimateLayout`. A scroll is a
  `RuntimeOffset`, which is not in it, so scrolling starts nothing. A marked child inside a marked parent does not
  move twice: only what changed inside the parent glides.
- "A window resize" is a change of `world.layoutW` or `layoutH` in the layout that saw the move. Every record then
  takes the new place with no motion.
- **The keyboard is not a resize (0.3.40).** A host that lays the page out shorter or taller for the on-screen keyboard
  calls `layoutKeyboardMoves(world, seconds)` first. The next change of the layout's size then glides every node with
  `AnimateLayout` to its new place on the `keyboard` motion token, whatever the node's own motion: a curve
  (`easeOutQuart`, built in at 0.25 s), as long as the platform says the keyboard takes. `Gpu2d.keyboardSeconds()` is
  that: on iOS UIKit's `UIKeyboardAnimationDurationUserInfoKey`, read with the keyboard's frame; 0 elsewhere, which keeps
  the token's own length. UIKit's keyboard curve is a private one, and `easeOutQuart` is close to it. The mark is used
  by one change; a change of the safe area's bottom inset is no resize and glides anyway. Case 1385.
- A hidden node forgets its place, so it comes back where it is laid out, and its `Appear` is what it plays.
- A glide on a node already moving adds the difference to its x and y and keeps their velocity. Its opacity, scale
  and rotation carry on, so a glide can start in the middle of an `Appear`.
- A text changing while the last change still shows: the text that was coming becomes the old text, at the alpha it
  had and, in a `roll`, at the place it had. The new text starts at the alpha and the place that are left, so the
  text that was on its way out is dropped at once. With one old text kept this is the smallest jump there is.
- The old text is drawn only in `paintTextWithGhost`. Layout, measuring and the hit test never see it. A `roll` is
  clipped to the node's box.

## 11. Testing

Motion has to be testable by an agent with no eyes on the phone.

| Setting | Meaning |
| --- | --- |
| `RAE_ANIMATION=off` | Every motion is at its end at once. Fast, for tests that do not test motion |
| `RAE_ANIMATION=slow:5` | Five times slower, for a person judging it |
| `RAE_ANIMATION_HOLD=0.4` | Every animation stops at 40% of its way and stays. A screenshot then shows a true middle state, the same every time |

- **Unit tests** (`AnimationRules`, `lib/Spring`): a spring ends at its target, at any
  frame rate, without a jump in position when retargeted. Delays obey `maxSeconds`.
- **Fixtures** (the lib/ui way): `AnimationPose` folds into the same world rectangle
  for paint and hit test. Nothing runs and nothing draws after the tracks end. A
  `Leaving` node takes no hit. A mid-flight retarget has continuous position. Scrolling
  a list starts no track.
- **Checks of world places.** A check of whether rows jump reads *laid-out* places when
  it wants layout's truth and *world* places when it wants what a person sees. World
  places must move by less than a limit per frame: this is the test that a transition
  has no jump, and it applies to every row in flight.
- **Screenshots.** A script takes a strip of shots of one transition at 0, 25, 50, 75
  and 100% by `RAE_ANIMATION_HOLD`, for review.
- A host's `settle` step also waits for `animationInFlight` to be false.

## 12. The editor and the examples

- **`examples/121_ui_editor`**: the inspector edits the new components. A Play button
  plays the selected node's `Appear` or `Disappear`, or the whole page with `Stagger`,
  at normal or slow speed. Its own panels and dialogs use the bundles. The `motion` tokens
  are edited with the theme, and a small curve graph shows each one.
- **`examples/106_mobile_ui`**: the reference app uses all of it: `push` and `sheet`
  pages, lists with `Stagger`, row enter and leave, its hero as `SharedElement`, and the
  edge swipe. Its DESIGN.md gets a section. Other UI examples get it when a bundle
  is added to their scenes.
- `docs/ui-scene-format.md` documents the components and the `motion` section.

**Built in 0.3.33:** the motion tokens in the editor, `motionTokenSystem` and `lib/ui/ThemeWriter.rae`. What the code decided:

- A panel under the hierarchy lists every token the page can name, the built-ins first so a save does not reorder the rows. It shows only for a
  document whose theme declares a motion or whose page uses one, so a document with none looks as it did. The token fields are typed like an
  animation component's, and a wrong value is refused in the status line.
- `MotionToken` carries its `name`. A commit sets the theme and calls `animationMotionChanged`, which retargets every channel on that name from where
  it is, as any second command does. A command's own `opacitySeconds` has no name, so it stays what the command said.
- The graph is 32 thin rectangles from `motionSamples`, which steps a real channel, so it shows what the motion does. Its width is 0.8 s for every
  token, so two can be compared by eye. A faint line marks the end value.
- The save writes each edited token to the nearest file that declares it: the page, else its imports in order. A built-in no file declares goes to
  the first import, or the page when it has none. `ThemeWriter` works on the text, so the rest of that file stays byte for byte; a parsed `JsonDoc`
  has no positions, and a theme file is shared. A reload drops an edit not saved, as it does a page's. Adding or removing a token is not offered.

**Built in 0.3.32:** the editor's half, in `inspectorSystem/MotionEdit.rae`. What the code decided:

- The seven components' rows were already generated. A click on a field is now the label input, and a commit rewrites the
  component's JSON with that field changed and decodes it by the scene's own path. So an enum, a number and a motion name take no code, an
  unknown motion is an ordinary diagnostic, and the document's save writes it.
- Play rows show when the page has motion: the selected nodes' `Appear` or `Disappear`, the page's `Appear` in `Stagger` order, and normal or
  slow (four times). They need a selected node, as the inspector's rows do. A played `Disappear` comes back at its end, and a played `Appear`
  goes through the triggers, so a `Stagger` above it counts. `PageTransition` is edited but not played: it needs a second page.
- The editor's pills use `pressable` and its panels `appearFade`. It has no dialogs of its own.
- A component a node inherits from a bundle, changed after load, is now saved (`SceneWriter`, case 1378). Before, the save dropped the change
  without a word. It writes the whole component on the node, and that merges over the bundle on load.

**Built in 0.3.31:** example 106, whose DESIGN.md section 10 has the list. What it decided for lib/ui:

- `pageTransitionSetMorphKey`: two pages can show the same many things, and then one key flies, not all of them.
- One key on the old page twice is one source, the first on screen.
- A morph target is marked seen even if its page was shown before, or its `Appear` would pull it away.
- A hidden flow list drops a row that went at once. A row with `Leaving` holds its layout place, so an album opened with new rows would
  show the old ones too.
- `findScrollRoot` skips a page on its way out.
- A size-class frame restoring a node now expands its `bundles`. Before, a node with a bundle that a frame named stopped the app.
- 106's hero stays authored as `HeroWidget`, the alias. Its keys are written by list bindings and app systems, which know that component; a
  move to `SharedElement` in the scenes would change those and nothing that shows.

## 13. SUMU: every screen

SUMU's pages are transparent over a background world, and its top bar, notification
stack and screensaver are their own worlds. Only the screen's world takes part in a
page transition; the background and the top bar stay still.

Nearly all of this is in scene files and the theme, not in code. So an app gets it with
no SDK change, and a hosted WASM app gets it too.

| Screen or widget | Motion |
| --- | --- |
| Launcher | `Apps` title `appearFade`; icons `appearPop`, `Stagger` from the bottom row |
| Launcher to an app | `zoom` from the tapped icon's rectangle. Back shrinks into that icon |
| Inside an app (Day, Week, Month; Counter's screens) | `sibling`; direction from the order of the manifest's `screens`. `SharedElement stay` on the bottom bar and the add row; the title changes by `AnimateText` |
| Shell screens (Sync) | `sheet`: from the bottom, back down |
| Every bottom bar | Selected slot's color and icon change with a short fade; `pressable` on each slot |
| Every button | `pressable` through the theme's button bundles |
| Dialogs (TodoTask, TodoTime, TodoSettings, RemoveTask) | `dialogMotion`, the scrim fades, the task dialog scales back under the time dialog |
| TODO Day | Rows `appearRise` and `staggerFromEnd`; added row enters and others glide; removed row leaves; ticked row glides (as now); previous and next day send rows sideways by `change`; `Earlier` heading `appearFade` |
| TODO Week | Rows `appearFade`, `staggerFromEnd`; chosen day's fill glides between days |
| TODO Month | Weeks `appearFade` and stagger; chosen day's fill glides between cells; the list above the grid as in Day |
| The line of the time now | `AnimateLayout` so it glides each minute, and `appearFade` |
| The time chip and the time dialog | `AnimateText` `roll`: up for later, down for earlier |
| Counter | The value `AnimateText` `roll`; the big screen's number `appearPop` |
| Weather | The hero `appearPop`; hours `appearRise` staggered across from the left; days staggered from the bottom; place rows `staggerFromEnd` |
| Sync | Fields and rows `appearRise`, staggered; state text `AnimateText fade` |
| Top bar | At the first start, parts `appearFade`; the pill's words and the clock `AnimateText fade`; the battery with no change |
| Background | At the first start, the orb `appearPop` and the wordmark `appearFade`; the pulse stays its own system |
| Screensaver | Comes in with a slow fade and a scale from 0.96; leaves with `quick`; its own move each minute stays |
| Notifications | New cards `appearDrop`; the others glide; a swipe continues at the finger's speed and fades; the hand-written rates in `notificationSystem` go |
| Text field | Focus edge fades in; keyboard up: content glides by `AnimateLayout` |
| Red failure screen | `none`. A failure must show at once |

**SDK.** Scenes need none. One addition: `listRowsEdit` gets the optional `change`, as
Previous and Next are known to the app only (SDK version up, an older hosted app simply
has no direction).

**Self-tests.** SUMU's frame systems run `animationSystem` for each of its worlds and
add the in-flight flag to `RenderDecide`; `settle` waits for it. `FrameRowJump` reads
world places for the rows that are not moving by design. Most self-tests run with
`RAE_ANIMATION=off` for speed, and new ones test motion on.

## 14. The order of work

Each step ends with something shippable. The numbers are the queue's.

1. **Transforms**: subtree scale about a pivot; paint and hit test agree (section 6).
2. **Core**: `lib/Spring`, `motion` tokens, `AnimationPose`, `AnimationTrack`, inbox,
   schedule entry, in-flight flag, `RAE_ANIMATION` and `RAE_ANIMATION_HOLD`.
3. **Appear, Disappear, Stagger**: components, triggers, `Leaving`, built-in bundles.
4. **AnimateLayout and AnimateText**.
5. **Lists**: first fill, enter, ghosts, `change`, listMotionSystem on tracks.
6. **Dialogs**: two-step close, scrim, `dialogMotion`, a dialog over a dialog.
7. **Page transitions**: both page models, all kinds, interruption.
8. **SharedElement**: stay, morph, the hero moved onto it.
9. **Edge swipe**: `pageTransitionDrag`.
10. **Reduced motion and frame rate**, with the budget measured on a phone.
11. **Older systems on tracks**: `HoverScale` motion token, carousel, switch.
12. **Example 106**.
13. **UI editor**.

SUMU's tasks follow Rae's, each named after the Rae step it waits for.

## 15. Not in this design

Each of these can come later with no API change:

- **Group opacity** by rendering a subtree to a texture.
- **Blur** behind a sheet or a dialog. Needs a backdrop pass in Gpu2d. It would be the
  next step toward an iOS look, and a scrim is good enough to start.
- **Size animation.** `AnimateLayout` moves a node and does not resize it, since a
  changing size reruns layout each frame. `morph` draws a resized box, with no layout.
- **Keyframe timelines.** Springs and curves from A to B cover everything in this
  document. Add a timeline only when one screen needs it.
- **Looping effects** (`WobbleFx`, `SmokeFx`, a background pulse) stay as they are: they
  have no start or end for a track to manage.
