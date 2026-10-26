# One tree that matches one reference tree

Status: plan, not started (2026-10-26). Decided: **a generator in Rae**,
written new, not based on the existing tree code (see "Built new").

## Why this plan

The old and the new tree rendering both miss the reference look. Many rounds
changed the leaf texture (plates, textured patches, sprays, camera-facing
clusters) on the existing tree shapes. That was the wrong layer. The
references get their look from four things, and our trees miss all four:

1. **Shape**: long thin trunks, small crowns that start high.
2. **Crown structure**: separate leaf clumps on visible branches, with sky
   between them.
3. **Clump shading**: each clump lit on top and dark below, with a light
   edge, so it reads as a volume.
4. **The scene around it**: haze with distance, warm sun and cool shade, a
   colour grade.

This plan builds **one tree that matches one reference tree well**, in a
separate test scene, and is judged against that reference at every step. It
is not about one species of ours (birch, cypress), and T02 is not a birch
(see "What kind of tree it is"). The shared tree code does
not change until the last step.

## The target: crop T02

`T02` in the private reference study, which lives in the separate, private
repository `own_repos/looks-development` (with Git LFS):
`looks-development/references/crops/T02.png` (source S04,
`hienoja_kasveja_ja_kivia4.jpg`, rectangle x 520, y 320, 330 x 570). The
reference images are private and are not copied into this repository.

What the tree in T02 has (the checklist for every step):

| # | Feature | In T02 |
|---|---|---|
| 1 | Trunk | One warm orange-brown trunk, about 1/26 of the tree's height thick at the base, thinning to 61% of that at the fork; straight to 40% of the height, then bowing gently to the left (the trace of T02: looks-development `treeReference/traces/T02.js`, the target since step 3 round 12; since round 13 the trace IS the tree's centre lines, lib/treeGen/TracedT02.rae, made by examples/146_treeLab/traceTree.py, and compare.sh's overlay-near.png shows them over T02 and the render) |
| 2 | Fork | At about 79% of the height (the trace) the trunk splits into two unequal leaders, both leaning right: the main one about 9 degrees, the thinner second one about 28 |
| 3 | Branches | The 4 short, slim side branches the trace shows, all in the top third: low right off the trunk (66%, 40 degrees), low left (70%, 38 degrees), high left off the main leader (81%, 48 degrees), a top one up left (89%, 20 degrees); nearly even in thickness to the tip, the low ones flattening a little toward it |
| 4 | Crown start | The lowest clump at about 50% of the height; nothing below it but trunk |
| 5 | Clumps | 6 to 10 separate clumps of different sizes at branch ends, the top one the largest; sky visible between them (about 20 to 30% of the crown area) |
| 6 | Clump shading | Lit top in yellow-green, dark blue-green underside, a clear light-to-dark range inside each clump |
| 7 | Clump edge | Ragged small-leaf edge, with single leaf tips sticking out; no smooth outline |
| 8 | Trunk shading | A broad light-to-dark change across the trunk, faint vertical streaks; no detailed bark |
| 9 | Setting | Seen slightly from below against a pale blue sky with clouds; darker, hazier trees behind; low bushes and flowers in front |

### What kind of tree it is

T02 is **not a birch**, and the work is **not based on our birch** (or any
other species of ours). What species it is cannot be told for sure: it
could be a pine, or a brown-trunked tree with leaves. The
decision (2026-10-26): **treat it as a brown-trunked broadleaf tree with
small leaves** (leaves, not needles), and **make it look like T02 looks**,
not like a named species from a field guide. So:

- the foliage is small LEAVES in clumps, not needles or needle tufts;
- the trunk is warm orange-brown (checklist 1 and 8), not the white bark of a
  birch, and not a pine's plated bark;
- no botanical source decides a feature that T02 shows otherwise; the crop
  is the reference, the checklist is the score;
- the generator's names say what the tree looks like (for example
  `warmBroadleaf`), not a species it is not.

## How every step is judged

