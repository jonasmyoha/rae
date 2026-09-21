# App-specific components live in the app: the extension world

Status: **design, not implemented** (2026-09-21). Prompted by the UI editor
having to call a music-player system to draw a scene the way the music player
does. Related: `ecs-general-architecture.md`, `compile-time-reflection.md`,
`ui-editor-design.md`.

## 1. The problem

`lib/ui` is the standard UI library, and today it carries the *vocabulary of
two applications*. `lib/ui/BindingComponents.rae` declares 29 authorable
components; only nine of them are generic:

| bucket | components |
|---|---|
| generic UI (stays in lib) | `ListSource`, `SearchField`, `EmptyState`, `AnchorBottom`, `LayerTag`, `BottomSheet`, `SheetPanel`, `ProgressBar`, `Camera2D` |
| music player (`examples/106_mobile_ui`) | `ProfileStat`, `AvatarSource`, `HistoryList`, `HasHistory`, `NavTab`, `AlbumHeader`, `PlaybackIcon`, `PlaybackCover`, `LikedHeart`, `TrackText`, `CoverStyle`, `PlaybackTime`, `HeroTransition` |
| UI editor (`examples/121_ui_editor`) | `EditorPanel`, `PanelToggle`, `EditArea`, `EditorList`, `GuideRect`, `DiagnosticsBadge`, `OutlineFor`, `OutlinePool` |

