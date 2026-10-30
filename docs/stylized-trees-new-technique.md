# The stylized trees in the new technique

Status: tasks 1-11/12 done (2026-10-29), 12 queued; the queue's "stylized trees 1/12 .. 12/12".

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
near (T02: 5844). RAE_STYLIZED_OLD=lime kept the old lime (removed in
task 12). Case 1256
checks the middle level's coverage (TreeCoverage with cards facing the
view) and the silhouette. That check found the middle level of every
treeGen tree covering 121-125% of its near level. TreeLevels.leafCardLevel
now moves a grown card inward by what it grew, which gives 100-103%. The
next trees reuse StylizedKinds: add the kind, and name the species in
stylizedNewKind, stylizedTreeKind and stylizedKindVariety.

Done for the sugar maple (task 3/12, 2026-10-29): `sugarMapleKind`, with
masses and the round crown as the lime has, but with a leaning leader
forking at half the height, long branches that rise only a little, and
masses wider than they are tall (`massAcross` 1.0-1.25, `massUp`
0.75-0.95). This gives a spreading crown, 1.05 of the height wide from 0.40
(the old one: 1.07 from 0.41). The leaf is `LeafOutline.lobed` (five lobes).
Orange cannot be raised 2.4 times to make up for the mass shading, as the
lime's green was, because red is already near 1. So the oranges are as
bright as red allows, with the three tones close, and the shaded side reads
deeper than the old pale orange. Case 1256 (now `1256_stylized_kind_levels`)
checks every rebuilt species against its old tree's numbers.

Done for the red maple (task 4/12, 2026-10-29): `redMapleKind`, with
masses and the round crown, looser than the sugar maple's: about 22 smaller
masses with sky between them, on branches that rise more steeply from a
fork at 0.55 of the height. The crown is 1.08 of the height wide from 0.46
(the old one: about 1.1 from 0.48). The leaf is `LeafOutline.threeLobed`.
The scarlet is about 2.4 times the old picture's average, as for the lime.
The bark is smooth gray with faint streaks. A loose crown's middle level
fills some of the gaps (106% at 2.6 card layers); 3.0 layers bring it to
100-103%.

Done for the Japanese maple (task 5/12, 2026-10-29): `japaneseMapleKind`.
Its look comes from layered plates held out on low, unequal stems, so it
uses flat masses in layers (`massUp` 0.5-0.65 of the radius against
`massAcross` 1.1-1.35). A new grower option, `TreeMeasures.twoStems`, makes
the two leaders two stems: they fork at 0.12 of the height and lean apart,
and the side branches take turns between them. The crown is 1.29 of the
height wide from 0.29 (the old one: about 1.2-1.3 from 0.35), 4 m tall. The
leaf is `LeafOutline.deepLobed` (seven narrow lobes). A flat mass's rim is
thin, so a twice-as-wide middle card sticks out past it (110-113%).
`TreeKind.middleKeep` / `middleGrow` (default 4 and 2.0) are now per kind,
and this tree uses 3 and 1.6, which gives 99-102%.

