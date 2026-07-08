# Rae UI theme system — design document

Status: PROPOSED (design only, no implementation). 2026-07-08.

The guiding principle under evaluation, which this document adopts as
the test for every decision below:

> UI scenes should describe **what** visual style they want, not
> **how** that style is built.

## 1. Where we are

### What already works

- **Semantic colors.** `ThemePalette` (lib/ui/theme.rae) has nine
  role-named slots (`surface`, `textPrimary`, `accentText`, …). Scenes
  write `"fill": "surface"`; light/dark themes flip for free. This is
  the one place the "what not how" principle is already mostly real.
- **Semantic spacing.** `spaceXS…spaceXL` (lib/ui/spacing.rae) with a
  documented intent scale, and scene support via `optFloatOrToken`
  (`"gap": "spaceM"`). Example 98 uses it consistently; 106 largely
  predates it and still writes raw design-unit floats.
- **Style ids on text.** `Text.styleId` means scenes already say
  `"styleId": "miniTitle"` — the *reference* side of the design is in
  place. What's missing is a real *definition* side.

### What doesn't

- **Text styles are code, not data.** `styleId` resolves through
  hardcoded if-chains — `textStyleSize` / `textStyleColorRgba` /
  `textStyleIsCentered` in lib/ui/text_style.rae — and those chains
  are **duplicated** as `g2dTextStyleSize` / `g2dTextStyleColor` /
  `g2dTextStyleIsCentered` in lib/ui/render_gpu2d.rae (and a third
  raylib copy in legacy render.rae). Adding a style means editing 2–3
  files of compiler-recompiled Rae code; the theme does not own text
  at all.
- **Colors are baked at parse time.** The scene loader resolves
  `"tokenName"` against `activeTheme` while parsing, so a runtime
  theme switch needs a scene re-parse + world rebuild. Workable, but
  it means the theme is a parse input rather than a live asset.
- **Per-entity visual attachments are the path of least resistance.**
  The recent `TextShadow` commit (e39a378) is the canonical example of
  what this document exists to prevent (see §2).
- **106 is drifting numeric.** Newer scenes author raw floats
  (`"gap": 26.024096`, `"radius": 69.39759`) where 98 would have said
  `spaceM`. Every such number is a tiny fork of the design system.

### Case study: the TextShadow commit (what NOT to do)

e39a378 added a `TextShadow` ECS component and authored it inline in
mini-player.raescene:

```json
"Text":       { "text": "Unsayable", "styleId": "miniTitle" },
"TextShadow": { "color": {"r":0,"g":0,"b":0,"a":115},
                "offsetX": 3.253012, "offsetY": 3.253012,
                "softness": 8.674699 }
```

Everything about this is *how*, not *what*:

- The scene says `miniTitle` — and then re-states, per entity, a
  shadow that is really part of what "miniTitle on the accent panel"
  *means*. The style id stopped being the whole truth.
- `MiniArtist` repeats the block with slightly different numbers.
  Two entities in, the values have already forked (a:115 vs a:100,
  softness 8.67 vs 6.51). Twenty entities in, nobody can change the
  app's text-shadow language without a scene-wide hunt.
- The numbers are unexplained magic (`3.253012` is `spaceXS`-ish in
  du after unit conversion, but nothing says so).
- A sixth ECS table (+ registry entry, + destroy hook, + test-count
  bump) was paid for something that is not per-entity state at all —
  it is a property of a *named style* that happens to be used by two
  entities.

The correct shape (post this design): `miniTitle` and `miniMuted`
*styles* own their shadows; the scene stays exactly as it was before
the commit — `"styleId": "miniTitle"` and nothing else. The
`TextShadow` component may survive as the override escape hatch (§6),
but it must stop being the authoring path.

The lesson generalises: **any visual property that would be authored
with the same values on more than a couple of entities is a style
property, not a component property.** Real apps have 8–20 text
styles, a handful of radii, five spacing steps. The system should make
the named-style road the paved one.

## 2. Proposed architecture in one picture

