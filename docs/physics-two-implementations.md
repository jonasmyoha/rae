# Physics — two implementations, then a decision

**Status:** plan, 2026-10-05. Decided by the maintainer. This is the current
physics plan. It replaces the single-engine decisions in
`docs/physics-design.md` §2 ("port Box3D to Rae", 2026-09-30) and
`docs/physics-box3d-port-research.md` §9 ("use the C library instead",
2026-10-01).

## 1. The decision

Rae gets **two implementations of the same engine, Box3D**, built side by
side, each with the same two example programs. When both are finished, they
are measured against each other, and the maintainer decides which one Rae
uses.

| | track A: the Rae port | track B: the C library |
|---|---|---|
| what | Box3D ported to Rae, `lib/physics/` | upstream Box3D's own C, called through generated bindings, `lib/box3d/` |
| role | a performance and language test: how close safe Rae gets to C on a large numerical program, and what the compiler needs for it | the engine as its author ships it: upstream features and fixes by moving a pin, Box3D's own SIMD and threading |
| state (2026-10-05) | complete through P9a: every subsystem bit-exact against Box3D's C (oracle fixtures 948-996), the ECS layer, single-threaded at 1.02-1.25x Box3D's scalar C build | not started |
| next | P9b, the parallel step | build integration and bindings, then the ECS layer |

Neither track is "the real one" until §6 decides. Both are maintained to the
same standard until then: fixtures, examples on the visual gate, memory and
leak checks.

### How we got here

The port was designed on 2026-09-30. On 2026-10-01 the research
(`docs/physics-box3d-port-research.md`) recommended the C library instead,
and the maintainer agreed. That recommendation was never applied: on
2026-10-02 the port phases were moved into `QUEUE.md` unchanged, a review
of those tasks kept them, and the queue ran the port to completion. Since
the port now exists and performs well, the maintainer chose (2026-10-05) to
finish both and decide on measurements instead of discarding either.

## 2. One app-facing surface, two backends

Both tracks expose the **same ECS surface**: the resource, components,
systems, spawn/despawn helpers, events and queries of `docs/physics-design.md`
§5. Type, function and parameter names and the call order are the same; only
the package differs.

- Track A: `physics/ecs/...` (exists; it is the reference surface).
- Track B: `box3d/ecs/...`, written to match it. Where Box3D's C API cannot
  give the same shape (for example a C handle where the port has a value),
  the C layer adapts, and the difference is listed in its module comment.

The ECS fixtures of track A (987, 988 and the rest of §7 in
`docs/physics-design.md`) are run against track B too, with the same expected
output. At the same Box3D commit, built scalar with `-ffp-contract=off`, the
two must agree bit for bit, since the port is bit-exact against that build.
Track B's product build uses Box3D's SIMD path; whether its results still
match the scalar ones is measured and recorded, not assumed.

## 3. The examples: two programs, each built on both tracks

| program | track A | track B |
|---|---|---|
| physics playground | `examples/122_physics_playground_port` | `examples/123_physics_playground_c` |
| vehicle | `examples/124_vehicle_port` | `examples/125_vehicle_c` |

- **The scenes are written once.** Scene descriptions (bodies, shapes,
  joints, the vehicle's chassis and wheels, the thrown ball) and the HUD are
  backend-neutral Rae in one shared package. Each example is a thin entry
  that builds those descriptions through its own track's ECS layer and feeds
  the HUD from its stats. The first example task picks where the shared
  package lives (a path dependency of both examples, or `lib/` if the
  benchmarks use it too) and records it here.
- **The same gates for all four:**
  - the visual example gate, with a headless screenshot;
  - a fixed-step determinism print: a hash of every body transform after N
    steps;
  - memory flat over a 5-minute memory test while bodies spawn and despawn;
  - `leaks --atExit` clean.

  At the same pin, built scalar, the two variants of a program print the same
  hash.
- **Featured:** both playgrounds are featured during the evaluation, side by
  side, with a README row and a screenshot each. After §6, the chosen track's
  playground stays featured.
- The playground's content is the existing brief: a large box pyramid (1k to
  10k bodies), dominoes, a hinged plank bridge and a chain, a joint-held
  stack, mesh and height-field ground, the character mover, a thrown ball, a
  performance HUD and instanced rendering. The vehicle is Box3D's Driving
  sample: a chassis and four wheel joints, steering, spin motors and a
  keep-upright torque. It also has a non-visual fixture: a driven car reaches a
  target speed, and a steered car turns.

