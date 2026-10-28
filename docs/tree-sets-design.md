# Tree sets: a low-poly museum set and the stylized set

Status: planned (2026-10-28); queued as six steps (§5); steps 1 and 2
done (0.2.182 and 0.2.183, §5.1, §5.2); step 3's museum done (§5.3), its
studies' move to the tree lab queued as its own step.

Decision: the walker examples keep the stylized trees being made
now (lib/treeGen, the T02 work), while Autumn Grove becomes a museum of the
earlier low-poly style, with all its LOD and billboard work, kept usable
for other games. This document says where the trees are today, how the two
sets are separated, and how the low-poly set comes back from the old
commits to the current Rae.

## 1. Where the trees are today

Everything is in the standard library; nothing is copied per example.

- **Generators**, `lib/procgen/`: one module per kind of tree, each making
  meshes from parameters. At the low-poly commit (rae `4d28e11d`, 23 Oct
  2026, the last before the first birch) there were nine: `Tree`,
  `Foliage`, `FluffyFoliage` (the leaf-patch crowns), `Camphor` +
  `CamphorGeometry`, `Palm`, `Spruce`, `Rock`, `Texture`. Since then six are
  unchanged, `FluffyFoliage` (+49 lines) and `Spruce` (+208/-78) changed,
  and about 14 new modules came (Birch, Lime, Ginkgo, the maples, the
  sculptural and curved forks, crown scaffolds, foliage cards, meadow
  flowers, harebells, oxeye daisies).
- **The species set**, `lib/app3d/GroveTrees.rae`: numbers the species
  (`speciesBroadleaf` .. `speciesBirch`), picks them for a place, sizes
  them, and builds each one's wood, leaves, near/middle levels, colors and
  billboard pictures. 611 lines at `4d28e11d` (nine species), 955 now (the
  birch and its pictures added). There is one set, shared by the walkers
  (`WalkerTerrain`, `IslandForest`, `WideForest`, `TreeCoverage`) and
  Autumn Grove (example 138).
- **The LOD and billboard machinery** is species-neutral and unchanged
  since `4d28e11d`: `app3d/PropField` (instanced levels, dithered
  cross-fades), `app3d/SpriteBake` (coverage-keeping billboard mips),
  `ImpostorBake` (octahedral impostors on the GPU; +97 lines since, all
  general), `app3d/FarCanopy` + `CanopyBake` (the far canopy). It takes
  meshes and pictures; it does not know the species.
- **The stylized generator**, `lib/treeGen/`: the T02 tree, traced and
  grown (`TreeGrower`), with its bark, branch detail, leaf cards and leaf
  pass. Used today by the tree lab (146) and 143's T02 stand.
- **Autumn Grove** (`examples/138_procgen_showcase`): at `4d28e11d` five
  files (`Main`, `GroveScene`, `FluffyGrove`, `CamphorGrove`,
  `GroveGrass`); now it also holds the birch, ginkgo, maple, fork and
  flower studies of the later landscape work.

The coupling between the forests and the species set is small: the walker
modules call about ten `GroveTrees` functions (pick, scale, height, trunk
top, the level meshes, the pictures, the bush parts).

## 2. The shape: two tree sets behind one type

```
lib/treeSets/
  TreeSet.rae            what a forest needs from a set of species
  TreeSetKinds.rae       builds a set's meshes and pictures by its style
lib/lowPolyTrees/        the museum: Autumn Grove's look, frozen
  LowPolyTrees.rae         the set (GroveTrees as of 4d28e11d)
  Tree.rae Foliage.rae FluffyFoliage.rae Camphor.rae
  CamphorGeometry.rae Palm.rae Spruce.rae   (their 4d28e11d versions)
lib/stylizedTrees/       the walkers' set, growing with lib/treeGen
  StylizedTrees.rae        today's GroveTrees, species by species
                           replaced by treeGen-grown ones
```

Each set is its own package (a top folder of `lib/`), not a folder inside
`lib/treeSets/`: every module of one `lib/` package sees every other's
names without an `open`, so the low-poly copies of `Foliage`,
`FluffyFoliage` and `Spruce` would clash with the `lib/procgen` ones the
stylized set uses (found in step 2).

- **`TreeSet`** is data, not code: per species its name, its placement
  weight, its size range and trunk (for the physics bodies), its level
  meshes and materials (near, middle), its billboard pictures and its
  impostor bake settings; per set its bushes. A set's constructor builds it
  once (`createLowPolyTrees(renderer)`, `createStylizedTrees(renderer)`).
  The forests (`IslandForest`, `WideForest`, `WalkerTerrain`,
  `TreeCoverage`) take a `view TreeSet` in place of calling `GroveTrees`,
  and the LOD machinery stays as it is.