Done for the ginkgo (task 6/12, 2026-10-29): `ginkgoKind`, with masses
on two fans. The old tree is a curved leader forking into two spreading
fans of rising branches under a rounded crown, so it uses `twoStems`
(forking at 0.38, the trunk bowed) and masses a little taller than wide.
The crown is 1.09 of the height wide from 0.30 (the old one: about 1.15
from 0.31). The leaf is `LeafOutline.fan` (a stalk, a wedge, a notched
edge). The gold was the hard part: a dark yellow reads as olive, and the
blue sky light in the shade turns yellow toward green. The hue stays the
picture's (green 0.62 of red), as bright as red allows, and a new
per-kind `TreeKind.occlusionFloor` (default 0.45, T02's) lets the deep
cards darken less (0.8 here). It still renders about half as bright as
the old flat-lit crown.

Done for the birch (task 7/12, 2026-10-29): `birchKind`, with masses
hanging from drooping branches. A birch reads as curtains of small leaves
off thin, drooping twigs around a slender white trunk, so it uses two new
grower measures: `branchDroop` (side branches bend toward the ground) and
`massHang` (a branch tip's mass hangs below the tip). Its masses are long
and narrow, with smaller middle cards (every 3rd at 1.7 times the width).
The bark uses a new rule, `BarkMarks.birch`: short dark horizontal dashes
and a few dark patches on near-white. The old birch's leaves rose to 1.22
times its height unit, while the other old trees top out near 1.0, so
placed old birches stood 22% taller than 6.5-8.5 m. The new birch grows to
9 m, and `stylizedOldTopRatio` keeps the old size. Crown: 0.82 of the unit
wide from 0.26 (the old one: 0.82 from 0.24).

Done for the cypress (task 8/12, 2026-10-29): `cypressKind`, one dense
column made of masses. Many small, tall masses spread through a narrow
crown read as a lumpy, see-through column from 35 m, so the kept version
uses wide masses centered on the straight stem, stacked until they merge,
with short, steep, hidden branches, dense cards (3.5 layers) and stamps of
many tiny scale leaves. A new grower measure, `crownTaper`, draws the
crown to a point: every mass (the tip and the filling ones) sits closer to
the stem and is smaller higher up, by 1 - 0.7 x (height share)^1.5. The
crown is 0.28 of the height wide from 0.09 (the old one: 0.27 from 0.10),
9.75 m. The blue-green is 1.7 times the old picture's average: the old
cypress renders dark, and a dense column is darkened less by occlusion.
It is lumpier than the old smooth cone. Cases 1251 and 1253 assumed the
cypress is built at unit height (its scale is its height); they now check
the placed height through the height unit, and the umbrella pine's unit.

Done for the "palm" (task 9/12, 2026-10-29): `palmKind`. The set's
`speciesPalm` is not a palm with fronds. Its old code (stylizedTrees/Foliage
palmWood, palmLeaves) is a "palm-style wood": a bending, ringed stem with a
whorl of eleven rising branches from 0.42 to 0.88 of the height, each ending
in a big leafy cluster, and one on top, of broad paddle leaves. (A real
fan or date palm, stylizedTrees/Palm, is used by no tree set, only by case
1147.) To keep the old look, it is grown with masses: big clusters at the
branch tips. The bark uses a new rule, `BarkMarks.rings` (dark bands round
the stem). The crown is 1.00 of the height wide from 0.40 (the old one: 1.07
from 0.38). The §4 row's frond plan would be a new tree, not a rebuild; it
is asked on the page.

Startup fix found here: every picture a kind added rebuilt the renderer's
whole material image array, reading every file again, so the walkers'
start grew with the square of the number of pictures. With eight rebuilt
trees, 136 and 137 took about 14 s to start and failed their gates.
`RendererImages.rendererQueueSurfaceImage` / `rendererQueueSurfaceImagePixels`
queue a picture and `rendererFlushSurfaceImages` builds the array once.
`referenceImages` queues, and the sets flush before baking. 136 now starts
in about 3.7 s (the old trees: 5.6 s).

## 5. Bushes (task 10)

Today StylizedTrees.createStylizedBushParts draws FluffyFoliage's bush form
as app3d/PropInstancer parts, with no levels. New: a short tree kind with
several stems from the ground and no trunk, a few low masses, a painted
leaf stamp, drawn as forest kinds with levels; the walker's bush physics
(slowing) stays.

Done (task 10/12, 2026-10-29): `StylizedKinds.bushKind`, a short tree kind
with two stems from the ground (`twoStems`, a fork at 0.05 of the height),
no trunk, short side branches and a dense, rounded pack of low masses (a
sparse first try read smaller than the old solid mound). It is built at the
old bush form's unit size (1.09 wide and 0.64 tall at scale 1, against 0.99
and 0.58), so the walkers' bush scales (1.5-2.4) stay. It is the stylized
field's kind 15 (`stylizedBushKind`), with an impostor in the trees' atlas.
`TreeSetKinds.treeSetBushKind` tells the walkers, and
`WalkerTerrain.updateDrawnProps` puts the bushes drawn round the camera
into the forest field's placement group 0 (`setPropGroup`) in place of
the instanced bush parts. The bush entities and their slowing are as
before. The walkers had taken every field prop for a tree (a trunk body,
grass kept off), which gave each bush an invisible trunk. `PropKind.hasTrunk`
(false for the bush) is now respected by `walkerTrunkRules` and the grass
obstacles. `RAE_STYLIZED_OLD=bush` kept the old bushes (removed in task
12). 137's far view
counts the bushes' impostors among its billboards (310), and its gate now
checks for at least 300.

## 6. The spruce (task 11, design only)

Designed (task 11/12, 2026-10-29), accepted and built:
docs/spruce-new-technique-design.md. It uses whorl growth in TreeGrower;
its first two rounds put fixed shelf and curtain cards along the branches,
round 3 (§11 - §12 there) made each whorl a skirt filled with
camera-facing cards, and round 4 (§13) builds each whorl as a solid tier,
the low-poly spruce's approach made better (treeGen/SkirtTiers,
`TreeFoliage.skirts`). The levels, the impostor and the painted pictures
carry over. Below is the starting point it grew from.