```
        theme file (data, JSON)                 scene file
  ┌──────────────────────────────┐      ┌──────────────────────────┐
  │ palette:  surface, accent…   │      │ "Text": {                │
  │ text:     miniTitle {…}      │◄─────│   "styleId": "miniTitle" │
  │           miniMuted extends… │ refs │ }                        │
  │ space:    spaceM = 12pt      │      │ "Layout": {              │
  │ radius:   radiusCard = 16pt  │◄─────│   "gap": "spaceM" }      │
  │ shadow:   textOnAccent {…}   │      │ "CornerRadius":          │
  │ padding:  cardPadding {…}    │◄─────│   "radius": "radiusCard" │
  └──────────────┬───────────────┘      └──────────────────────────┘
                 │ loaded + flattened at boot / theme switch
                 ▼
        ResolvedTheme (parallel-list tables in UiWorld or beside it)
                 │ looked up at RESOLVE time (not parse time)
                 ▼
   renderers / layout read concrete values through one accessor set
        (textStyleOf(styleId) → ResolvedTextStyle, spaceOf(token)…)
```

Three moves, in order of importance:

1. **Styles become theme-owned data** (a theme file), not code
   if-chains. The theme file is the single place a designer edits.
2. **Scenes reference tokens; the reference is kept, not baked.**
   Resolution happens when a concrete value is needed (style lookup
   at paint/derive time, token lookup at component-deserialise time
   *into a token-carrying field* — see §5 for the two-tier rule).
3. **One resolver.** text_style.rae becomes the only styleId →
   concrete mapping; render_gpu2d's duplicate tables and render.rae's
   third copy are deleted.

## 3. Theme-owned TextStyle (design question 1)

A `TextStyle` is a named bundle:

| field | type | notes |
|---|---|---|
| `font` | font id (string) | `"body"` / `"icons"` / future `"mono"` — indirection over the app's loaded font set, NOT a file path |
| `size` | Float (du) | |
| `weight` | enum or Float | today we fake bold by double-draw (debug panel); a weight slot lets the style own that trick, or a real variable-font axis later |
| `lineHeight` | Float multiplier | for future multi-line text |
| `tracking` | Float (du) | letter-spacing; 0 default |
| `color` | **palette slot name** | stored as the slot (`"accentText"`), resolved against the active palette at use — this is what makes theme switching not require scene re-parse |
| `shadow` | shadow token name or inline (none default) | see §5 shadows |
| `align` | enum start/center/end | replaces `textStyleIsCentered` / `IsRightAligned` |
| future | any | new fields default; old theme files stay valid |

**Extensibility rule:** every field has a defined default, and a style
JSON object may omit any of them. New fields therefore never break
existing theme files — the same forward-compat convention the scene
loader already uses for components.

