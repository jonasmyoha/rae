# The stylized trees in the new technique

Status: tasks 1-2/12 done (2026-10-29), 3-12 queued; the queue's "stylized trees 1/12 .. 12/12".

The T02 tree (lib/treeGen, docs/reference-tree-plan.md) is the first tree
made the new way. The same technique is to be carried over to
the stylized set's other trees and bushes, each keeping its own look, as a
first pass without references or review input. This document is
what changes from the old way, so each tree task carries over the method,
not the old tree's pieces.

**Each tree is different.** The leaf masses (clumps) are what made the
T02 tree work, but they are one technique, not a rule. Some trees will
work well with clumps (the lime, the maples, probably the ginkgo); others
may need something else: a cypress may read better as one dense column
than as stacked clumps, a birch's hanging twigs may need cards along
drooping branches, a palm has fronds, a spruce has tiers. Each tree task
first looks at what makes that tree read as itself and picks the
technique that fits it, keeping the rest of the new method (painted
pictures, the wood rules, PBR without a sky rim, the levels and the
impostor path). The per-tree table in §4 is a starting guess, not a
prescription.

## 1. Old and new, side by side

| | old trees (lib/stylizedTrees: Lime, SugarMaple, RedMaple, Ginkgo, JapaneseMaple, Birch, Cypress, Palm, FluffyFoliage, Foliage, FoliageCards, CrownScaffold) | new technique (lib/treeGen, the T02 tree) |
|---|---|---|
| where the crown's look comes from | leaves placed along twigs: textured sprays on joined axes (Lime, the maples, Ginkgo), opaque lobed patches over a crown envelope (FluffyFoliage), six-sided leaves over a round crown (Foliage), hanging sprays (Birch), an inner column with scale sprays (Cypress) | **leaf masses**: separate ellipsoid clumps (center, three radii, a turn) at the branch ends, with sky between them; leaves only fill each mass's skin |
| the shape | one hand-written generator per species with its own growth curves, often at unit height or a 5.5 m reference height, scaled by the placement | one generator: a skeleton grown from numbers (TreeParams ranges, TreeGrower: height, trunk taper and bow, fork height, the leaders' lean and length, side branches' heights, angles and lengths, the masses' count, sizes and spread), in meters |
| branches | species-specific axes and forks (CrownScaffold, CurvedFork, SculpturalFork, ForkSurface) | BranchDetail: every branch tapers to a fine tip, a child is thinner than its parent there and the parent goes on thinner (the pipe rule), two to four straight side branches along each limb, rising, ending inside a mass; LimbJoin: a limb that carries its parent on is one tube, no seam |
| the wood mesh | per-species tubes and fork surfaces | BarkMesh: tapered tubes with frames carried along (no twist), the bark picture wrapped once round and repeated up, a branch starting inside its parent, a dome on every tip |
| the bark | a PNG per species (lib/assets/trees/*Bark.png) | painted in Rae (treeGen/Bark): faint broken streaks, near white, multiplying the material's color; the light-to-dark across the trunk comes from the sun on the round tube; no sky rim on the bark |
| the leaves | cards showing a whole spray with its twig (lib/assets/trees/*Spray.png), or opaque patches / six-sided leaves, rotating cards inside a "conservative envelope" | small square camera-facing cards (0.6 m, about 4.5 layers over a mass's skin, depth 0.75-1 of its radius), each showing a **stamp** of several single small leaves painted in Rae (treeGen/LeafImage: a scatter brush of flat one-tone leaves, see-through gaps; three stamps so neighbors differ) |
| leaf shading | normals from the crown envelope (FoliageCards, FluffyFoliage) | one normal per card: its **mass's** ellipsoid normal (lit top, dark underside, per mass); an occlusion darker deep in the mass and low in the tree; three close greens per mass with a per-mass tint |
| material | toon on (foliageMaterial), the sky rim on | PBR (toon off), roughness 0.8, no sky rim on leaves or bark |
| levels | near: full meshes; middle: fewer, bigger patches or leaves tuned to cover the same (TreeCoverage) or the same textured meshes; far: impostor of the near level | near: every card; middle: every 4th card at twice the width (same covered area, clump layout kept); far: impostor baked from the near level with the cards' normals turned to the eye (ImpostorSubject.cardNormalsToEye), the leaves baked as their filled outline (TreeLevels.leafImpostorImagePixels) and no sky rim (PropField.setPropKindSpriteRim) |
| wind | per spray: a pivot at its branch and a flexibility (FoliageMesh windPivots), so sprays swing on their twigs | per card: a pivot at the card's own center (TreeLevels), so cards only flutter in place; no branch or mass motion yet |
| scale | geometry at unit height or 5.5 m, the species' size in TreeSpecies heights | meters; TreeSpecies.heightUnit is the grown tree's height |

## 2. What carries over and what does not

Carried over from the T02 tree as it is: the skeleton growth, BranchDetail,
LimbJoin, BarkMesh, LeafCards' filling of masses (where the tree suits masses), the three-tone card
meshes and levels (TreeLevels), the impostor path and its fixes, the set
integration (ReferenceTrees.addReferenceKind, a kind in the set's impostor
atlas, a share of the stands), PBR leaves with no rim.

Taken from the old tree, as **numbers only**: its silhouette (height range,
crown width, where the crown starts, its outline: round, oval, columnar,
layered, umbrella), the leaf colors in its three tones (StylizedTrees'
speciesLeafColor / the species' own color functions), the bark color, the
leaf's outline and size (lobed maple, fan ginkgo, heart lime, small oval
birch, scales for the cypress).

Not carried over: the old leaf and bark pictures, the old meshes, the
species generators' growth curves, the crown envelopes, the toon material.

## 3. What treeGen needs first (task 1/12)

- **A tree-kind description** instead of T02's fixed numbers: the
  skeleton's ranges (TreeParams), the masses' layout (count, size ranges,
  shape ratios: a cypress's masses tall and narrow and stacked, a Japanese
  maple's flat and layered, a birch's hanging below its branch ends, a
  ginkgo's upright fan, the maples' rounded clusters), the leaf stamp
  (outline, size, leaves per stamp, angle spread), three greens per mass
  (or autumn colors), the bark color and its streaks.
- **Growth without a trace.** TreeGrower grows from tracedT02Measures (the
  T02 trace). A kind without a trace needs its measures from the
  description's numbers; the trace stays T02's.
- **LeafImage per kind**: the stamp painter takes the leaf outline as a
  parameter (today the almond leaf of T02); the gaps keep the leaves' own
  color (leafCardImagePixels) and the impostor outline is made the same
  way.
- **Bark per kind**: Bark's color and streak strength as parameters (a
  birch: white with dark horizontal marks, a different painter rule).
- **Wind at mass level** (optional, recommended): give each card its
  mass's center (or its branch tip) as the pivot, so a mass sways as one,
  as the old sprays did, instead of every card fluttering alone.
- T02 must not change: its lab renders before and after are the check.

Done (task 1/12, 2026-10-29): `lib/treeGen/TreeKind.rae`. A `TreeKind` is
a name, the `TreeMeasures` the tree grows from (TreeGrower; the masses'
shape ranges `massAcrossMin/Max`, `massUpMin/Max`, `massDepth` are new
measures that default to T02's round masses), whether variety 0 is traced,
a `LeafKind` (the leaf's `LeafOutline`, count, size, width, angle spread
and seed of the stamp LeafImage paints), the card size and layers, the
three greens and a `BarkKind` (color and streak depth). `warmBroadleafKind`
holds T02's numbers; `kindParams` / `kindCardSeed` give a variety's params
and card seed. LeafImage, LeafCards, Bark, TreeLevels (the picture names
are per kind) and ReferenceTrees.referenceImages / addReferenceKind take
the kind. A grown kind still takes its other settings (depth turn, limb
end thickness) from warmBroadleafParams; a kind that needs others adds them
here. Not done: wind at mass level, and a foliage other than masses: the
tree that needs them adds them. T02 is unchanged: the lab's T02 scene and
the levels at near/middle/far from two cameras differ from before by less
than the same build differs from run to run, the leaf card counts are the
same and the painted bark picture is byte-identical.

## 4. Per tree (tasks 2-9)

| tree | the masses | the leaf stamp and colors (the old functions in StylizedTrees and the species modules) | bark color | note |
|---|---|---|---|---|
| lime (speciesBroadleaf) | many medium round masses in a rounded crown, starting low | heart-shaped leaves; its old leaf colors | limeWoodColor | closest to T02 |
| sugar maple (speciesOrange) | rounded clusters in an oval crown | lobed (5 points); sugarMapleLeafColor | sugarMapleWoodColor | autumn colors |
| red maple (speciesRed) | looser rounded masses | lobed; redMapleLeafColor | redMapleWoodColor | |
| Japanese maple (speciesCrimson) | flat, wide masses in layers, low | deeply lobed small leaves; its old crimson | japaneseMapleWoodColor | low joined stems: two or three trunks from near the ground |
| ginkgo (speciesGold) | masses in an upright fan off a curved leader | fan-shaped; its old gold | ginkgoWoodColor | |
| birch (speciesBirch) | small masses hanging below thin drooping branch ends | small oval leaves; its old greens | white (the old albedo 0.94, 0.95, 0.97) with dark horizontal marks | the bark painter needs a new rule; masses hang |
| cypress (speciesCypress) | tall narrow masses stacked into one flame-shaped column | tiny scale leaves; its old greens | cypressWoodColor | branches short and steep, almost hidden |
| palm (speciesPalm) | none: a crown of fronds | frond leaflets; its old greens | speciesWoodColor, a ringed trunk | may not fit masses: fronds as long cards along drooping curves, or a design first |

Done for the lime (task 2/12, 2026-10-29): `stylizedTrees/StylizedKinds.limeKind`,
with masses, because the crown reads as packed round clusters. It is grown
with `TreeMeasures.roundCrown`: T02 is grown in one plane, like its trace;
a round crown turns its branches round the trunk by the golden angle and
fills its masses through the crown's depth (TreeParams.roundCrown keeps the
skeleton from turning them back into the trace's plane). The numbers come
from the old meshes: crown about 0.85 of the height wide, from 0.32 to 1.05
of it; 6 m grown, placed at 4.8-7.2 m. The leaf is LeafOutline.heart. Its
greens are about 2.4 times the old picture's average, in the same hue: the
old toon crown was lit flat, and the mass shading and occlusion of the cards
take that much away (measured in 143). Cost: 2543 cards, 9308 triangles
near (T02: 5844). RAE_STYLIZED_OLD=lime keeps the old lime. Case 1256
checks the middle level's coverage (TreeCoverage with cards facing the
view) and the silhouette. That check found the middle level of every
treeGen tree covering 121-125% of its near level. TreeLevels.leafCardLevel
now moves a grown card inward by what it grew, which gives 100-103%. The
next trees reuse StylizedKinds: add the kind, and name the species in
stylizedNewKind, stylizedTreeKind and stylizedKindVariety.

## 5. Bushes (task 10)

Today StylizedTrees.createStylizedBushParts draws FluffyFoliage's bush form
as app3d/PropInstancer parts, with no levels. New: a short tree kind with
several stems from the ground and no trunk, a few low masses, a painted
leaf stamp, drawn as forest kinds with levels; the walker's bush physics
(slowing) stays.

## 6. The spruce (task 11, design only)

The masses-and-cards method does not fit a conifer's tiers. Starting point
for the design: a straight trunk to the tip; rings of branches every
0.4-0.6 m, shorter toward the top (the cone); each branch drooping a
little, more toward its tip; needles as flat, downward-hanging spray cards
along each branch (shelves), not round masses; the same levels and
impostor.
