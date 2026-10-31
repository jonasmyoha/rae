# Seasons, temperature and weather (design)

Status: design (2026-10-30; winter, ice and the panel readouts added
2026-10-31). Not built. §9 lists the tasks, queued in QUEUE.md as
"[seasons n/15]".

The stylized trees, the grass and the terrain change with the seasons: green
leaves turn to autumn colors and, on the deciduous species, fall; grass and
ground go from fresh green to dry and brown; snow covers the ground and the
trees in winter. What drives it is a small climate model: the date, the
place on Earth (latitude, longitude, height) and the weather give a
temperature, and the temperature drives the seasons. The weather (clouds,
rain, mist) also sets how much sun reaches the ground.

Where it applies: the huge terrain (143, the walker forest) and the tree
lab (146), and through lib/app3d the other walkers (136, 137). The low-poly
tree set (lib/lowPolyTrees, the museum 138) does not change with the
seasons.

The rule over all of it: **playable performance.** Every season and weather
value is a handful of numbers per frame (per tree kind, per scene), not work
per tree, per blade or per pixel beyond a few shader instructions. Nothing
here may add more than about 1 ms to 143's frame (about 18 ms today on the
development machine), and §8 says how each part stays inside that.

## 1. Time and place: what exists, what is added

- **The clock exists:** `app3d/TimeOfDay` has an hour, a day of the year
  and a year. `app3d/SettingsSky` turns the hour, the date, the latitude
  and the longitude into the sun's direction (Helsinki by default), so the
  sun already follows the seasons. The walkers' day lasts 30 minutes.
- **Added: the year's speed.** At 30 minutes a day a year takes 7.6 real
  days, so nobody would see a season pass. `TimeOfDay` gets `yearSpeed`:
  how many days the date moves per simulated day (1 real; a game might use
  14, a year in about 13 hours of play; a test or the lab can set the day
  directly). The hour and the date stay separate: the sun's daily arc does
  not get faster.
- **Added: the place.** The scene's latitude and longitude exist in the
  settings; the terrain gives the height above the sea (WorldBiome, the
  terrain height). 143's world radius is 1,000 km, so far from the origin
  the latitude changes: a walker 500 km north is 4.5 degrees further north.
  The latitude at a point is the scene's latitude plus its north offset
  over the Earth's radius (111 km a degree).

## 2. The climate model (temperature)

One CPU function, `climateAt(place, date, hour, weather)`, gives the air
temperature in °C. It is a fit, not a simulation:

- **The yearly mean and swing by latitude:** the mean falls from about 27
  °C at the equator to about -22 °C at the poles (27 - 0.0061 x
  latitude², which gives Helsinki, 60° N, 5 °C; the first draft's
  27 - 0.45 |latitude| - 0.004 latitude² gave -14 °C there); the swing between summer and winter grows with latitude (about 2 °C
  at the equator, about 22 °C at 60°). The coldest day is about 25 days
  after the winter solstice (late January in the north, late July in the
  south: the hemisphere flips the season).
- **The day:** about ±5 °C round the day's mean, warmest at 15:00, coldest
  at dawn; under clouds the daily swing shrinks to a third.
- **Height:** -6.5 °C per 1,000 m above the sea (the standard lapse rate).
  143's hills are at most a few hundred meters, a degree or two.
- **Weather:** clouds and rain cool the day (-2 °C under full cover), and
  a weather front moves the day's mean by a few degrees (§5).

Every value is in °C and the function is deterministic for its inputs, so
the same date and place always give the same answer.

**The season is not the calendar, it is the temperature.** Plants follow
how warm it has been, not the date: a cold spring is late. The scene keeps
a **slow temperature**, the air temperature averaged over the last two
weeks (an exponential average, stepped once per simulated hour), and the
seasons below are driven by it. A walker who moves 500 km north into colder
air sees an earlier autumn there, because the slow temperature is per place:
it is computed for a coarse grid of points (one per 50 km), not for every
tree.

## 3. Trees through the year

### 3.1 What a kind says

Each `TreeKind` gets a **season** block:

- `deciduous: Bool` — the leaves fall (lime, the maples, ginkgo, birch,
  the palm-style tree's broadleaf cousins and the bush: yes; spruce,
  cypress: no).
