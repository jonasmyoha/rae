# Globals and App ownership — the no-globals rule

**Status:** decided (language rule). Enforcement is phased — see "Rollout" and the
QUEUE items (#762–#766). The compiler bugs listed under "Prerequisites" MUST land
before the rule is enforced; without them the rule creates pain instead of
removing it.

## The rule, in ECS terms

Rae is an ECS-first language. In ECS vocabulary the rule is one sentence:

> **All program state is a component, a resource, or a constant. Nothing else.**

- A **component** is per-entity state in a `ComponentTable(T)` on a World.
- A **resource** (#753, `docs/ecs-resources.md`) is world-global singleton state:
  a plain field on the World, created with it, passed down, dropped with it.
- A **constant** is a compile-time value (`const`), immutable, heap-free.

There is no fourth category. A module-level `var`, or a module-level `let` that
holds heap (`String`, `List`, any struct carrying heap), is a **compiler error**
(after rollout). Module-level `const` is allowed.

The `App` is the root of that tree: `main` builds it, everything hangs off it,
and it is passed down. An App is, structurally, "a World plus its Schedule plus
its resources" — the same shape the ECS already gives you.

```rae
type App {
  world: GameWorld        # entities + component tables
  schedule: Schedule      # the ordered systems (#755)
  gpu: Gpu2dContext       # a resource wrapping a runtime handle
  theme: Theme            # a resource (was a global in lib/ui/theme.rae)
  settings: Settings      # tuning knobs, with defaults
}

func main() {
  var app: App = createApp()
  loop appShouldRun(app: app) {
    tick(app: app)
  }
}
```

That is the ONE blessed shape. Two developers writing the same app converge on
the same structure because the language leaves nowhere else to put state.

## Why (the evidence is already in this repo)

Constants and mutable state are different problems that share a location. A
`const` has no lifetime, no allocation, no init order — nothing to get wrong.
Mutable global state has all three, and no owner, which is why its lifetime and
dependencies are unknowable. Rae's ownership model (`own`/`view`/`mod`) has no
way to express "nobody owns this"; a global is the one thing that escapes it.
"No globals" is not a new rule — it is the ownership model with no exemption.

Concrete failures this repo has already paid for:

- `lib/ui/theme.rae` holds `var activeTheme` AND a separate token global.
  `lib/app3d/ui_shell.rae:74` documents the result: *"palette slots resolve
  through a different global … nothing was bridging the two"* — a scene's
  theme colour silently rendered as the built-in mint. Two singletons, an
  invisible dependency between them.
- `lib/ui/RenderSystem/RenderSystem.rae:6`: fonts are *"PASSED IN via Gpu2dUi
  (module-level heap globals miscompile)"* — the compiler itself already pushes
  toward the App-struct shape.
- Authors instinctively mark globals as "different": `gProfNames`,
  `gCompositeBindMade`, `g_themeGeneration`. A `g` prefix is a confession that
  the thing does not fit the normal rules.

And the win that is specific to Rae: **Live mode.** `lib/hot_reload.rae` already
serializes app state to survive a rebuild. When ALL state is one `App` tree,
"snapshot the whole program" is `app.toJson()` — hot reload, time travel, and
the inspector fall out of the no-globals rule for free. With state scattered in
module globals across files, none of that is possible.

Uniformity also serves the stated design goal: an AI agent reading any Rae app
knows where state lives and how it is wired (constructor arguments = the
dependency graph).

## What is allowed at module level

| Form | Allowed? | Why |
|---|---|---|
| `const maxRetries: Int = 5` | yes | compile-time value, no lifetime |
| `const origin: Vec2 = { x: 0.0, y: 0.0 }` | yes | POD struct literal, no heap |
| `let defaultFontSlot: Int = 0` | migrate to `const` | it IS a constant; `let` is the wrong spelling |
| `var frameCount: Int = 0` | **no** | mutable state → a resource on App |
| `var names: List(String) = createList(...)` | **no** | heap + mutable → a resource |
| `let table: StringMap(Int) = createStringMap(...)` | **no** | heap; has a lifetime → a resource |

Constants follow the naming rule: camelCase, never `SHOUTING_SNAKE_CASE`.

### Const walls vs. tuning knobs

A file that opens with twenty consts is fine IF they are genuinely fixed. Two
smells to split out:

1. **Pre-multiplied magic numbers** — `let debugRowH: Float = 121.44578313253011`
   is a design-unit conversion baked into a literal. Express it as a function of
   a base unit; that is a design-token problem, not a globals problem.
2. **Knobs the app may vary** (an editor, hot reload, a settings dialog) — these
   are state. Put them in a defaults-bearing struct that is a resource on App
   (`app.settings.debugRowH`). Then Live mode can tweak them.

If neither applies, `const` at module level is the right place. Do NOT wrap
consts in a struct just to namespace them: the module already IS the namespace
(`open ui/theme`). Java puts everything in classes because it lacks modules and
free functions; Rae does not have that problem, so do not import the workaround.

## The C / runtime boundary — no `unsafe`, `extern` is the exception

Some state genuinely IS process-global: the GPU device, the window, stdout, the
allocator, the clock, the profiler ring. The rule is **"no globals in Rae
code"**; the C runtime underneath stays global, and that is fine. Rae reaches it
only through `extern` functions, which already exist and already mark the
boundary:

```rae
func rae_ext_rae_chan_new() extern ret Int
```

So:

- **Do not add an `unsafe` keyword.** `extern` is the exception mechanism, and it
  already exists. An `extern` function may touch C-side globals; Rae code may
  not declare any. There is exactly one door and it is already labelled.
- Runtime-global objects are exposed to Rae as **handles owned by the App**.
  `lib/gpu2d.rae`'s `var gG2dColorAtts` becomes a field of a `Gpu2dContext`
  that `createApp` builds and `app.gpu` owns; `lib/profile.rae`'s `gProf*` lists
  become a `Profiler` resource. The C state may be global; the Rae-side
  *wrapper* is not, so its lifetime and init order are explicit.
- If a handle must exist before any App does (e.g. the allocator), it is created
  inside `createApp` as the FIRST field — ordering is then a struct-literal
  order, which is visible and deterministic. There is no static-initialisation
  fiasco because there is no static initialisation.

## Prerequisites — compiler bugs to fix FIRST

The rule makes every function take `app`/`world` as `mod`. That only works if
mutation through a `mod` parameter is reliable. Today it is not:

- **Nested-field `mod` write-back is unreliable and fails silently.** Cited in
  `lib/app3d/slider.rae:182`, `lib/app3d/settings_dialog.rae:90`,
  `lib/renderer_deferred.rae:387`: passing `app.io.ui` (a nested struct field)
  as a `mod` argument does not always write back. Under a no-globals rule this
  is THE access pattern, so people would reach for globals precisely to escape
  the bug. **#762 must land before any enforcement.**
- **Module-level heap globals miscompile** (RenderSystem note). Moot once they
  are banned, but it is the same class of bug and worth a regression test.
- **`getChildren`-style owned returns bound to a `let` are not dropped** (#761's
  cousin, observed in test 704). App-tree ownership leans on cascade-drop being
  complete; every leak in the App tree is now a leak of the whole program.

## Rollout (sequencing against the ECS refactor)

The ECS refactor (#731–#750) builds the `GameWorld`/`App` roots for 112 and 114.
Decision: **write the rule now, enforce it last, and let the ECS migration land
in the final shape in between** — so 112/114 are structured once, not twice.

1. **Now (sidequest, small):** this doc; #762 fix nested-`mod` write-back;
   #763 add the diagnostic as a WARNING (module-level `var` / heap `let`);
   #765 convert the ~112 scalar module-level `let`s to `const` (mechanical).
2. **Then the ECS refactor continues (#731–#750)** under the rule: new
   GameWorlds and Apps put every singleton on the World/App, never at module
   level. The warning catches regressions as they are written.
3. **After #750:** #764 migrate the lib-level globals (`theme.activeTheme`,
   `gpu2d.gG2dColorAtts`, `gbuffer_passes.gTaa*`/`gComposite*`,
   `profile.gProf*`) into App-owned resources — these libs are reworked by
   #737/#748 anyway, so doing it after avoids double work.
4. **Last:** #766 flip the diagnostic from warning to hard error; suite green.

## Systems, under this rule

Nothing changes for systems — they were already the right shape:
`func layoutSystem(world: mod UiWorld)`. A system takes the World (or App) it
acts on and reads/writes components and resources on it. A system that needs
"global" configuration reads a resource. A system that needs another system's
output reads the component that system wrote. The Schedule (#755) fixes the
order. There is no hidden channel between systems because there is nowhere to
put one.
