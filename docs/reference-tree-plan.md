# One tree that matches one reference tree

Status: plan, not started (2026-10-26).

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
is not about one species of ours (birch, cypress). The shared tree code does
not change until the last step.

## The target: crop T02

`T02` in the private reference study:
`peli_ideoita/environmentReferenceStudy/crops/T02.png` (source S04, `hienoja_kasveja_ja_kivia4.jpg`, rectangle
x 520, y 320, 330 x 570). The reference images are private and are not
copied into this repository.

What the tree in T02 has (the checklist for every step):

| # | Feature | In T02 |
|---|---|---|
| 1 | Trunk | One warm orange-brown trunk, thin (about 1/40 of the tree's height at the base), a slight S-curve, visible for about 55% of the height |
| 2 | Fork | Below the crown the trunk splits into two unequal leaders; the opening between them is part of the shape |
| 3 | Branches | 4 to 6 thin side branches at 35 to 60 degrees, visible against the sky and through the crown |
| 4 | Crown start | The lowest clump at about 50% of the height; nothing below it but trunk |
| 5 | Clumps | 6 to 10 separate clumps of different sizes at branch ends, the top one the largest; sky visible between them (about 20 to 30% of the crown area) |
| 6 | Clump shading | Lit top in yellow-green, dark blue-green underside, a clear light-to-dark range inside each clump |
| 7 | Clump edge | Ragged small-leaf edge, with single leaf tips sticking out; no smooth outline |
| 8 | Trunk shading | A broad light-to-dark change across the trunk, faint vertical streaks; no detailed bark |
| 9 | Setting | Seen slightly from below against a pale blue sky with clouds; darker, hazier trees behind; low bushes and flowers in front |

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

## Decision needed before step 3

**How the tree is made:**

- **(a) Procedural, in Rae:** a generator that grows the skeleton from
  parameters and places the clumps. It fits the existing pipeline (seeds,
  levels of detail, the impostor bake) and gives endless variations. It is
  the slower way to find the look.
- **(b) Made by hand in Blender, loaded as glTF:** faster to find the look,
  because a person (or an agent through the Blender connection) shapes it
  directly. It gives a few fixed trees, not endless variations, and leaf-card
  normals and per-vertex darkness must be set up in Blender.
- **(c) Both:** find the look by hand in Blender first (steps 3 to 5 as one
  model), then build the generator to reproduce that model, judged against
  both T02 and the hand-made tree.

The recommendation is (c): the look is the hard part, and it is found
fastest by hand; the generator then has a fixed target it can be checked
against.