- `autumnGreens: List(Vec3)` — the three autumn tones (the sugar maple's
  orange, the red maple's scarlet, the ginkgo's gold, the birch's yellow,
  the lime's yellow-green, the bush's brown-red). The autumn kinds of
  today (AutumnKinds: the sugar maple, red maple, Japanese maple and ginkgo
  are colored all year) become the autumn end of their species: green in
  summer, their color in autumn. (A decision for the task split: the
  walkers then show a green forest most of the year, with the colors only
  in autumn; keeping some always-colored "fantasy" trees is the other
  choice.)
- `springGreens: List(Vec3)` — a lighter, yellower green for fresh leaves
  (a few weeks only).
- `winterGreens` for the evergreens: a darker, slightly browner green.
- The temperatures where its phases start (defaults per kind): leaves come
  out when the slow temperature rises past about 8 °C, turn when it falls
  past about 10 °C, are fully turned at about 5 °C, are gone at about 2 °C.
  Species differ by a degree or two, so a forest turns over a few weeks,
  not all on one day (the birch and the ginkgo first, the oaks and limes
  last).

### 3.2 How it is drawn (cheap)

- **Colors are per kind, per frame, nothing baked again.** A kind's leaf
  parts are three tone meshes, each drawn with one material color
  (ReferenceTrees' `referenceLeafMaterial`). The season blends that color
  between spring, summer, autumn and winter greens: a CPU blend of three
  colors per kind per frame, written into the instance batch's material.
  Within a stand the trees should not change in step: each tree gets a
  **phase offset** (a few days earlier or later). The instance record has
  no free slot ([40..42] hold the level fade, [43] the sway), so the
  shader derives the offset from the tree's own world position (a hash of
  the model matrix's translation, as the fade seed is hashed from the
  placement) and shifts the color blend by it: no new data per tree.
- **Leaves falling: a discard by card.** The shaders already discard
  whole instances by a threshold (fade_dither.wgsl, the level fades); leaf
  loss does the same per card. Every vertex of a leaf card carries the
  card's center (`MeshData.leafCards`) and its leaf mass's wind anchor
  (`windPivots`), so the shader hashes the center into a per-card random
  and discards the card when the random is below the kind's **leaf loss**
  (0 to 1, a uniform per draw). Cards far from their anchor (the mass's
  outside) fall first: the random is biased by that distance. No mesh
  changes, no new vertex data. At loss 1 the tree is its bark: the rebuilt
  trees already draw the whole skeleton (innerWood true for the broadleaf
  trees), so a bare tree is a real winter tree.
- **The impostors and the far canopy** have their colors and leaves baked
  in (the atlas is baked once at load). Two options, and the design takes
  the second:
  1. bake again when the season moves (the tree atlas and the canopy take
     about 1 s at load; at a season step, spread over many frames, it is
     invisible);
  2. **bake three states once at load** — summer, autumn (fully turned),
     bare — as three atlas layer sets, and let the sprite shader blend
     between the two that bracket the kind's state (one more texture read
     per far pixel; the far pixels are a small part of the screen). The
     atlas grows from one set to three (memory: three times today's tree
     atlas and canopy atlas, to be measured in the task; acceptable on the
     desktop if it stays within a few tens of MB, the phone keeps one set
     and tints it).
  Spring uses the summer set tinted; winter evergreens the summer set
  darkened.
- **Snow on trees:** in winter (§4) the leaf and bark shaders lighten the
  parts of a card or a tier whose normal points up, by the scene's snow
  amount (a few instructions; the spruce's solid tiers carry snow on their
  tops, which is exactly how a snowy spruce reads).
- **Falling leaves (later, its own task):** a GPU particle system of leaf
  cards that fall from a kind's crown while its leaf loss rises, using the
  kind's autumn tones. Not part of this design's first steps; the leaf
  loss value it would read is designed here.

### 3.3 The colors themselves