- **The low-poly set is a museum.** Its look is frozen at `4d28e11d`: it
  owns its own copies of the generators as they were then, so later work on
  the shared or stylized generators can never change it. Allowed changes:
  language ports, API changes of the machinery around it, and fixes that
  restore the old look. Its acceptance test is the old render: example 138
  at `4d28e11d` rendered with its own compiler (looks-development
  `rendering/beforeAfter.html#groveBeforeBirch`, left) against the port.
- **The stylized set is the walkers'.** It starts as today's
  `GroveTrees`, moved (no look change), and each species is replaced by a
  treeGen-grown one as the reference-tree work reaches it (the T02
  broadleaf first, the varieties already in 143's stand).
- **`lib/procgen/`** keeps what is not a tree set: rocks, textures, the
  flowers. The later tree modules (Birch, Lime, Ginkgo, the maples, the
  forks, scaffolds, foliage cards, `SpruceControl`) were the rejected
  landscape attempts; they move under `lib/stylizedTrees/` while the stylized set
  still uses them, and are deleted as treeGen species replace them.
- **Other games** pick a set. The tree lab (146) gets the setting first
  (`RAE_TREE_SET=lowPoly|stylized`), which proves a set is usable outside
  the museum; the walker apps can take the same setting later.

## 3. The museum: Autumn Grove (example 138)

- Its scenes come back from `4d28e11d` (`GroveScene`, `FluffyGrove`,
  `CamphorGrove`, `GroveGrass`, `Main`), on the low-poly set, with today's
  shared app machinery (the settings panel, light units, the editor-panel
  sun at 20,000 lux, quality).
- A new LOD scene shows the low-poly set at every distance: near, middle,
  billboards, impostors and the far canopy, the same machinery the walkers
  use (until now only visible in 143).
- The later studies (birch, ginkgo, maple, forks, flowers) leave it (see
  §6).

## 4. Porting from 4d28e11d

1. Take each file with `git show 4d28e11d:<path>` into its new place.
2. Compile with today's compiler and fix what the language changed since
   0.2.124: a negative number is `-1`, a literal is not cast to the type it
   already gets, a frozen `let` (write needs `var`), struct literals with
   the type on the left, module/folder names, light in real units
   (`Light3d.sunColor` in lux, the sky in cd/m2), the editor-panel API.
   `rae format` and the compiler's errors list them; nothing is guessed.
3. Render 138 from the camera of the old picture and compare it with the
   old render (`4d28e11d` built with its own compiler, as done for the
   picture above): the same trees, the same colors within the light-unit
   conversion. The pair goes on the looks-development page.

## 5. Steps (for the queue)

1. `TreeSet` and the forests on it: the type, `GroveTrees` moved to
   `stylizedTrees/StylizedTrees` behind it, the forests and
   `TreeCoverage` taking a set. No look change (walker gates, 136/137/143
   screenshots before/after).
2. The low-poly set: `lib/lowPolyTrees/` ported from `4d28e11d` (the set
   and its frozen generators), a test case that builds every species and
   checks its level coverage (case 1228's measure) and triangle counts.
3. The museum: 138's old scenes on the low-poly set, rendered against the
   old picture; the later studies moved out (§6).
4. The low-poly LOD scene in 138 (near to far canopy).
5. The tree-set setting in the tree lab (146): `RAE_TREE_SET` and a
   settings-panel choice, with a forest scene that shows the chosen set
   at every distance. The lab's own scenes for hand-made trees (T02, the
   studies) stay. The lab is the place to compare the sets (2026-10-28);
   Autumn Grove stays the museum.
6. Clean-up: `lib/procgen` keeps only non-tree modules; the rejected tree
   modules go as the stylized set stops using them.

### 5.1 Step 1, done (0.2.182)

- `lib/treeSets/TreeSet.rae`: the `TreeSet` type (its species with their
  heights, the stand and mixed pick tables, the stand size and the mixed
  share) and the functions that read it (`treeSpeciesAt`, `treeScaleAt`,
  `treeHeightFor`, `treeTrunkTop`). The physics trunk and the billboard
  picture settings (`treeTrunkRadius`, `spriteCutoff`, ...) live there too:
  they are the same for every set.
- `lib/stylizedTrees/StylizedTrees.rae` is `app3d/GroveTrees` moved:
  `createStylizedTrees()` is its data, `addStylizedForestKinds` and
  `createStylizedBushParts` build its meshes and pictures.
- `lib/treeSets/TreeSetKinds.rae` builds a set's kinds and bushes by its
  `style` (a set is data; Rae has no function values), so step 2 adds one
  branch for the low-poly set.
- `WalkerTerrain` owns the set (`terrainSystem.treeSet`, given by
  `WalkerApp`); `IslandForest`, `WideForest`, the trunks and the climb test
  read it. `app3d/TreeCoverage` no longer knows any set: it compares two
  levels' parts, and `StylizedTrees.speciesLevelParts` gives them.
- Case `1253_tree_set`. The look did not change: looks-development
  `rendering/treeSets.html` (136, 137, 143 before and after).

### 5.2 Step 2, done (0.2.183)

- The two sets are packages of their own (§2): `lib/stylizedTrees/` (moved
  from `lib/treeSets/stylized/`) and `lib/lowPolyTrees/`.
- `lib/lowPolyTrees/`: `LowPolyTrees.rae` is `app3d/GroveTrees` at
  `4d28e11d` with its picking and sizing turned into the set's data
  (`createLowPolyTrees()`: nine species, the old stand and mixed picks);
  `addLowPolyForestKinds`, `createLowPolyBushParts` build it. The seven
  generators are the `4d28e11d` files with only their import paths, a
  header line and US spellings changed. `TreeSetStyle.lowPoly` and its
  branch in `TreeSetKinds`.
- Case `1254_low_poly_trees` is case 1228 as it was at `4d28e11d`, run on
  the low-poly set: every species' middle level and billboard mips cover
  what they did then, to the percent (the numbers 1228 printed at that
  commit), plus each species' triangles (broadleaf and umbrella 8,484 near
  / 2,772 middle, cypress 10,438 / 3,094, spruce 2,454 / 1,842, the autumn
  trees 52,938 / 12,378, palm 48,594 / 11,154).