## 4. Track B: building the C library

- **Source:** upstream Box3D, fetched at a pinned commit into a cache outside
  the repository, as `tools/box3d-oracle/` and wgpu-native already work. No
  Box3D source is committed. The pin starts at `9f998c8`, the port's commit,
  so the two tracks are comparable. §6 also measures moving it to upstream
  HEAD.
- **Build:** compiled with `-ffp-contract=off`, the cross-platform
  determinism flag, for every target Rae builds: native, and WASM through the
  existing WASM build. Released with SIMD on; scalar for the oracle-equality
  fixtures.
- **Bindings:** `rae bindgen` over `include/box3d/*.h`, checked in under
  `lib/box3d/` with a regeneration script, like `lib/webgpu`. The binding
  generator's gaps (static inline math helpers, structs by value, callbacks)
  are fixed in the generator or covered by a small hand-written module, with
  each case listed.
- **Linking:** Rae has no general "this library needs that C library" step
  yet. The first track-B task uses the existing mechanism if one fits. If it
  needs new `.raepack` surface, it writes the design and stops with a question
  for the maintainer.
- **Threading:** Box3D's own scheduler through `workerCount`, or later
  `enqueueTask` / `finishTask` over Rae's worker pool, whichever the step
  measurements favour.

## 5. Task order

The order in `QUEUE.md`, top to bottom:

1. **A: P9b, the parallel step.** It is already specified, independent of
   track B, and needed before any fair multi-worker comparison.
2. **B1: the C library build and its bindings**, plus a fixture that steps a
   world and prints poses equal to the oracle's.
3. **B2: the ECS layer `lib/box3d/ecs`**, matching track A's surface, with
   track A's ECS fixtures passing against it.
4. **The shared scene package and `122_physics_playground_port`.**
5. **`123_physics_playground_c`**, on the same scene package.
6. **`124_vehicle_port`**, plus the vehicle fixture.
7. **`125_vehicle_c`**, plus the same fixture on track B.
8. **Performance pass on both playgrounds:**
   - profile, frame budget and fixes;
   - scaling curves at 1-8 workers;
   - 120 Hz;
   - WASM.
9. **The comparison and the decision (§6)**, which ends with a question for
   the maintainer.

The examples come after both ECS layers on purpose. Writing the shared scene
package while only one backend exists would bake that backend's shape into
it.

## 6. The comparison

Measured on the four examples and `benchmarks/physics` (which gains a track-B
column), written up in this document:

- **speed:** step ms per scene at 1, 2, 4 and 8 workers, frame time in the
  playground at stress scale, and the WASM build in the browser;
- **behaviour:** the determinism hashes (identical across worker counts and
  across platforms where measurable), and equality between the tracks at the
  same pin;
- **cost to own:**
  - lines of Rae and C each track adds;
  - build time and binary size;
  - what moving the pin to upstream HEAD costs: for track B, measured by
    doing it; for track A, by the size of the upstream diff it would have to
    port;
- **what each makes possible:** callbacks (custom filtering, friction
  mixing), solver changes of our own, debugging in one language, and the
  compiler and language work the port drives.

The task ends with `[question]`: the maintainer picks the track. The other
one is then retired, kept as a benchmark only, or kept as a second backend,
and the docs and examples follow.