The greens were the first problem (the trees read as plastic lime). That
part is done (nicer greens, 0.2.213 - 0.2.216: olive greens anchored on
T02's, the birch the lightest). The seasonal palettes take today's greens
as the summer state, so the autumn and spring tones must be chosen against
them in the scene's light (136, 143 and the tree lab), as the greens were.

## 4. Grass, ground and snow

- **The grass and the painted ground** take their color from a few
  constants today (`walkerGroundColor`, `RAE_TERRAIN_GRASS`,
  `WorldBiome.grassMatColor`, the tuft material). They become **one
  per-frame ground palette** (a uniform: fresh, summer, dry, dead-winter
  grass colors blended by the slow temperature and the soil's moisture
  from WorldBiome), read by the terrain, the grass and the tufts. The two
  copies that must be kept equal by hand today (the Rae constant and the
  WGSL one) become one value passed to both.
- **Dry summer:** a hot, dry spell (§5) yellows the grass where WorldBiome's
  moisture is low (hilltops, sand edges) first.
- **Snow:** the scene keeps a **snow depth** (meters), stepped once per
  simulated hour: it grows while it precipitates and the temperature is
  below about 0 °C, and melts above it (about 3 mm per °C per hour). The
  terrain shader blends to snow by the depth and the ground's slope (no
  snow on steep rock) with a noise edge; the grass is pushed down and
  hidden under a depth of about 10 cm (the grass compute already places
  blades by a density: snow lowers it); the tufts sink. Water does not take
  snow until it is cold for long (ice is a later task).
- **Per place:** 143's slow temperature grid (§2) also holds the snow
  depth per 50 km cell, so the north of a 1,000 km world can be white while
  the south is green. Between cells the shader blends.

### 4.1 Deep winter: snow as a mesh, footprints that sink

When the snow is deep (from about 5 cm, the peak of winter), painting it
is not enough: it is a **physical layer**.

- **The snow surface is geometry.** Near the walker (a square of about 40
  m, following it like the grass grid) a fine snow mesh (a vertex every
  10 - 15 cm) lies over the terrain, raised by the snow depth with a soft
  noise (drifts against trunks and rocks, less on slopes), in a snow
  material (white, blue in the shade, a faint sparkle). Further out the
  terrain itself is raised by the depth in its vertex shader and painted
  snow, so the two meet without a step.
- **Footprints sink.** A trample map round the walker (a texture of about
  64 m at 6 - 12 cm a texel, scrolled as the walker moves) is stamped where
  a foot comes down (the walker's foot events, app3d/WalkerMarks /
  GroundMarks already find them), deeper for a run than a walk, a little
  rim of pushed-up snow round each print; the snow mesh is lowered by it.
  Prints stay until new snow fills them (the map fills back by the
  snowfall). The crowd's feet stamp it too.
- **Feet stand in the snow.** The walker's ground height becomes the
  terrain plus the snow depth minus the trampled depth under the foot, so
  the legs sink in up to the snow's depth (capped at the knee); deep snow
  slows the walk a little.