**Storage constraints (learned the hard way in this codebase):** no
`StringMap` in structs on the Live target, `List(struct)`-by-index is
hazardous in compiled code. The resolved theme therefore stores styles
the same way `FrameSet` (ui/frames) stores frames: parallel primitive
lists (`names: List(String)`, `sizes: List(Float)`, `colorSlots:
List(String)`, …) with linear name lookup — tens of styles, so O(n)
lookup is irrelevant, and the result should be cached per style id by
consumers that resolve per frame (the VisualBounds pattern, #219).

**Authoring format:** JSON, same flat-pool parser as `.raescene`.
Either a standalone `theme.raetheme` per app, or a `"theme"` section
allowed in a designated scene file. Recommendation: standalone file —
themes are app-wide, scenes are per-page, and hot-reloading the theme
file (the existing data-watch machinery) gives live design iteration
across every screen at once.

```json
{
  "type": "Theme",
  "version": 1,
  "palette": { "surface": {"r":15,"g":15,"b":18,"a":255}, "…": "…" },
  "text": {
    "body":      { "font": "body", "size": 43.4, "color": "textPrimary" },
    "miniTitle": { "extends": "body", "size": 39.0,
                   "color": "accentText", "shadow": "textOnAccent" },
    "miniMuted": { "extends": "miniTitle", "size": 30.4,
                   "color": "accentTextMuted", "shadow": "textOnAccentSoft" }
  },
  "shadow": {
    "textOnAccent":     { "color": {"r":0,"g":0,"b":0,"a":115},
                          "offset": "spaceXS", "softness": 8.7 },
    "textOnAccentSoft": { "extends": "textOnAccent",
                          "color": {"r":0,"g":0,"b":0,"a":100}, "softness": 6.5 }
  },
  "space":  { "spaceXS": 4, "spaceS": 8, "spaceM": 12, "spaceL": 16, "spaceXL": 24 },
  "radius": { "radiusChip": 8, "radiusCard": 16, "radiusPill": 999 }
}
```

(The exact numbers above are the current hardcoded ones, expressed as
data. Note how the TextShadow commit's two inline blocks became two
named shadow tokens, one derived from the other.)

## 4. Style inheritance (design question 2)

**Yes — single-parent `extends`, flattened at theme-load time.**

This is the prefab/prototype model the prompt asks about, with a
deliberate restriction: inheritance is a *load-time authoring
convenience*, not a runtime structure. The loader resolves each
style's chain once (parent first, child fields override) and stores
only flattened styles in the resolved tables. Runtime never walks a
chain.

Advantages:

- **Only differences are stored** — `miniMuted` is three lines, and
  reads as a statement of intent: "like miniTitle, but smaller,
  softer, muted."
- **Consistency by construction.** Change `body.font` and every
  derived style follows. This is exactly the centralized control the
  problem statement wants.
- **Small, reviewable diffs.** A design tweak is a one-line theme-file
  change, not a 15-scene sweep.
- **It matches how designers already think** (Figma text styles,
  CSS custom properties + composition, UIKit/Compose typography
  scales all converge on this shape).

Disadvantages, and how the design blunts them:

- *Hidden coupling / "where did this value come from?"* — every
  effective value is the end of a chain you can't see at the use
  site. Mitigations: single inheritance only (no diamond problems);
  a **max chain depth (4) enforced at load**; a debug dump
  (`themeDumpResolved()`) that prints every style fully flattened, so
  the answer to "what is miniMuted, actually?" is one command away.
- *Refactor hazards* — editing a base style changes descendants you
  forgot about. Mitigation: few styles (8–20) and the dump make the
  blast radius inspectable; this is also precisely the *point* of a
  theme (you usually WANT descendants to follow).
- *Cycles / missing parents* — load-time validation: unknown
  `extends` target or a cycle is a loud parse error, same policy as
  scene-loader diagnostics.
- *Temptation to build deep taxonomies* — the depth cap plus the
  style budget ("if you have 40 text styles you don't have a theme,
  you have per-widget styling with extra steps") keep it honest.

Flatten-at-load also sidesteps the Live-target constraint set: no
recursive structures, no runtime graph walking, resolved tables stay
parallel primitive lists.

## 5. Tokens beyond text (design question 3)

One mental model everywhere: **a token is a named value in a
category; a style is a named bundle of values.** Categories proposed,
in adoption order:

1. **space** — exists. Keep the five-token budget and the existing
   discipline comments; the only change is the values move from
   module `let`s into the theme file (with the module keeping
   fallback defaults so lib/ui works themeless).
2. **radius** — `radiusChip / radiusCard / radiusPill` (+ maybe
   `radiusThumb`). 106 currently hand-writes `69.39759`,
   `26.024096`, `47.71` across scenes; three tokens cover almost all
   of them. Scene support is the existing `optFloatOrToken` pattern
   extended to `CornerRadius.radius` and `Shape.radius`, resolving
   against a second category.
3. **shadow** — named presets (`textOnAccent`, `cardFloat`, …) with
   fields color / offset / softness. Referenced BY text styles (§3)
   and by container styles; per-entity `TextShadow` demotes to an
   override (§6).
4. **padding** — preset Insets (`cardPadding`, `screenEdgePadding`).
   Worth having because Insets are 4 numbers that always travel
   together; `"Padding": "cardPadding"` reads better than four
   spacing tokens.
5. **icon styles** — size + tint-slot pairs (`iconNav`,
   `iconInline`), replacing the current convention of a Rect sized in
   raw du + a `tint` color name on every icon sprite.
6. **button / container styles** — *composite* styles: a named bundle
   that references other tokens (background slot, radius token,
   padding token, text style, icon style). `"containerStyle":
   "primaryButton"` is the long-term replacement for a scene node
   carrying five visual components. This tier is the natural bridge
   into the widget model (`StateStyle` from #220 already gropes in
   this direction — its per-widget copies of textStyle/shapeFill
   variants should eventually reference theme styles per widget
   *state* rather than carry raw values).

Categories are namespaced by the field they appear in (a radius field
looks up radius tokens), not by a global token pool — so `spaceM` and
a hypothetical `radiusM` can't collide, and error messages can say
which table was searched.

## 6. ECS interaction — where resolution happens (design question 4)

Two tiers, chosen by what kind of value it is:

**Tier 1 — geometry consumed by layout (space, radius, padding):**
resolve at scene-deserialise time into the existing concrete
components (`Layout.gap: Float`, `Padding.insets`, …), exactly as
`optFloatOrToken` does today. Rationale: layout runs from plain
floats in a hot pass; these values don't change on theme *mode*
switch (dark and light share geometry); and re-resolving them on
theme-file hot-reload already has a path (data-watch → scene re-parse
→ world rebuild). No new components needed.

**Tier 2 — appearance consumed by paint (text styles, colors,
shadows, icon styles):** keep the *reference* in the component
(`Text.styleId` — unchanged), resolve through **one accessor module**
at use time:

```
textStyleOf(theme: view ResolvedTheme, styleId: view String)
    ret view ResolvedTextStyle
```

with the flattened style tables living beside `UiWorld` (in the same
place the fonts already travel — `Gpu2dUi` grows a `theme` handle, or
a `ResolvedTheme` rides in UiWorld like `MsdfState` does).

Why an accessor and NOT per-entity computed/render components (the
alternative the question raises): a `ComputedTextStyle` component per
text entity would copy ~10 fields onto hundreds of entities that all
share 8–20 styles — the exact denormalisation this design is removing
from scenes, reintroduced at the ECS layer. Per-entity derived
components are the right tool when the derived value is genuinely
per-entity (`VisualBounds` from #219: depends on the entity's own
string). Style resolution is per-*style*, so it belongs in a shared
table with a cheap lookup. The palette-slot → RGBA step happens
inside the accessor, so **runtime theme switching becomes: swap
ResolvedTheme, bump one generation, repaint** — no scene re-parse, no
world rebuild (fixing the parse-time-baking weakness of the current
color tokens; scene-baked colors migrate to this same path over
time).

Caching: consumers that resolve per frame keep the #219 pattern —
change-gate on (styleId, theme generation). The theme generation
counter is the invalidation spine for everything appearance-related.

## 7. Overrides (design question 5)

Order of preference, most to least encouraged:

1. **Derive a style in the theme** (`"extends"` + diffs). If a widget
   needs "miniTitle but 52 du", that is a *new named style*
   (`heroTitle`), visible in the theme file, reusable, themeable.
   This should handle nearly every real case — and it keeps the
   design-system inventory honest.
2. **Partial override object in the scene**, explicit and namespaced:

   ```json
   "Text": { "text": "9:41", "styleId": "caption",
             "styleOverride": { "tracking": 2.0 } }
   ```

   Only style fields, applied over the resolved style, stored as a
   sparse per-entity component ONLY when present (no cost to the 99%
   of nodes without one). The key is that the base style id remains —
   an override never severs the link to the theme, it annotates it.
3. **Raw per-entity visual components** (`TextShadow`, explicit
   color) — the escape hatch of last resort, for one-off art
   direction where no style relationship exists. Kept working, but
   the scene loader gains a **lint line** when it sees one
   (`[scene] MiniTitle authors raw TextShadow — consider a shadow
   token`), the same tone as the existing unknown-component warning.

Why overrides must feel like exceptions: every override is invisible
to the theme file — the one place we're promising designers they can
see and change everything. A partial override keeps most of the link
(base style still applies, diffs are declared); a raw component
severs it completely. The friction gradient (theme edit = easy,
override block = explicit ceremony, raw component = warned) is the
mechanism that makes the paved road the default without forbidding
the dirt road.

Anti-pattern guard, restated as a review rule: **if the same override
appears twice, it was a style all along.** The TextShadow commit
failed exactly this test on its second entity.

## 8. Where raw numbers remain correct (design question 6)

Tokens are for *design language*; numbers are for *domain geometry*.
Keep raw values where the number IS the content:

- Artwork/board dimensions (`1080×1600` GameBoard, cover tile sizes
  derived from column math), phone frame + device preset dimensions,
  aspect ratios.
- Positions inside a fixed-design subtree (`ScaleToFit` boards,
  absolute-layout nodes) — authored coordinates, not rhythm.
- Computed layout values (responsive column widths, scroll floors) —
  these are outputs of math, not inputs of style.
- One-off physical tuning that matches no scale step, with the
  existing spacing.rae convention: keep the literal, comment the
  intent, don't mint a token for one site.

Prefer (eventually require, via lint) tokens for: gaps, paddings,
margins, corner radii, font sizes/styles, colors, shadows, icon
sizes — anything where two occurrences should stay equal *because
they mean the same thing*. The test: "if design changes this, should
every use change together?" Yes → token. No → number.

Module-level `let` constants in Rae must stay literal (C-backend
constraint), so code keeps the existing `# spaceM`-comment convention
at those sites; call-site code uses tokens directly.

## 9. Migration strategy (design question 7)

Non-breaking, value-preserving, in slices — each lands green with
byte-identical (or intentionally-identical) 106 screenshots:

- **M0 — Theme file + loader.** Add `ResolvedTheme` (parallel-list
  tables), the `.raetheme` parser with `extends` flattening +
  validation, and a default theme file whose values are transcribed
  from today's hardcoded tables. Nothing consumes it yet. Ship with
  a dump tool.
- **M1 — Single text resolver.** text_style.rae reads ResolvedTheme
  (falling back to its current if-chains when no theme is loaded, so
  lib/ui stays usable standalone); render_gpu2d's duplicated
  `g2dTextStyle*` tables are deleted in favour of it; render.rae's
  raylib copy follows. Screenshot-verified no-change.
- **M2 — Shadows into styles.** Add the shadow token table; give
  `miniTitle` / `miniMuted` their shadows in the theme file; delete
  the TextShadow blocks from mini-player.raescene. The TextShadow
  component remains as tier-3 override + gains the lint. This is the
  direct repair of e39a378 and the template for every future "should
  this be a component?" conversation.
- **M3 — Radius + padding tokens.** Extend `optFloatOrToken` to
  category-aware resolution; add radius/padding tables; sweep 106
  scenes' repeated radii/paddings onto tokens (pure data change,
  values identical).
- **M4 — Spacing sweep in 106.** Replace raw gaps/paddings with the
  existing space tokens (98 is the reference). Data-only; screenshots
  identical by construction since tokens carry the same values.
- **M5 — Live theme switching.** Move scene color-token resolution
  from parse time to the tier-2 accessor path (store the slot name in
  components that today store baked RGBA), making dark/light and
  theme-file hot-reload a repaint instead of a rebuild. This is the
  largest slice and intentionally last — everything before it is
  value-neutral plumbing.
- **M6 (with the widget model, #214/#215) — composite styles.**
  `StateStyle` variants reference named styles/tokens instead of
  carrying raw values; `primaryButton`-class container styles arrive
  here, once there is a widget system to consume them.

Rollback safety: every slice keeps the previous mechanism as
fallback until the following slice removes it, and 106 headless
screenshots gate each step (the #213/#219 methodology).

## 10. Summary of recommendations

1. Theme file (JSON, hot-reloadable) owns palette + text styles +
   space/radius/shadow/padding/icon tokens; parsed into flattened
   parallel-list tables at load.
2. Single-parent `extends` inheritance, flattened at load, depth-
   capped, with a resolved-dump debug tool — prototypes as an
   authoring convenience, never a runtime structure.
3. Scenes keep referencing ids/tokens (`styleId`, `"spaceM"`,
   `"radiusCard"`); geometry tokens resolve at deserialise time,
   appearance styles resolve through one accessor at paint time,
   invalidated by a theme generation counter → live theme switching.
4. Overrides exist but are graded: derived style ≫ explicit partial
   `styleOverride` ≫ raw component (lint-warned). Repeated override
   = missing style.
5. Raw numbers stay for domain geometry and computed layout; tokens
   are mandatory where equality-of-meaning matters.
6. Migrate in six value-preserving slices, deleting the duplicated
   style tables early and moving color resolution off parse-time
   baking last.

End of design.