Each of the twenty app components is also a `ComponentTable` field on
`UiWorld` (`lib/ui/Ecs.rae`) and a `registerComponent` line in
`lib/ui/RegistryFlags.rae`. Two app *systems* followed their components into
lib as well: `lib/ui/coverStyleSystem/CoverStyleSystem.rae` (the music
player's "rounded album covers" setting) and
`lib/ui/animationSystem/HeroTransitionSystem.rae` (a generic-looking name for
a system that exists for the album cover fly-through; it is a candidate to
stay generic, see §6).

Why it ended up this way is one sentence: **"the registry IS the world"**
(`lib/ui/Registry.rae`). The `.raescene` loader is a compile-time loop over
`fields(world)` for `world: mod UiWorld`, so a component is authorable if and
only if it is a table on `UiWorld` — and `UiWorld` is a closed lib type. Every
app that wanted an authorable component had to edit lib. The album-cover
components are the symptom; the closed world is the defect.

What it costs:

- The stdlib knows what an album is. A CLI tool that opens `lib/ui` links a
  `LikedHeart` table.
- Two apps' names share one namespace: `NavTab` is the music player's tab bar;
  the next app with tabs cannot have a `NavTab` of its own.
- A generic editor cannot be generic. To show a 106 scene the way 106 shows
  it, the editor has to *run 106's systems* — it now calls
  `coverStyleSystem(rounded: true)` on every document mount
  (`examples/121_ui_editor/documentSystem/DocumentSystem.rae`), i.e. it knows
  a music-player setting's default. That is the wrong direction and is
  reverted by this design (§5).

## 2. The rule

**A component or system goes in `lib/ui` only if a UI in general needs it.**
The test is the one `AGENTS.md` already uses for `core` vs `collections`: lib
holds what the *pipeline* understands — layout, shape, text, sprite, input,
scroll, animation, camera, list views, sheets. A component that names a
domain object (album, track, playback, avatar, the editor's panels) belongs to
the app that has that domain, next to the system that reads it.

Corollary for the editor: **the editor runs the lib pipeline and nothing
else.** Whatever an app wants a scene to *look like* in the editor must be
expressed in generic components in the scene. An app system may vary that
look at runtime (a setting that squares the covers), but the authored data
is the default, and the editor shows the authored data.

## 3. The mechanism: an app extension world

Rae already has the two pieces this needs, both landed:

- **Generic world parameters with the field loop** (#773,
  `compile-time-reflection.md`): `func f(W: type, world: mod W) { loop let
  table: mod ComponentTable(any) in fields(world) { ... } }` expands per
  instantiation. `clearEntityComponents` and `worldToJson` are written this
  way today.
- **Sparse-set tables keyed by `EntityId`** (`lib/ecs/ComponentTable.rae`): a
  table does not care which struct it is a field of. An app table and a lib
  table can hold components for the same entity.

So an app declares its own world struct of tables, and the lib functions that
today take `mod UiWorld` and loop over its fields take the app world as a
second, generic argument:

```rae
# examples/106_mobile_ui/MusicWorld.rae — the app's authorable vocabulary.
# ONE table per component, the same shape as UiWorld; the loader, the
# serializer and destroyEntity walk it with the same compile-time loop.
type MusicWorld {
  profileStats: ComponentTable(ProfileStat)
  avatarSources: ComponentTable(AvatarSource)
  navTabs: ComponentTable(NavTab)
  albumHeaders: ComponentTable(AlbumHeader)
  playbackIcons: ComponentTable(PlaybackIcon)
  playbackCovers: ComponentTable(PlaybackCover)
  likedHearts: ComponentTable(LikedHeart)
  trackTexts: ComponentTable(TrackText)
  albumCovers: ComponentTable(AlbumCover)   # was CoverStyle, see §5
  playbackTimes: ComponentTable(PlaybackTime)
  # ...
}
```

The lib entry points gain the extension parameter. There is exactly one
type parameter, which is what the compiler supports today (two-type-param
generics are not usable yet):

```rae
# lib/ui/SceneLoader.rae
func loadSceneFile(Ext: type, world: mod UiWorld, extension: mod Ext, path: view String, ...)
# lib/ui/Registry.rae
func applyComponentByName(Ext: type, world: mod UiWorld, extension: mod Ext, entity, name, doc, val, ...)
# lib/ui/Ecs.rae
func destroyEntity(Ext: type, world: mod UiWorld, extension: mod Ext, entity: view EntityId)
func unmountPage(Ext: type, world: mod UiWorld, extension: mod Ext, pageId: view String)
# lib/ui/Serialize.rae
func worldToJson(Ext: type, world: view UiWorld, extension: view Ext, ...)
```

`applyComponentByName` runs its existing loop over `fields(world)` and then
the same loop over `fields(extension)`; a key that matches neither is the
same hard error as today (#941). Because both loops fold at compile time, an
app whose extension has no table matching a key instantiates no decoder for
it — same cost model as now.

**An app with no components of its own passes the empty extension**
`lib/ui` provides: `type NoExtension {}` and `let none: NoExtension = {}`.
`fields(none)` is empty, the second loop vanishes. The 3D examples and the
CLI apps change one argument and nothing else.

Entity lifecycle is the part that must not be forgotten: `destroyEntity`,
`destroyEntitySubtree` and `unmountPage` clear the entity from **both**
worlds, or the extension leaks dead-entity rows. With the extension as a
parameter this is `clearEntityComponents(Ext, extension, entity)` next to the
existing lib call — one line, no hook or callback (Rae has no function
references, and this design needs none).

Systems do not change shape. A lib system takes `mod UiWorld` as today. An
app system takes what it reads: `func likedHeartSystem(world: mod UiWorld,
music: mod MusicWorld, ...)`. The app's schedule already threads its
resources explicitly (no globals); the extension world is one more resource
on the `App`, threaded the same way.

### What this is NOT

- Not a registry of closures, not a runtime `Map(String, Spec)` — the
  reference architecture's shape, which Rae rejected in #961 for the
  compile-time loop. The extension keeps that decision: the loop is just run
  over two structs instead of one.
- Not nesting (`type MusicWorld { ui: UiWorld ... }` with a recursive field
  walk). Nesting would need `fields()` to recurse into struct-typed fields, a
  compiler feature that does not exist; two flat worlds need nothing new.
- Not `@component` / `derive` / any sigil. The extension is a plain struct
  of plain tables; the compile-time loop is the existing `fields()`.

## 4. The editor and components it does not know

A scene of a project can name components the editor has never heard of —
that is the *point* of the split. The editor's behaviour, in order:

1. **Preserve.** An unknown key is not an error in the editor. The node keeps
   it as an opaque `AuthoredExtra { name, json }` row (an editor-side table),
   so a scene the editor saves round-trips byte-for-byte on the parts it does
   not understand. This is how the reference editor treats foreign
   components and it is the only behaviour that lets an editor be used on
   someone else's project.
2. **Show.** The inspector lists the opaque component by name with its raw
   fields, read-only, marked "not a lib component". The Diagnostics panel
   counts them under a new `unknownComponent` kind at *info* level, not
   error — the app knows them, the editor does not.
3. **Render with the lib pipeline only.** Whatever the app's systems would
   have derived from that component does not happen in the editor. That is
   correct by §2: the authored generic components carry the default look.

A later step, out of scope here, is an app-hosted editor: `lib/uiEditor/`
as a library the app links with its own extension world and systems, so the
editor renders *with* the app's systems. That is the reference editor's
"editor mode inside the game" and needs this split first; nothing in this
design blocks it.

## 5. The worked example: rounded album covers

Today: the cover node authors `CoverStyle { radius: 52 }` and *no*
`CornerRadius`. `coverStyleSystem(world, rounded)` (lib) sets a
`CornerRadius{52}` when the app setting `roundedAlbumCovers` is on and
removes it when off. The scene as authored is square; only a running 106
rounds it. The editor showed it square until it started calling the 106
system.

After — the rounding is generic data, the *setting* is the app's:

```jsonc
// MusicPlayerNowPlaying.raescene — the default look is in the scene.
"Hero": {
  "Rect": { ... },
  "Sprite": { "textureKey": "albumCover", "scaleMode": "Fill" },
  "CornerRadius": { "radius": 52.048193 },   // generic: any renderer rounds it
  "AlbumCover": {}                            // app marker: "this is a cover"
}
```

- `CornerRadius` is the generic component every renderer — the app, the
  editor, a screenshot tool — already understands. The scene is rounded by
  itself.
- `AlbumCover {}` is a music-player tag in `MusicWorld` (the former
  `CoverStyle`, minus its `radius` — the radius no longer needs a second
  home). It marks which nodes the setting applies to.
- `albumCoverStyleSystem(world, music, rounded)` moves to
  `examples/106_mobile_ui/`. When the setting turns rounding **off** it
  removes `CornerRadius` from every `AlbumCover` node after recording the
  authored radius in its own table (`albumCoverRadii: ComponentTable(
  AlbumCoverRadius)`); when it turns rounding back **on** it restores it from
  there. Change-disciplined as today: writes only on a transition.
- The editor's `coverStyleSystem(rounded: true)` call (`DocumentSystem.rae`)
  is deleted. The editor shows the scene rounded because the scene says so.

Same treatment applies to every "the app decides the look at runtime"
component: the authored scene carries the default in generic components; the
app's system varies it and remembers what it overrode.

## 6. Migration

Ordered so every step leaves the tree green; each is one queue item.

1. **Rounded covers as data (no new mechanism).** Author `CornerRadius` in
   the seven 106 scenes with covers; `coverStyleSystem` becomes remove/restore
   (still in lib for this step); delete the editor's call. 106's #981 pixel
   gate keeps proving the toggle. Unblocks the editor immediately.
2. **`NoExtension` + the `Ext: type` parameter** on `applyComponentByName`,
   `loadSceneFile`/`loadScene`, `destroyEntity`/`destroyEntitySubtree`/
   `unmountPage`, `worldToJson`/`worldFromJson`. Every caller passes
   `NoExtension.none`. Zero behaviour change; fixture 839's pipeline counts
   and the 116 snapshot round-trip are the regression net.
3. **Move the 121 vocabulary** into `examples/121_ui_editor/EditorWorld.rae`
   (eight components; their systems are already in the editor). The editor
   is the first real extension user and the smallest.
4. **Move the 106 vocabulary** into `examples/106_mobile_ui/MusicWorld.rae`
   (thirteen components), `coverStyleSystem` with it (renamed
   `albumCoverStyleSystem`, §5). `HeroTransition` is the one to decide: the
   system is a generic shared-element transition keyed by `HeroWidget` and
   drives a plain `Sprite`+`CornerRadius`; if it stays in lib it is renamed
   and documented as generic and its `HeroWidget` stays generic too;
   otherwise both move. Recommendation: **stays** — a hero transition is a
   UI pattern, not a music-player concept.
5. **Editor: opaque unknown components** (§4) — `AuthoredExtra` table,
   inspector rows, `unknownComponent` info diagnostic, save round-trip
   fixture. After step 4 the editor *must* have this to open a 106 scene at
   all.
6. **Docs sweep**: `ui-ecs-106-reference.md`, `ecs-api-reference.md`,
   `ui-editor-design.md`, the `AGENTS.md` "registry IS the world" note gains
   "…plus the app's extension world".

Steps 1 and 2 are independent and can go first in either order. Step 3
before 4 because the editor must preserve unknown components (5) before 106's
components leave lib, and 5 is easiest to build on the editor's own moved
vocabulary.

## 7. Decisions to confirm

- **Two flat worlds, one `Ext: type` parameter** (§3) rather than waiting for
  nested field reflection or multi-type-param generics. Recommended: yes —
  no compiler work, and the folded loops keep today's cost.
- **`HeroTransition` stays generic** (§6 step 4). Recommended: yes.
- **Unknown components in the editor are preserved, not rejected** (§4).
  Recommended: yes; this is what makes the editor usable on any project.
- **Bare `unmountPage(world, pageId)` keeps working for extension-free
  apps?** Either every caller passes `NoExtension.none` (uniform, more
  churn) or lib keeps a one-line non-generic wrapper. Recommended: the
  wrapper, so the 3D examples and CLI tools do not change at all.
