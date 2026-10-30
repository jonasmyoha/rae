# The spruce in the new technique (design)

Status: accepted and built (2026-10-29), rebuilt in rounds 3 and 4
(2026-10-30): `treeGen/WhorlGrower`, `treeGen/SkirtTiers`,
`StylizedKinds.spruceKind`. Cards never read as a spruce (rounds 1 - 3);
§13 - §14, rounds 4 and 5, build it from solid tiers, the low-poly spruce
made better.

The other stylized trees were rebuilt with leaf masses: ellipsoid clumps
whose skins are filled with camera-facing stamp cards
(docs/stylized-trees-new-technique.md). A spruce does not read as clumps.
It reads as a cone of flat tiers: rings of branches round a straight trunk,
each branch a shelf of needles that hangs down toward its tip. So the spruce
keeps everything of the new method except the masses. Cards along whorl
branches replace them.

## 1. What the old spruce is (the numbers to keep)

- `stylizedTrees/Spruce.rae`, a Norway spruce built at unit height and
  placed at 5.5 - 8.5 m (`TreeSpecies` "spruce", height unit 1).
- Crown: a cone from 0.03 of the height to the top, about 0.72 of the height
  wide at its base (the old meshes' reach 0.36). "Broken whorls carry
  unequal, drooping boughs" (its own comment).
- Needles: dark green, the old spray picture's average times its tint,
  linear 0.08, 0.15, 0.06. Bark: the old bark picture times 0.65 gray (to
  be measured the same way when it is built).
- Like the other rebuilt trees, it takes only these numbers. Its spray and
  bark pictures are not reused.

## 2. The shape: whorl growth (what treeGen needs, 1)

A new growth mode in `treeGen/TreeGrower`, chosen by the kind. Today the
measures describe a fork with two leaders and side branches.

- **Trunk:** straight to the tip. The main leader carries the trunk on, with
  no fork and no second leader: `forkHeight` 1 and `secondLength` 0 already
  express that, or the mode skips the leaders.
- **Whorls:** a ring of branches every 0.4 - 0.6 m (a random step in that
  range), from the crown's start (0.03 of the height, below the lowest
  shelf) to about 0.4 m under the tip. A whorl has 4 - 6 branches at even
  turns round the trunk plus up to 15 degrees of jitter. Each whorl is
  turned by a random amount, so whorls do not stack in columns. One branch
  in five is left out, for the old tree's "broken whorls".
- **Cone:** a branch's length is the cone's radius at its height, about
  0.36 of the height at the bottom and 0 at the tip, times 0.85 - 1.15. A
  new measure, `whorlTaper`, gives the cone's shape: 1 a straight cone,
  below 1 a fuller middle.
- **Droop:** a branch leaves almost level (75 - 85 degrees from upright) and
  bends down more toward its tip. The existing `LimbShape.bend` already
  grows along the limb (the goal direction tilts by bend times the share of
  the length), so a positive bend (0.3 - 0.6) is the droop; `branchDroop`
  from the birch task sets it. Lower branches droop more than upper ones (a
  bend rising toward the bottom).
- **Branchlets:** two to four short side shoots along each branch, in the
  branch's own level plane, alternating sides (BranchDetail's side branches,
  kept flat instead of rising). They carry the outer cards, so a shelf is
  wider than its branch.
- `roundCrown` stays as it is (the branches already go all round). The
  masses are not grown at all: `massCount` 0.

## 3. The foliage: cards along branches (what treeGen needs, 2)

The kind says which foliage it has: a new `TreeKind` field
`foliage: TreeFoliage = TreeFoliage.masses`, with `TreeFoliage.shelves` for
the spruce. That is the room for another foliage that `treeGen/TreeKind.rae`
already notes. LeafCards keeps filling masses. A new module,
`treeGen/ShelfCards`, makes the spruce's cards:

- **Fixed cards, not camera-facing.** A card with its `MeshData.leafCards`
  flag at 0 is drawn as a plain quad (gbuffer_static_surface.wgsl,
  shadow_static.wgsl and impostor_bake.wgsl expand only cards whose flag is
  above 0). So shelves need no new renderer code. The foliage pipeline draws
  with no culling (GbufferResources `foliageSpec`), so a shelf's underside
  is seen from below.