- **Grass is gone** under deep snow (the grass compute's density 0), the
  tufts and turf hidden, the bushes white on top.

### 4.2 Winter trees

At the deep-winter end of the year every deciduous kind has lost all its
leaves (leaf loss 1, §3.2): the trees are their bark. The spruce (and the
other evergreens: the cypress) keep their needles. Snow lies on them: the
up-facing parts of branches, trunks and the spruce's tiers turn white by
the scene's snow amount (§3.2), heavier after a snowfall, and slides off
slowly in thaw.

### 4.3 Ice

Water freezes when the slow temperature has stayed below about -3 °C for a
few days, and thaws above 0 °C: a **freeze** value per water body (lakes)
and per stretch of shore (the sea), stepped hourly with the climate.
Lakes freeze over completely, the sea only along the shore in deep cold.
Frozen water draws as ice (opaque, white-blue, cracks, snow on top while
it snows) instead of moving water, and is walkable: the walker's water
test treats frozen water as ground at the water's surface.

## 5. Weather

A small weather model drives cloud cover, rain and mist, with fronts that
pass over several hours:

- **State:** cloud cover (0..1), precipitation (mm per hour), humidity,
  wind (it already exists: lib/Wind), and a front: a slow random walk in
  time (deterministic from a seed and the date), so the weather changes
  over hours, with more rain in the climate's wet season.
- **Its effects:**
  - **The sun:** the sun's illuminance on the ground falls with cloud
    cover (full cover lets about 20% through, mostly as diffuse light); the
    sky's own light gets grayer and its share of the light grows. This is
    the sun lux the settings already have (`sunNoonLux`), now scaled per
    frame. The auto exposure follows.
  - **Clouds:** the sky's clouds (today a fixed look) take their cover and
    their darkness from it.
  - **Mist:** RendererMist's density rises when the air is near its dew
    point (cool mornings after a humid day, autumn), and falls by midday.
  - **Rain:** a screen-space or near-camera particle rain, and wet ground
    (darker, a little glossier) while it rains and for an hour after.
    Snow falls the same way as particles when it precipitates below 0 °C.
  - **Temperature:** §2 (clouds and rain cool the day).
- **The tree lab and the walkers' settings** get a weather override
  (clear, cloudy, rain, snow, mist) next to the time controls, as the time
  of day has today, so a look can be judged without waiting for it.

## 6. The tree lab and the walkers

- **146:** a season control: the day of the year (a slider and
  `RAE_DAY_OF_YEAR`), the latitude (`RAE_LATITUDE`), and a "year runs"
  speed; the forest and the single-tree views show every kind in its
  state. A check renders each kind at summer, autumn, bare and snowy.
- **143 (huge terrain):** the climate grid over the world, the year speed
  from the walker's settings, snow and weather.
- **The settings panel (every app with it):** under the time and the date,
  the **temperature** where the camera looks (the climate's air
  temperature now, e.g. "-12.4 °C") and the **season** in words, from the
  slow temperature ("early spring", "summer", "autumn", "deep winter",
  and "dry summer" with the weather). Under the latitude and longitude, the
  **nearest well-known city** to that place ("near Helsinki, Finland, 12
  km"): a table of a few hundred cities in lib (name, country, latitude,
  longitude) and the great-circle nearest, so a jump to an eclipse or a
  latitude slide says where on Earth the scene is.
- **136, 137:** the same through lib/app3d (they share the walker code).

## 7. Who owns what (ECS, no globals)

- A `Climate` resource on the walker app / lab: the slow-temperature and
  snow grid, the weather state, the year speed. Stepped by a climate system
  once per simulated hour (cheap), read by the render systems every frame.
- A `SeasonState` per tree kind (on the tree set's forest): its phase, its
  current three tone colors, its leaf loss, its impostor blend — computed
  from the Climate once per simulated hour, written to the batches each
  frame.
- A `GroundPalette` (the grass and ground colors, the snow depth near the
  eye) passed to the terrain, grass and tuft draws as one uniform.
- No module-level state: all of it lives on the app's resources and is
  threaded through `mod` / `view` parameters like the time of day today.

## 8. Performance

| part | cost per frame | why it stays small |
|---|---|---|
| climate, snow, weather | none most frames | stepped once per simulated hour, a few hundred grid points |
| tree colors | a few hundred CPU flops | three colors per kind (about 20 kinds), not per tree |
| per-tree phase | a hash in the shader | from the instance's position: no new record data |
| leaf loss | a hash and a compare per card vertex or pixel | the card center is already in the vertex data |
| far trees, canopy | one more atlas read per far pixel | far pixels are a small share; phone keeps one set |
| snow, ground palette | a few instructions per terrain/grass pixel | uniforms, no new textures |
| rain, snow particles | one instanced draw near the camera | a few thousand particles at most |
| mist, clouds | the existing passes with other numbers | no new pass |

The budget is checked like the levels' fades were: 143's frame time over a
fixed camera move before and after each step, within 1 ms, in the commit
that adds it.

## 9. The tasks (queued as "[seasons n/15]")

1. **Nearest city:** the city table and the panel line under the latitude
   and longitude (§6). Independent of the rest. Done (2026-10-31): lib/Cities
   (381 cities, the haversine nearest), the panel's "near ..." line, case 1262.
2. **Year and place:** `TimeOfDay.yearSpeed`; the latitude at a point
   (143's north offset); the lab's `RAE_DAY_OF_YEAR` / `RAE_LATITUDE` and a
   day slider. Done (2026-10-31): `TimeOfDay.yearSpeed` (at 1 midnight rolls
   the date as before; at any other speed the date moves by the speed
   alone), lib/GeoPlace `geoPlaceAt` (+Y north, +X east), the lab's
   LabSeason (`RAE_DAY_OF_YEAR`, `RAE_LATITUDE`, `RAE_YEAR_SPEED`, a day
   slider over the frame), case 1263.
3. **Climate and the panel readout:** `climateAt` (yearly mean and swing
   by latitude, the hemisphere, the day, height; longitude through the
   local solar time), the slow temperature grid stepped hourly, a case for
   the fit (Helsinki, the equator, the southern hemisphere); the panel's
   temperature and season lines (§6). Done (2026-10-31): lib/Climate
   (climateAt, climateSlowMean, climateSeasonName on the thermal seasons:
   summer from a 10 °C two-week mean, winter at 0 °C, deep winter below
   -5 °C; the Climate grid of 50 km cells, stepped hourly, reseeded after a
   jump), the walker app's climate (1,000 km for 143) and the lab's, the
   panel line "Temperature 14.2 °C, summer" where the camera looks
   (app3d/SettingsClimate), case 1264.
4. **Tree season states:** the kind's season block (deciduous, spring /
   autumn / winter tones, phase temperatures); the per-kind color blend and
   the per-tree phase offset; the autumn kinds folded into their species.
   Done (2026-10-31): `TreeSeason` on the TreeKind (deciduous, the autumn
   color, leafOut / turnStart / turnFull), app3d/PropSeason's tint (spring
   fresh green fading over 6 °C, the autumn color between the turn
   temperatures, then darker; evergreens a little darker in deep cold),
   multiplied into the leaf parts' materials when the PropField gathers
   them. The per-tree offset (±1.5 °C) is hashed from the tree's position
   on the CPU per instance, not in the shader: the instance record has no
   free slot, and the tint is per material anyway. The four autumn kinds
   are green in summer. The far impostors stay summer green until step 6.
5. **Leaf loss:** the leaf pass's loss threshold (outer cards first), bare
   winter trees (all deciduous leaves gone, spruce and cypress keep theirs);
   case checks of cards kept per loss.
   Done (2026-10-31): app3d/PropSeason propSeasonLoss (falling: from a
   degree past fully turned to bareAt; rising: the 4 °C up to a degree
   past leafOut; the square root of the ramp, since a crown 3 to 6 card
   layers deep still looks full with half its cards gone), with the tree's
   own ±1.5 °C shift. The loss is per tree, not per draw: the instance
   record has no free slot, so a leaf part's loss rides in its metallic
   slot [35] (a leaf card is never metal; the static vertex stage passes
   metallic 0 on for cards) and the shadow caster carries it beside the
   image code. lib/wind.wgsl leafCardKept collapses a lost card in the
   vertex stage (main pass and shadows); at loss 1 the gather skips the
   leaf parts. Case 1265 counts the cards kept per loss with the same key
   (propLeafCardKey). The impostors stay summer until step 6.
6. **Seasonal impostors:** three baked atlas sets (summer, autumn, bare)
   and the sprite and canopy blend; the phone's tinted single set.
   Done (2026-10-31), with the design's option 1, not 2: measured in 143,
   the tree atlas is 170 MB and the far canopy 307 MB (the phone's: 42 and
   43 MB), so three sets would have cost about 950 MB more, far past the
   few tens of MB this design allowed. Instead app3d/SeasonImpostors bakes
   both again in place when the forest's slow temperature moved by 0.25 °C
   or its direction turned (at most every 20 frames): each kind's subject
   gets its leaf tint and leaf loss (ImpostorSubject.leafTint, leafLoss;
   the bake drops cards by lib/wind.wgsl leafCardKept, a bare kind is
   baked as its wood), then the canopy is baked again from it (FarCanopy
   keeps its trees and clusters). 100 - 160 ms a bake in 143, no memory.
   A far tree shows its kind's state at the forest's temperature, without
   the near trees' own ±1.5 °C shift. The phone bakes its one set the same
   way.
7. **Ground palette:** one per-frame grass/ground/tuft palette (replacing
   the hand-kept constant pairs), dry grass by moisture.
   Done (2026-10-31): app3d/GroundPalette (groundPaletteAt: summer's
   green; rising, the dead winter grass to fresh green from 1 to 7 °C and
   on to summer's by 14 °C; falling, back to the dead grass from 5 to
   -2 °C; straw on dry soil from 15 to 22 °C, at most 0.75). WalkerTerrain
   holds it, set with the climate each frame. The terrain gets it in its
   draw record ([33..35], [40..43]; GbufferTerrain
   terrainSetGroundPalette, terrain_splat.wgsl), the GPU grass in its
   uniform (GrassViewSettings.dryColor, dryAmount, vec4 j), the meadow per
   place (each place's moisture kept at planting). All three mix by the
   soil's moisture with one rule (grass_field.wgsl groundPaletteGrass; the
   Rae twin GroundPalette.groundGrassAt). walkerGroundColor and the
   walker's RAE_TERRAIN_GRASS are gone. RAE_WALKER_LATITUDE sets a
   walker's latitude for a headless frame.
8. **Snow cover:** the snow depth model; terrain snow by slope and noise;
   grass and tufts under snow; snow on trees (bark, branches, the spruce's
   tiers) by the snow amount (§4, §4.2).
   Done (2026-10-31): lib/Climate keeps a snow depth per 50 km cell,
   stepped hourly with the slow temperature: a steady precipitation
   stand-in (0.25 mm of snow an hour, about 6 mm a day) falls as snow
   below 0.5 °C, 3 mm melts per degree-hour over 0 °C, a jump seeds it by
   running the last 200 days in 3-hour steps (Helsinki: late December to
   mid March, about 38 cm at most; 68 N November to April; case 1266).
   The shaders get one depth a frame, where the camera looks
   (climateSnowAt), in the frame uniform's new scene vec4 (Gbuffer
   setGbufferSnow; 48 floats), not a per-cell blend: a view spans a few
   km of a 50 km cell. lib/snow.wgsl: the terrain whitens by depth, slope
   (none on steep ground) and a noise edge, never on water; the static
   meshes and the far billboards lay it on what faces up (branches,
   trunks, the spruce's tiers, rocks, roofs; the lab's flat ground); the
   GPU grass and the meadow's blades flatten and sink below the ground,
   gone under 10 cm. The panel's temperature line and the lab's slider
   add "snow 35 cm".
