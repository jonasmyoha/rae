# Clo memory

## Durable Rae design principles (carry across tasks)
- **Value/ref boundary:** `mod`/`view`/`own`/`=>` make refs; else by value.
- **Two backends:** Compiled (C) is the target; Live VM deprecated/frozen.
- **Ownership model IS a borrow checker** (view/mod/own/copy + cascade-drop,
  no GC). `List(Struct)` doesn't reliably deep-copy inner heap fields → keep
  components primitive/Vec3-only.
- **COORDINATE MANDATE:** `docs/coordinate-system.md` — right-handed **Z-up**
  Blender (+X right, +Y forward, +Z up), yaw about +Z. "New 3D code MUST use
  Z-up." Only the projection touches clip space, so WebGPU's Y-up preference
  never justifies Y-up world space. I got this WRONG in review; verify docs
  before asserting conventions.
- **Namespacing:** a lib calling another package needs `open pkg`, not
  `import pkg` (bare calls otherwise error).
- Paren-precedence codegen bug fixed (test 548). Parser still SEGFAULTS on
  `view` as a let-binding name (#288, open).

## 3D renderer unification — IMPLEMENTED as lead (2026-07-31)
Scores: 107=6.6 (GPT-5.5 raymarch), 108=7.35 (GPT-5.6-Sol raymarch, best
data model), 109=7.9 (mine, raster PBR — only shippable architecture).
Both raymarchers are capped by a per-frame GPU→CPU→GPU pixel round-trip.

**Landed** (rae 81caf33/doc, outer 107a3a8):
- Z-up conversion of math3d (doc-exact lookAt basis + degenerate fallback,
  `orbitEye`, `mat4ScaleXYZ`) and mesh3d (sphere poles ±Z, plane XY/+Z,
  torus ring XY, CCW winding preserved).
- `lib/scene3d.rae` — backend-agnostic scene: Vec3 components, entity
  indirection (from 108), typed generation-carrying Mesh/MaterialHandle
  (from Chattie), and the KEY insight: geometry is a **tagged split**
  (MeshRenderer=raster vs SdfPrimitive=raymarch). Total unification is
  impossible only at geometry; everything above it is shared.
- `lib/gpu3d.rae` — typed seam: `beginScene(camera, light, aspect, time)` +
  `renderScene(scene)`. The 36-float block and draw(9 floats) are now
  internal encoding. C mesh handles are **1-based** (slot+1), so id 0 = none.
- 109 ported + verified headless (659 colours, correct Z-up layout).
- `rae/docs/unified-3d-renderer.md` — scores, architecture, disagreement
  resolutions, keep-list, all limitations mapped to queue items.
- QUEUE #306-#316 = every deferred review item (Mat4 value types to stop
  per-frame heap churn, inverse-transpose normals, HDR+tonemap pass, shrink
  C to raw WebGPU, raymarch off CPU round-trip, raymarch-on-scene3d, owned
  device context, extraction/culling, hybrid SDF depth, draw cap).

**Deliberately deferred:** Chattie's full `World3d`/ComponentTable +
extraction/culling/frame-graph is the right destination but would gate the
demo on one large refactor — parallel arrays give the same authoring shape
and grow into it without app-code changes (#314). Gem contributed no 3D
design in this run, so nothing of theirs was available to adopt.

**Demo-quality gotcha:** with the camera on -Y, a sun circling in XY sends
the GGX lobe away from the viewer and the material grid reads flat — bias
the sun toward the camera side (-Y) for highlight sweep.

## Prior task (concurrency design) — archived
spawn exists as a VM-only half-prototype (detached sub-VM, never joined, no
C-backend codegen). Proposed: spawn→Task(T) joinable, unmarked await,
scoped tasks borrow (join barrier) vs detached=own move, parallelFor for
ECS, Channel/Shared/Atomic. raylib main-thread-only.