- **Two layers per branch:**
  - the *shelf*: cards lying along the branch and tilted 15 - 30 degrees
    down outward, the needle spray seen from above;
  - the *curtain*: cards hanging from the branch, 55 - 80 degrees down,
    rolled -30 to +30 degrees round the branch so some face every way. They
    are what is seen from the side, the "downward-hanging spray cards".
  Every 0.2 - 0.3 m along a branch and its branchlets, from a quarter of
  the length to the tip. About 0.5 m wide near the trunk and 0.25 m at the
  tip; a card is longer down than across.
- **Shading:** one normal per card, as the masses' cards have. It is not
  taken from the card's plane, which would flicker as cards turn; it is the
  cone's normal at the card (outward from the trunk, tilted up by the
  cone's slope), mixed a little toward up for the shelf layer. The cone
  then lights as a cone, as the masses light as ellipsoids.
- **Occlusion:** darker toward the trunk (inner cards) and lower in the
  tree, from the same floor as now (`occlusionFloor`; a spruce is dark,
  0.45 as for T02).
- **Three tones:** three dark greens per whorl, picked per card, so the
  tiers differ a little. `TreeLevels` splits the cards into three tone
  meshes as now; it needs a shelf variant of `leafCardLevel` that builds
  the fixed quads from the cards' corners instead of a center and a size.
- **Wind:** each card's pivot is its branch's base (not its own center), so
  a whole bough sways and bobs as one. This is the mass-level wind the
  other trees still lack (§3 of the main design).

## 4. The pictures (what stays)

- **Needle spray, painted by `treeGen/LeafImage`.** A new outline,
  `LeafOutline.needleSpray`: a thin twig down the picture, short straight
  needles from it on both sides at 50 - 70 degrees, 3 - 5 side twigs with
  their own needles, all one flat tone per twig, see-through between the
  needles. The top of the picture is where it hangs from the branch.
  (T02's "no fern" note was about a broadleaf's leaves; for a spruce, a
  needle spray is the right shape.) Three stamps, as now.
- **Bark, painted by `treeGen/Bark`:** the streak rule, gray-brown, with a
  stronger depth (flaky bark). Mostly hidden under the shelves.