9. **Deep snow mesh and footprints:** the fine snow mesh near the walker,
   the raised far terrain, the trample map stamped by every foot, feet
   standing in the snow (§4.1).
   Done (2026-10-31): app3d/SnowField. From 5 cm the snow is 5 x 5 tiles
   of 9 m (45 m) round the camera's focus, a vertex every 12.5 cm, laid on
   the terrain mesh's own triangles of its 0.75 m lattice, raised by the
   depth times a soft drift (0.75 - 1.25, even at the window's edge), none
   on water; drawn through the static pipeline (deferred and forward), the
   meshes uploaded at the first deep snow. Past the window the terrain's
   vertex shader raises the ground by the depth (the frame uniform's
   scene.yzw: the window's center and half side). The trample map is per
   tile vertex (not a separate texture): every touch of app3d/Footprints
   (the walker's and the crowd's) presses an ellipse to a quarter of the
   depth (a run to a sixth, never deeper than 45 cm) with a 3 cm rim; the
   depth's rise is taken off every print. Characters are drawn standing on
   the pressed snow (ThirdPersonController.standLift; the physics stays on
   the terrain) and slowed (WalkerPhysics.snowSpeedLimit: 4.6 m/s at 5 cm
   to 2.6 at the knee). The renderer's resource table grew from 1024 to
   4096 slots: 143 was at the edge, and a full table draws a black frame
   with no error. RAE_WALKER_AUTOWALK / RAE_WALKER_FRAMES film a walk
   (app3d/WalkerAutoWalk). Case 1267.
10. **Ice:** the freeze per lake and shore, the ice look, walkable ice
    (§4.3).
    Done (2026-10-31): lib/Climate keeps ClimateIce (lake, sea) per 50 km
    cell, stepped hourly with the snow and seeded with it
    (climateWinterSeed): lakes freeze below -3 °C air (a day at -13 °C),
    the sea's shore below -8 °C at half speed, both thaw slowly above 0 °C
    (a fifth a day at +5 °C); case 1268. Not one value per lake but per
    place: a lake is the water well inside the island (all of a world with
    no island, 143), the rest is sea; the ice grows out from the shore by
    depth (water/WaterIce: a lake to 5 m at a full freeze, the sea only
    its 1.2 m fringe; waterSurface.wgsl waterIceCover the twin), so a lake
    thaws open from its deep middle. Drawn in the water shader: opaque
    white-blue ice with cracks and frosted patches, the scene's snow on it,
    no waves. Walkable: WalkerPhysics takes ice over waterIceWalkable (0.5)
    as a place to stand and holds a mover at the water's surface there
    (app3d/WalkerIceTest in RAE_WALKER_PHYSICS_TEST).
11. **Weather model:** cloud cover, precipitation, humidity and fronts;
    its effect on the sun lux, the sky light and the auto exposure.
12. **Clouds and mist from the weather:** cloud cover and darkness; mist
    from the dew point.
13. **Rain and falling snow:** near-camera particles; wet ground.
14. **Falling leaves:** leaf particles from crowns while their leaf loss
    rises, in the kind's autumn tones.
15. **Settings and lab controls:** season and weather overrides in the
    walkers' settings and the tree lab; looks-development pages for each
    step (every season of every kind, in 136, 143 and the lab).

Each step keeps the frame-time check of §8 and adds its own
looks-development page with before/after pictures.