- A trial run of 143 on the low-poly set (not kept: the setting is step 5)
  drew the whole forest of 31,042 trees; looks-development
  `rendering/treeSets.html`.

### 5.3 Step 3, the museum, done

- Example 138 is its `4d28e11d` files again (`Main`, `GroveScene`,
  `FluffyGrove`, `CamphorGrove`, `GroveGrass`, `assets/grove_surface.wgsl`,
  the pack and the README), with the tree generators taken from
  `lib/lowPolyTrees/` (138 never used `GroveTrees`; it builds its trees from
  the generators directly). Ported: today's grass API (the shared wind,
  `lib/grass_field.wgsl`, `raeGrassObstacle` in the surface shader), the
  shadow calls' module, the ambient in cd/m2 and the panel's automatic
  exposure, US spellings.
- Rendered from its start camera beside the `4d28e11d` picture: the same
  trees in the same places (looks-development `rendering/treeSets.html`).
  The 138 gate checks the museum's eight views again.
- The studies' files (`BirchGrove`, `BirchStudy`, `ForkStudy`,
  `GinkgoGrove`, `JapaneseMapleGrove`, `MeadowFlowers`,
  `assets/grove_ground.wgsl`, `assets/grove_path.wgsl`) left 138; their
  reference pictures moved to `examples/146_treeLab/references/`. They
  were built on 138's island (its ground height, paths, obstacles, spots
  and views), so moving them into the tree lab is a step of its own, ported
  from rae `fc8dbdc3` where they still run. Until then the
  `tools/capture-*` study scripts that start 138 with a study view do not
  work.

## 6. Decisions

The set the walkers keep is named **stylized** (2026-10-28:
"realistic" is not a good name; US spelling, as in code. The whole tree moved to US spelling
the same day, AGENTS.md).

1. **The later studies stay with the stylized set** (answered): the birch,
   ginkgo, Japanese maple, fork and flower modules move under
   `lib/stylizedTrees/` with it, and will be iterated on heavily from
   the T02 work. Their scenes leave Autumn Grove (the museum); proposed home:
   the tree lab (146), which is the stylized set's workshop, as scenes
   beside the T02 tree.
2. **The museum keeps everything it had at `4d28e11d`**, the leaf-patch
   crowns (`FluffyFoliage`) included: the museum is that commit's look,
   nothing added and nothing taken away.
3. **The walkers keep today's trees** (the birch included) until a
   stylized species replaces one; nothing goes back to the low-poly look.