- The impostor's leaf picture is made from the spray as now
  (`leafImpostorImagePixels`: the spray's filled outline). A spray's
  outline is a fringe, so the impostor shows the tiers' ragged edges rather
  than single needles.

## 5. Levels and impostor (what carries over)

- **Near:** every card.
- **Middle:** every 2nd card along each branch, 1.4 times as long along it
  and as wide (the same covered area; the shelf layout kept). A level of
  fixed cards turns with the tree, so it is checked as case 1228 checks the
  old trees (`levelCoverageShares` with `cardsFacing: false`), within 5%,
  in case 1256 like the others.
- **Far:** the hemi-octahedral impostor baked from the near level, as for
  every rebuilt tree, with no sky rim. `cardNormalsToEye` turns only
  camera-facing cards, so it has nothing to do here: fixed cards bake with
  their own normals. A cone is the easiest shape for the impostor: it looks
  the same from every side.
- **Shadows:** fixed cards cast as geometry (shadow_static.wgsl), so the
  tiers throw tiered shadows, which the masses' camera-facing cards cannot.
- **Set integration:** as the others: `StylizedKinds.spruceKind`,
  `stylizedNewKind` / `stylizedTreeKind` / `stylizedKindVariety` for
  `speciesSpruce`, `RAE_STYLIZED_OLD=spruce` (removed with the old trees), pictures queued
  (`referenceImages`) and built once.

## 6. Size and cost

A 7 m spruce has about 12 whorls of 4 - 5 branches, about 55 branches with
their branchlets. At 0.25 m steps and two layers, that is about 10 cards a
branch: 550 cards, 1100 triangles, plus the bark. That is cheaper than the
masses' trees (1300 - 2500 cards) because the cards sit only where needles
are, not over a mass's whole skin.

## 7. The steps it would take (one task each)

1. Whorl growth in TreeGrower (the mode, `whorlTaper`, broken whorls, flat
   branchlets) and `TreeKind.foliage`, with a skeleton check in case 1239.
2. `treeGen/ShelfCards` (shelf and curtain cards, cone normals, bough
   pivots) and the shelf variant of TreeLevels' card level.
3. `LeafOutline.needleSpray` in LeafImage.
4. `spruceKind` in the stylized set, its case 1256 entry (coverage with
   fixed cards, silhouette), the before/after pictures and gates 136, 137,
   143, 146, as for the other trees.

## 8. Open questions

- Accept the design: a cone of whorl branches with needle-spray cards along
  them as shelves and hanging curtains, in place of masses?
- The curtain cards are fixed. From a few angles a card is seen edge-on,
  and the roll spread of -30 to +30 degrees is meant to hide that. If a
  test shows thin lines, the fallback is to make the curtain cards turn
  about their branch toward the camera (a new card mode in the shaders).
  Is that fallback acceptable if needed?

## 9. As built (2026-10-29)

- **No branchlet tubes.** The flat branchlets (§2) made about 180 extra bark
  tubes, most of the tree's 44,000 triangles, under the needles. The kind
  has `branchletsMin`/`Max` 0, and its wide shelf cards (0.7 m next to the
  trunk) make the shelf wider than the branch. The grower still supports
  branchlets.
- **Boughs of 6 steps.** `LimbShape.steps` (default 16) is 6 for whorl
  branches. The near level is 12,400 triangles, still mostly bark.
- **Curtains hang along their branch.** They span the branch's direction and
  the hanging direction, the hang turned -30 to +30 degrees round the branch
  (a first build laid them across the branch, edge-on from its side).
  Cards come every 0.15 - 0.2 m from a fifth of a branch's length, a
  curtain 1.6 times as tall as it is wide.
- **The lowest whorl is at 0.08 of the height** (not 0.03): the curtains
  hang to near the ground, where the old cone's foliage started.
- **Middle level:** every 2nd shelf-and-curtain *pair* (every 2nd card kept
  only the shelves) at 1.15 times their size, 100 - 101% of the near
  outline. Grown more, the curtains reached past their branch tips (1.4,
  the same area, covered 110%).
- **Impostor:** the far side's shelf cards lit the impostor pale
  blue-gray. A first fix turned the fixed cards' normals toward the bake's
  eye (`ImpostorSubject.turnPartsToEye`); the real cause was the sky rim
  (§10, the impostor fix), and the flag is gone: the bake keeps the cards'
  own normals, as the near pass draws them.
- **Thinned per pair, not per card**, and checked in case 1256 (with
  `TreeCoverage` turning fixed cards as geometry, flag 0, and
  camera-facing cards by their center, per vertex). Case 1250, the old
  spruce's case, now checks the placed height through the height unit.

## 10. Round 2 (2026-10-29: "better than earlier, worse
than the low-poly one")

- **No wind** on the spruce for now: its bough sway disturbed the look
  (ShelfCards' flexibility is 0; a wind task is queued).
- **No branches and no hidden trunk:** only the trunk from the ground to
  0.4 m into the lowest tier is drawn (ShelfCards.lowerTrunkSkeleton,
  ReferenceTrees.kindBarkMesh). The lowest whorl is at 0.16 of the height,
  so the trunk shows under the skirt. The near level went from 12,400 to
  about 1,200 triangles.
- **Fluffy, big foliage in place of thin needles:** stylized conifers are
  made of big foliage-clump cards shaded as one volume (Polycount and
  stylized-tree tutorials: cards that each stand for a whole branch
  system, normals taken from a blob or the cone). The stamp is
  `LeafOutline.tuft`, 4 - 6 big rounded tufts with a scalloped edge, on
  1.0 m cards next to the trunk, curtains 1.1 times as tall as wide, and
  shelves tilted 35 - 55 degrees so they do not read as edge-on streaks.
- **The impostor fix (2026-10-29):** from 40 m the far impostor's upper
  half was lighter than the near level and its lower half dark teal, with a
  hard line between them. Two causes, both in every impostor, not only the
  spruce's:
  - *The sky rim was never off.* A no-rim impostor is marked with a
    negative sprite emissive (`addSpriteInstanceWithRim`), but
    `packInstanceRecord` stores every non-emitter as 1.0, so the sign was
    lost and the rim lit every normal turned from the eye in the sky's
    color. The mark is now written after packing, and the sprite pass
    writes its size (1.0, unoccluded) as the occlusion.
  - *The impostor shadowed itself.* Its shadow caster is one picture plane
    facing the sun, through the bounding sphere's center, so the front half
    of the surface it shows lay behind its own caster. The plane now stands
    at the sphere's back (shadow_sprite.wgsl); moved along the sun's rays,
    it throws the same shadow on everything outside the sphere.
  The near level stays a few percent darker at the top: its tiers shadow
  each other, which one plane cannot.

## 11. Round 3 (proposal): the low-poly spruce's skirts as the shape

After round 2 the spruce still read badly: its fixed cards do not turn
toward the camera, so from most angles they show as flat slabs or thin
edges, and the low-poly spruce (`lowPolyTrees/Spruce`) still reads best.
What makes that one work is its shape, not detail: six stacked, solid,
scalloped cone skirts, each with a smooth lit top and a closed underside,
lit through broad normals. A semi-realistic spruce can keep exactly that
and add what the other new trees have:

- **Shape:** the low-poly tiers as the leaf masses, 8 - 12 of them on a
  straight trunk (the whorl grower's heights and widths), each a scalloped
  skirt with a few longer, drooping bough tips breaking its rim, so the
  silhouette is ragged where the low-poly one is a clean polygon.
- **Foliage:** the skirts filled with camera-facing tuft cards, as the
  masses of the broadleaf trees are (LeafCards over a mass's skin), so the
  foliage turns toward the camera. Each card takes its skirt's normal (the
  low-poly tier's broad normal), so a tier lights as one smooth shape and
  the cards only fray its edge. The fixed shelf and curtain cards go.
- **Depth:** the skirt's underside darker (the occlusion floor), a gap of
  shadow between tiers, as the low-poly tree's closed undersides give it.
- **Levels:** near every card; middle fewer, bigger ones; far the impostor,
  as for the other rebuilt trees. A camera-facing card mass is what the
  impostor bake and case 1256 already handle.
- **Kept:** the dark spruce green, no wind for now, only the lower trunk.

## 12. Round 3 as built (2026-10-30)

- **`TreeFoliage.skirts`** (`treeGen/SkirtCards`): the whorl grower's tree;
  each whorl a tier, at most 8 (more whorls share a tier, so each tier is
  tall enough for its step to show). A tier's rim reaches 0.82 of its
  branches' mean reach between them and out to each branch's tip along
  it, and hangs down to the tip's droop there (the bough tips). Its top
  rises 1.7 times the step to the next tier at the trunk (the low-poly
  tier's proportion; no cap at the reach, or the trunk showed between the
  small upper tiers), the top tier to the tree's tip; its underside is
  nearly flat under the rim.
- **Cards:** `cardLayers` (5) cards over each point of the top skin, a
  quarter as many on the underside toward the rim; 0.45 m on the widest
  tier, smaller on the upper ones. A top card takes the low-poly tier's
  broad normal (up and out, more out at the rim); an underside card faces
  out and down (straight down it lit nearly black). The occlusion is
  darkest in toward the trunk, under the tier above: the band that makes
  the tiers read. The cards are the masses' own (`LeafCards` layout), so
  the leaf pass turns them to the camera and the levels, the impostor and
  case 1256's coverage check need nothing new.
- **Wood:** only the trunk under the lowest skirt (`innerWood` false;
  `lowerTrunkSkeleton` moved into SkirtCards). **No wind** (every card's
  flexibility 0). The lowest whorl at 0.12 of the height (the crown from
  10%, within the old tree's 12%).
- **Levels:** middle every 3rd card at 1.35 times (101 - 103% of the near
  outline; 1.5 covered 105 - 108%). Near 1,305 cards, 2,792 triangles.
- The shelves mode and `treeGen/ShelfCards` are removed.
- **The picture (2026-10-30):** the round, scalloped tuft stamp read as
  cookie-cutter blobs. `LeafOutline.sprig` replaces it: one or two fronds
  a card, each a main sprig with three pairs of side sprigs 50 degrees
  forward, shorter toward the tip, every sprig pointed with a forward-
  slanting fringe of needle tips. A frond leaves more of its card open, so
  `cardLayers` is 8 and cards 0.5 m (1,695 cards near).

## 13. Round 4: solid tiers (2026-10-30)

Round 3's camera-facing cards on skirts did not work either, whatever the
stamp (round tufts read as cookie-cutter blobs, needle fronds as a ragged
broadleaf). What other games do: stylized pines are geometry, stacked cone
tiers shrinking upward, shaded by smooth volume-wide normals and a color
gradient, not alpha cards. Firewatch's trees were made by hand, and its
lead artist called pines the hardest trees to make in 3D (80.lv,
"Creating the world of Firewatch"); procedural stylized pines stack cone
tiers with per-face colors (Josh Marinacci, "Procedural geometry trees");
cel-shaded foliage edits normals toward one smooth volume. So the spruce
is the low-poly one's approach, made better:

- **`treeGen/SkirtTiers`** (in place of SkirtCards): the whorl grower's
  tree, each whorl one solid tier (at most 8). Its top falls from 1.7
  times the step to the next tier above the whorl, at the trunk, to an
  uneven rim (two sine waves, up to 7% in or out) that reaches out and
  hangs down to each branch's tip (the drooping bough tips); its
  underside closes it under the rim. Each tier is turned its own way.
- **Color:** three bands of tone, so a tier is dark at the trunk (and on
  its underside: the dark line under each tier) and light at the rim: the
  darker green times 0.45, the main green times 0.66, the lighter times
  0.76. **Normals:** the low-poly tier's broad ones, up and out, more out
  at the rim, smooth round the tier.
- A rim cut into alternating points and notches was tried and dropped: the
  folds shadowed each other into a pleated lampshade.
- `taper` 0.75 (a fuller middle; 0.95 left the upper tiers skinny), the
  lowest whorl at 0.09 of the height (the crown from 10%). Near 36
  segments round each tier (about 2,500 triangles), middle 18 (100% of
  the near outline). No leaf picture (the kind's stamp, card size, layers
  and middle thinning are unread), no wind, only the lower trunk.

## 14. Round 5: no bands, a deeper green, a less cartoony shape (2026-10-30)

Round 4 read as banded (its three bands of tone showed as stripes on every
tier), grayish and light, and goofy: stacked lampshades.

- **No bands:** a tier's top is one green. Neighboring tiers alternate
  between the kind's first two greens, which are close; the undersides
  take its third, darker one.
- **Green:** 0.03, 0.085, 0.038 and 0.034, 0.093, 0.04 for the tops,
  0.019, 0.055, 0.026 underneath: a deep spruce green with little red and
  blue in it (the old 0.08, 0.15, 0.085 rendered gray).
- **Shape:** every whorl its own tier (`mostSkirts` 14, was 8), each
  rising 2.2 times the step to the next (was 1.7), so the tiers overlap
  deeply and read as layered boughs. Each tier is tilted a few degrees its
  own way (5 - 9% a meter), so the tiers draw no level lines. The rims
  are ragged: 0.72 of the mean bough reach between the boughs (was 0.82),
  three waves of unevenness, and broader bough lobes (the pull as cos^6,
  was cos^8) whose tips hang half again lower than their branches end. A
  narrower cone (`baseReach` 0.29, was 0.32): 67% of the height wide.

- **Wind (2026-10-30):** each tier swings gently about its whorl's point on
  the trunk (flexibility -0.5, the broad mode; docs/stylized-trees-new-
  technique.md §8).

