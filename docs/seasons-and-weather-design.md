# Seasons, temperature and weather (design)

Status: design (2026-10-30). Not built. §9 splits it into tasks.

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
  °C at the equator to about -15 °C at the poles (roughly 27 - 0.45 x
  |latitude| - 0.004 x latitude², which gives Helsinki, 60° N, about 5
  °C); the swing between summer and winter grows with latitude (about 2 °C
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

## 9. The tasks (to split into the queue)

1. **Year and place:** `TimeOfDay.yearSpeed`; the latitude at a point
   (143's north offset); the lab's `RAE_DAY_OF_YEAR` / `RAE_LATITUDE` and a
   day slider.
2. **Climate:** `climateAt` (yearly mean and swing by latitude, the day,
   height, the hemisphere), the slow temperature grid stepped hourly, a
   case for the fit (Helsinki, the equator, the southern hemisphere).
3. **Tree season states:** the kind's season block (deciduous, spring /
   autumn / winter tones, phase temperatures); the per-kind color blend and
   the per-tree phase offset; the autumn kinds folded into their species.
4. **Leaf loss:** the leaf pass's loss threshold (outer cards first), bare
   winter trees; case checks of cards kept per loss.
5. **Seasonal impostors:** three baked atlas sets (summer, autumn, bare)
   and the sprite and canopy blend; the phone's tinted single set.
6. **Ground palette:** one per-frame grass/ground/tuft palette (replacing
   the hand-kept constant pairs), dry grass by moisture.
7. **Snow:** the snow depth model; terrain snow by slope and noise; grass
   and tufts under snow; snow on tree tops and the spruce's tiers.
8. **Weather model:** cloud cover, precipitation, humidity and fronts;
   its effect on the sun lux, the sky light and the auto exposure.
9. **Clouds and mist from the weather:** cloud cover and darkness; mist
   from the dew point.
10. **Rain and falling snow:** near-camera particles; wet ground.
11. **Falling leaves:** leaf particles from crowns while their leaf loss
    rises, in the kind's autumn tones.
12. **Settings and lab controls:** season and weather overrides in the
    walkers' settings and the tree lab; looks-development pages for each
    step (every season of every kind, in 136, 143 and the lab).

Each step keeps the frame-time check of §8.
