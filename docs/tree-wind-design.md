# Tree wind: stable crowns and moving leaves

Status: **ON again, without gusts** (2026-10-23). It was switched off on
2026-10-22 because it looked glitchy. Part of that was the gust field
(`windGustAt`): it moves by the drifting wind direction times the total
time, so it jumped between frames (docs/wind-design.md §5), and the grass
twitched the same way. windLeaf now uses a steady gust level (0.5), and the
rest of the motion (each patch turning about its pivot on the wind clock)
is unchanged. The switch is `windLeafScale` in lib/wind.wgsl: 1.0 on, 0.0
every leaf still.

The rigid-leaf implementation is in the shared renderer and generators.
The structural/branch hierarchy below remains a later refinement.

## Implemented first pass

- Six-sided leaves and broad scalloped patches preserve a local pivot for every
  vertex. MeshData keeps optional pivot data separately from positions, normals
  and UVs. Foliage uploads interleave one extra vec4 (16 bytes per vertex);
  ordinary meshes retain their 32-byte layout. No extra per-leaf draws or
  per-frame mesh uploads are introduced.
- The shader rotates each patch intact, with its shading normal, about its
  centre. In a steady breeze (strength 1) a patch turns about 6 degrees each
  way, in a passing gust about 15 degrees and leans a little downwind; the
  angle is bounded to 0.35 radians. It has no crown translation. (The first
  version turned under 1 degree between gusts and was bounded to 0.14
  radians: a leaf moved a few millimetres and the wind could not be seen,
  2026-10-22.)
- Every mesh baked into an impostor or drawn by another pipeline must be read
  with its own vertex size: the impostor bake (lib/ImpostorBake.rae) has a
  32-byte and a 48-byte pipeline and picks one per mesh with
  meshStoreHasFoliage. One 32-byte pipeline read the leaf meshes' 48-byte
  vertices as garbage, and every billboard came out as broken squares.
- Forest cypress and umbrella foliage have lower flexibility. Spruce skirts,
  wood and other meshes without leaf metadata stay rigid. This deliberately
  removes the old spatial wobble from those shapes; it is not branch simulation.
- Forest instance motion smoothly fades from full at 25 metres to zero at
  90 metres. Geometry and shadows share that instance factor. The grove's
  individually placed inspection trees keep full detail. Far impostors remain
  unchanged and still. A later version can expose the detail range in settings.
- Dedicated optional foliage pipelines serve deferred, forward, and shadow
  rendering. The previous full wind vector is retained for motion vectors.
  Walker 136/137/143 use the shared upload path; Autumn Grove now uses it too
  and advances an App-owned WindState.
- The GPU check in checks/leafWind tests 64 times for preserved patch distances,
  fixed pivots, consistent rotated normals, exact calm and actual movement.
  Fixture 1141 checks deterministic shared pivots and rigid spruce layers.

There is no C runtime change. The full branch hierarchy and projected-size
animation LOD proposed below are not part of this first pass. Current phase
comes from each patch's world pivot, so separately generated coarse patches
can have different phases during their existing LOD cross-fade.

Verification: full suite 895 passed; GPU leafWind check passed; visual gates
136, 137, 138 and 143 passed, including the walker's running-memory check,
all eight grove views, forward billboards, and 31,090 trees / 829 forest clusters.
The extra vertex cost has not been isolated in a before/after GPU benchmark;
these results do not claim a frame-time improvement.

## Source review before this implementation

`lib/wind.wgsl::windOffset` applies three continuous sine waves to every
foliage vertex. Their phases come from the vertex's world position, not from
leaf or branch identity. Different vertices of the SAME leaf patch therefore
move differently. Broad patches can stretch and shear instead of turning.
The names `cluster` and `leaf` in that shader are spatial frequencies, not
actual clusters or leaves.

The main periods are about 3.7 seconds, 0.9 seconds and 4.8 seconds. Wind gusts
multiply these oscillations; between gusts they still run at full base strength.
The resulting forest can look restless even in an ordinary breeze.

`app3d/GroveTrees.rae` assigns the same `groveLeafSway = 1.0` to leafy parts at
mesh LODs and zero to wood. Species do not have separate wind responses.
`procgen/FluffyFoliage.rae::fluffyPatch` knows each patch's centre while building
it, but `foliageVertex` uploads only position, normal and zero UVs. Attachment
points and patch identity are lost before the shader sees the mesh.

`gbuffer_static_surface.wgsl` moves positions but does not rotate normals with
that motion. It shares the displacement with forward rendering, and
`shadow_static.wgsl` also uses it. The previous-frame calculation uses previous
time but CURRENT wind direction/strength. Those details matter when replacing
translation with rotations and when checking temporal trails.

The far impostor shader (`sprite_billboard.wgsl`) does not call `windOffset`.
Do not assume all visible forest movement comes from this function: isolate
mesh levels and impostors when reproducing the complaint. The findings above
explain a likely cause, not a measured capture of the reported view.