- **One picture per step**: the T02 crop and our render side by side, at the
  same size, from a fixed camera that frames our tree like T02 frames its
  tree. A script makes this picture (`tools/treeLab/compare.sh` or similar).
- **The checklist above is the score.** For each of the 9 features: matches,
  partly, or does not match. It is scored in review; an agent may give its
  own score but marks it as its own.
- **An agent never marks a visual step done on its own.** Tests passing only
  shows the code works. A step ends with the comparison picture and waits for
  a yes or no. A "no" with a reason starts the next round of
  the same step.
- **The report and its history** live in the looks-development repository
  (`own_repos/looks-development`, private, Git LFS for images and videos).
  Every round of every step is kept, never overwritten, in
  `treeReference/rounds/YYYY-MM-DD-stepN-short-name/`:
  `TREELAB_ITERATION=<that name> examples/146_treeLab/compare.sh <camera>`
  for each of `near`, `mid` and `bark`. The round is then ONE entry at the
  end of `treeReference/rounds.js` (what changed, the pictures, and later
  the response); `treeReference/progress.html` shows
  every round oldest first, the same cameras in the same places, and
  compares any two. Videos of motion go into the round's folder too. Then
  commit the looks-development repository (its own git identity; its README
  says the same). The older report pages are kept as they were.
- **Two simple measurements** help but do not decide: the share of sky
  pixels inside the crown's outline (feature 5), and the light-to-dark range
  (10th to 90th percentile of brightness) inside the crown, compared with the
  same numbers measured on T02 (feature 6).

## Steps

### Step 1 — The tree lab scene

A new example, `examples/NNN_treeLab` (the next free number). Nothing in
`lib/` changes.

- A small patch of ground with grass, a sky with clouds, a fixed sun.
- A fixed camera: slightly from below, framing a 12 to 15 m tall tree like
  T02 does. A second camera at about 40 m and a third at about 120 m for
  later steps.
- A placeholder tree (a cylinder and spheres) so the comparison picture
  works from the start.
- The comparison script that puts T02 and the render side by side.

Done when: the comparison picture is made by one command, and the camera
framing matches T02 (agreed in review).

### Step 2 — The scene look in the lab

The background must match first, or the tree is judged against the wrong
light. Only in the lab:

- haze that grows with distance (far trees turn paler and bluer);
- a warm sun and cooler shadows and sky light;
- a tone curve and a colour grade toward T02's colours;
- a few darker, hazier placeholder trees behind.

Done when: with the placeholder tree, the sky, light and background in the
comparison picture read like T02's (features 9; agreed in review).

### Step 3 — Trunk and branches

The tree's skeleton, without leaves:

- a trunk from a curve (slight S-curve, slight lean), tapering, with the fork
  below the crown into two unequal leaders (features 1, 2);
- 4 to 6 side branches at 35 to 60 degrees, thinning to tips (feature 3);
- the trunk material: warm orange-brown, a broad light-to-dark change across
  it, faint vertical streaks (feature 8);
- one seed gives one tree; the numbers above are the parameters.

Done when: the bare tree beside T02 has the same trunk, fork and branch
layout as the branches visible in T02 (features 1, 2, 3, 8).

### Step 4 — Where the clumps are (mass layout)

Clumps placed at the branch ends, first as plain grey spheres or ellipsoids,
so only their layout is judged:

- 6 to 10 clumps, of different sizes, the top one the largest;
- the lowest at about 50% of the height (feature 4);
- gaps between them so sky shows through (feature 5);
- slightly flattened undersides.

Done when: the silhouette (tree in black, sky in white) is close to T02's
silhouette, and the sky share inside the crown is in T02's range (features 4,
5).

### Step 5 — The clump surface

Each sphere is replaced by a leaf clump:

- a few leaf-cluster cards per clump, with an alpha texture of small leaves
  (one texture made for this tree, painted or generated once);
- the cards' normals taken from the clump's sphere, so light falls smoothly
  across the whole clump and not card by card;
- darker the deeper a point is inside its clump and inside the tree
  (ambient occlusion stored per vertex);
