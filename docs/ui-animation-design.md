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
`rollFrom: "above"` for one that goes down).

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

**In flight.** `animationInFlight(uiSystems)` is true while a track exists and is fed
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

### 8.4 A swipe from the edge

`pageTransitionDrag(progress, velocity)` runs a transition from a finger, not from
time: a right swipe from the left edge drags the page back. On release, the page
continues, or springs back, from the finger's velocity. Since a track retargets from
the current value and velocity, this needs only a paused state, and no new motion
math. It is one thumb's natural Back. The bar's Back stays.

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
