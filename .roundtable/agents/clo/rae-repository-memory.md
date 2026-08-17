# Clo memory

## Active investigation: mobile UI regression triple (2026-05-15)

Three regressions reported simultaneously after rae commits
`62e4c6b` (parser/runtime) and `b5b07e2` (mobile UI refactor):

1. **Pill bg missing** — `album_view.rae:170` `buildPill` is missing
   the `setShapeRounded(... fill, radius: h/2)` call between
   `setRectFixed` and `setLayout`. One-line restore. Trivial.
2. **Live VM exits with "cannot assign to a read-only 'view'
   reference"** — `<unknown>:0:0` source location. The string lives
   at `compiler/src/vm.c:448` inside `OP_SET_LOCAL`. Runtime error,
   not parse-time. Fires after texture loads → inside build-world
   loop. Likely root cause: parser change in 62e4c6b
   (`expr_to_type_ref` + nested generic-arg conversion + `pub` on
   types) interacting with the `sizeof(StringMapEntry(V))()` fix in
   `lib/core.rae` such that StringMap.set's `entry.value = value`
   assignment now sees `entry` as a view-ref. Live VM emitter
   wasn't re-tested after parser changes — that's the gap.
3. **MGMT cover renders dark** — config.rae now defines
   `imageTintColor: RgbaColor = {255,255,255,255}` and
   album_view.rae's `buildAlbumHero` reads it across files. raylib
   multiplies texture by tint; near-zero alpha → black. Suspect:
   cross-file struct-global is hitting same view-ref / struct-copy
   issue as Bug 3.

## Decisions made this round

- Treat as one regression bundle, not three. Fix order: 2 → 3 → 1.
- Bug 2 lands immediately (1 line). Bug 3 needs bisect from
  `21c8802` (last known green Live state). Bug 1 diagnosis depends
  on Live working.
- Don't speculate further on Bug 1 until we can run Live and
  log `imageTintColor.a` from inside `buildAlbumHero`.

## Action items I proposed

- Add chunk-name + bytecode-offset to all
  `diag_error(NULL, 0, 0, ...)` callsites in `compiler/src/vm.c`.
  Cheap follow-up; would have made this report a one-step fix.
- Wire `make test --target live` into the snapshot script so the
  Live regression would have been caught at commit time, not by a
  user.

## Key context for next rounds

- Recent rae commits worth knowing: `62e4c6b` (parser+runtime),
  `b5b07e2` (mobile UI multi-file), `2f97850` (editor grammars,
  no exec code), `aa07f39` (docs, no exec code), `4601a24` (stats).
- Snapshot tool currently only exercises Compiled target.
- `rae/docs/ui-viewport-and-safe-area-plan.md` and
  `ui-rendering-tech-stack-comparison.md` exist — relevant if the
  fix discussion drifts toward renderer choices.