- a little light through the leaves when the sun is behind them;
- two or three close greens per tree, varied a little per clump.

Done when: close up and at T02's framing, the clumps have T02's lit top, dark
underside and ragged edge (features 6, 7), and the measured light-to-dark
range is close to T02's.

### Step 6 — Variations

Three seeds give three trees that are clearly different but clearly the same
kind (different fork heights, branch counts, clump sizes, lean). Shown in a
row, like the background trees in T02.

Done when: the review shows three trees of one kind, none a copy of
another.

### Step 7 — At a distance

The tree at 40 m and 120 m, then as a billboard. The impostor bake
(`lib/ImpostorBake.rae`) bakes the new tree like the old ones. A cheaper
version for the middle distance keeps the clump layout and drops leaf cards.

Done when: at each distance the tree keeps its silhouette, its gaps and its
light-to-dark range, and the change between versions is not noticeable in a
short camera move.

### Step 8 — Cost

Measured in the lab and in a forest of a few thousand copies: triangles per
tree at each distance, frame time on this machine and on the phone. Targets
are set from the walker forest's current budget.

Done when: the numbers are written down and within the budget, or the
trade-off is accepted.

### Step 9 — Into the shared forests

Only after steps 1 to 8 are accepted:

- the tree becomes a species in `lib/app3d/GroveTrees.rae`, next to the old
  ones, with a switch to show the old ones;
- the lab's scene look (step 2) is proposed for the walker examples as its
  own task, because it changes every example;
- examples 136, 137, 138 and 143 get it, each with a before and after picture.

Done when: the before and after pictures of each example are accepted.

### Later, not part of this plan

- Wind: only after the still tree is accepted. Earlier versions looked
  glitchy, so it starts with whole clumps swaying slowly on their branches,
  judged in a video.
- Other reference trees (T01's tall tufted tree, T04/T05's autumn trees) are
  done the same way, one at a time, each against its own crop.

## Decided: a generator in Rae, built new

The tree is made by a **procedural generator written in Rae** (decision
2026-10-26). No Blender, no hand-made model, no imported tree mesh.
One seed and a set of parameters give one tree; the T02 checklist gives the
parameters' ranges.

### Built new, not on the old or new tree rendering

Both the old trees and the new ones have large problems, so the generator
does not start from them. It lives in its own package, `lib/treeGen/`
(PascalCase files, e.g. `lib/treeGen/Skeleton.rae`, `Clumps.rae`,
`Bark.rae`, `TreeMesh.rae`), and it does NOT import, copy or extend:

- the tree species and their helpers in `lib/procgen/`: `Tree`, `Foliage`,
  `FoliageCards`, `FluffyFoliage`, `CrownScaffold`, `CurvedFork`,
  `CurvedForkBranch`, `SculpturalFork`, `ForkSurface`, `Birch`, `Camphor`,
  `CamphorGeometry`, `Cypress`, `Ginkgo`, `Lime`, `JapaneseMaple`,
  `RedMaple`, `SugarMaple`, `Spruce`, `SpruceControl`, `Palm`;
- the forest's tree setup in `lib/app3d/GroveTrees.rae`;
- the foliage mesh, pipeline and shadow path (`lib/FoliageMesh.rae`,
  `lib/FoliagePipeline.rae`, `lib/FoliageShadow.rae`) and the leaf wind in
  `lib/wind.wgsl`.

If a piece of those files turns out to be exactly what is needed, it is
copied into `lib/treeGen/` only with approval, and the
step says why.

What it MAY use is the general engine, which is not tree code: math
(`Vec3`, `Quat`, `Math3d`, `Noise`), `Mesh3d`/`MeshStore` and the renderer
(the G-buffer and forward passes, shadows, materials and textures), the
image and PNG code, the impostor bake (`lib/ImpostorBake.rae`) for step 7,
and the ECS. A new shader for the clumps (step 5) is new code in
`lib/treeGen/`, not a change to the foliage shaders.

The new tree enters the shared forests only in step 9, as a new species
next to the old ones.