## Recommended look

Keep the broad crown silhouette almost still in a normal breeze. Let small
attached leaf groups turn, briefly exposing lighter or darker surfaces. Let a
gust pass across several trees, with different response strength and a small
response delay per tree. Quiet intervals are important.

Use three scales, separately adjustable by species:

- Structural motion: a very small, slow downwind bend. A stiff mature tree may
  have almost none; a slender tree can have more. Wood and its attached foliage
  must share any structural transform.
- Branch motion: shallow rotations about branch attachments. Leaves on a
  branch inherit that rotation, preserving their arrangement.
- Leaf motion: small rotations of intact leaf patches about their attachments,
  strongest during gusts. Fine flutter fades out with projected size.

Starting art targets, to tune in motion rather than treat as physical units:
branch rotations around 0.5–2 degrees in an ordinary breeze, leaf rotations
around 2–6 degrees, nearly rigid trunks. Broadleaf, cypress, spruce and palm
need separate responses. Avoid turning a spruce's entire layered crown like
one loose leaf. Keep wind strength zero exactly still.

A shading-only normal variation can add distant leaf shimmer cheaply, but
should be subtle and optional. Large brightness pulses or scrolling leaf
textures would replace the wobble with another visible effect. Do not warp
whole far-canopy impostor cells: each contains several trees and baked depth.

## Shared implementation

1. Keep `WindState` and the existing travelling gust field as the shared wind
   input. Replace continuously strong detail motion with a low resting response
   plus gust-driven activity. Derive persistent variation from placement identity
   and generator IDs, never draw order, visible instance slot or vertex position.
2. Preserve attachment data in Rae generators. Add optional foliage animation
   data owned with the mesh: per vertex a patch/branch index; per patch a local
   pivot, axis, stiffness and stable phase; per branch its pivot and parent.
   Start with one branch level and a leaf level. Shared mesh data is uploaded
   once and reused by every instance. Do not turn leaves into ECS entities or
   add one draw call per branch.
3. Add a shared WGSL deformation function returning position AND normal.
   Rotate a leaf about its pivot, then inherit the branch transform. Rotate its
   authored shading normal by the same rotations. Crown-smoothed normals can
   remain crown-smoothed. Any connected wood bend needs attachment weights so
   joints stay connected. A generic mesh with no wind data stays rigid.
4. Put species wind settings beside the shared GroveTrees configuration, not in
   copied example shaders. Keep the general eight-float mesh layout for ordinary
   meshes; use optional GPU buffers for wind metadata rather than stealing UVs
   or overloading normals. Measure the extra buffer reads before extending this
   layout to every generated tree type.
5. Use the same deformation in deferred, forward and shadow passes. Retain the
   previous wind vector and time for motion vectors. Expand culling bounds by
   the maximum allowed displacement. Make mesh/LOD attachment IDs deterministic
   so a change of detail does not restart the wind phase.
6. Fade fine motion with projected size, before switching to existing static
   impostors. Use the main camera's animation detail for shadows too. For the
   first version, accept a quiet distant forest; distant animation is a separate
   measured refinement, not a reason to distort baked depth or silhouettes.

No new language feature or C runtime policy is needed. Rae owns settings,
generator metadata and GPU resources; WGSL performs per-vertex deformation.
CPU physics per leaf and per-frame mesh uploads are unnecessary for this look.

## Bounded implementation steps and acceptance

First, capture the same fixed forest camera with current wind and with
`RAE_WIND=0`, then isolate mesh LODs and impostors. Include a close broad patch,
a medium-distance group and a large forest. Keep exposure, camera and seed
fixed; compare several seconds of movement, not just one frame.

Then implement a leaf-pivot pilot on one broadleaf mesh, with its wood still
rigid. Check that distances between vertices of each patch remain unchanged,
its pivot stays attached, normals follow its rotation, and calm wind is exact.
Render the pilot in 138 and a dense group in 143. This is the first visual
approval point, before adding a branch hierarchy or changing all species.

Next add shared branch response and species tuning, then animation LOD and
previous-frame wind. Update 136, 137, 138 and 143 together through shared lib
code. Verify their narrow visual gates, shadow alignment, camera movement and
LOD transitions. Preserve canopy coverage tolerances with checks/canopyCoverage.

Measure GPU time at matched camera/seed/instance counts with wind off, old wind
and new wind. Report metadata bytes per mesh and draw counts. Expect additional
vertex buffer reads and rotations, but no per-frame CPU traversal of leaves or
extra per-leaf draws. Do not promise a frame-time improvement without measuring.

## Technique references

The useful established idea is to separate structural bending from leaf detail,
with per-leaf stiffness and phase data. See NVIDIA's
[vegetation animation chapter](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis).
For shallow joint hierarchies, procedural angular motion and animation detail
levels, see its [tree wind chapter](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees).
The intact-patch pilot above adapts those ideas to Rae's broad stylised meshes;
it is not a proposal to copy their code or introduce a physics solver.