The masses-and-cards method does not fit a conifer's tiers. Starting point
for the design: a straight trunk to the tip; rings of branches every
0.4-0.6 m, shorter toward the top (the cone); each branch drooping a
little, more toward its tip; needles as flat, downward-hanging spray cards
along each branch (shelves), not round masses; the same levels and
impostor.

## 7. Clean-up (task 12/12, 2026-10-30)

With every tree rebuilt (tasks 2 - 11), the old ones are gone:

- **Modules removed** from lib/stylizedTrees: Lime, SugarMaple, RedMaple,
  JapaneseMaple, Ginkgo, Birch, Cypress, Spruce, SpruceControl, Palm,
  FoliageCards, the forks and scaffolds (CurvedFork, CurvedForkBranch,
  ForkSurface, CrownScaffold, SculpturalFork), Camphor and CamphorGeometry
  (only a case used them), and Tree (used by nothing). Foliage keeps its
  mesh, vertex, face, basis and tube helpers (the umbrella pine and the
  meadow flowers use them), not its palm and broadleaf builders;
  FluffyFoliage keeps the leaf-patch forms (broadleaf, bush, umbrella), not
  the lime, cypress and spruce dispatch.
- **The umbrella pine** is the one species not rebuilt (it is only planted
  with RAE_STYLIZED_UMBRELLA=1; the warm broadleaf takes its share), so
  StylizedTrees keeps its leaf-patch path (addUmbrellaKind) and nothing
  else of the old trees: every other species and the bush are always
  treeGen kinds. `RAE_STYLIZED_OLD` is removed.
- **Cases:** 1235, 1242, 1243, 1245 - 1251, 1142 and 1147 tested only the
  removed trees and are removed; 1141 checks the three leaf-patch forms;
  1228 checks the umbrella pine's levels (the low-poly set's are 1254, the
  rebuilt kinds' 1256). The GPU checks impostorBake and canopyCoverage bake
  the low-poly set's species (the same kind of meshes); the birch's bark
  and spray pictures stay as their test textures, the other old tree
  pictures are removed.
- **The tree lab** lost its tree studies, their files, reference pictures
  and capture scripts (docs/tree-sets-design.md §5.4); its meadow flower
  studies stay.
- **FoliageMesh, FoliagePipeline and FoliageShadow stay:** the new trees'
  leaf cards are foliage meshes (64-byte vertices carrying a wind pivot and
  the card's corner, lib/FoliageMesh), drawn by the foliage pipeline
  without culling and cast by the foliage shadow pass; only the bark and
  the spruce's solid tiers would fit the standard mesh path, and the
  umbrella pine and the meadow flowers use the foliage path too.

