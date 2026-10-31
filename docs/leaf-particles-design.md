# Leaf particles and weather wind (design)

Leaves that come off the trees and fly: a few in a calm autumn, a stream in
a fresh breeze, a storm of them in a gale, and now and then a few green
ones torn off in a summer storm. The wind that drives them comes from the
weather and the season, so a windy autumn day looks and feels different
from a still summer evening.

This document plans the work. The steps are queued as
"[leaves n/8]" in QUEUE.md (§9).

## 0. What exists today

- **lib/Wind.rae** (docs/wind-design.md): one base wind for the world, a
  direction and a strength in "sway units" (1 a fresh breeze, 2 strong),
  drifting about the settings' own values on two slow sine waves. The
  trees' leaves (lib/wind.wgsl `windLeaf`) and the grass
  (`grassGustEnvelope`) read it as one `WindVector`. **Nothing drives it
  from the weather or the season:** the walkers use `defaultWindSettings()`
  all year, plus `RAE_WIND`.
- **lib/Weather.rae**: cloud, precipitation, humidity and the front from a
  seed, the place and the date. No wind.
- **app3d/FallingLeaves** (seasons step 14): each deciduous tree within
  32 m that is losing its leaves lets up to 10 leaves fall in a loop: a
  CPU function of the clock builds one box instance per leaf every frame
  (at most 2,500). It has no state: a leaf starts in the crown, falls
  straight to the foot at 0.9 m/s with a sideways sine flutter, and starts
  again. It ignores the terrain (it stops at the tree's foot height) and
  it does not react to gusts beyond a constant drift. The wind it reads is
  the renderer's sway vector times 2, which is not a speed.
- **GPU compute with indirect draws** (lib/GrassCompute.rae): a compute
  pass writes draw records into a storage buffer and the G-buffer pass
  draws them with `recordDrawIndirectInPass`. The terrain height function
  exists in WGSL (lib/grass_compute.wgsl `terrainHeight`). This is the
  model for the leaves.
- **The trees' leaf loss** (lib/wind.wgsl `leafCardKept`, app3d/PropSeason
  `propSeasonLoss`): a kind's crown thins by discarding cards as its leaf
  loss rises in autumn.

## 1. Goals and limits

- **Organic, stylish, stylized.** A leaf is a small cut-out card in the
  kind's own colors, bent along its midrib, that flutters, glides side to
  side and tumbles as it falls. In a gust it is carried, lifted and
  swirled. It lands on the ground (and floats on water) and fades there.
  The look matches the trees' stylized cards, not a photographic leaf.
- **Epic sometimes.** On a windy autumn day a strong gust front sweeps
  through: a band of leaves lifts off a stand of trees at once, swirls in
  a few vortices and rains down tens of meters downwind. This is rare
  (a few times a minute in a gale, never in a calm) and is the payoff of
  the system.
- **No entity per leaf.** Leaves are not ECS entities and have no CPU
  state per leaf. The ECS owns ONE resource (the leaf system) and one
  system that runs per frame; the leaves live in a GPU buffer.
- **Cheap.** Target on the desktop: 16,384 leaves at most, under 0.15 ms of
  GPU time for the simulation and under 0.3 ms for the draw; under 0.1 ms
  of CPU per frame. On the phone and in the browser: 4,096 at most. With
  no leaves alive, nothing is dispatched or drawn.
- **Visual only.** Nothing reads a leaf back. Leaves never touch physics,
  lockstep or game state, so the GPU simulation need not be deterministic
  across machines. What IS deterministic (the wind, when and where leaves
  are released) is a function of the seed, the place and the clock, so a
  screenshot can be reproduced.

## 2. Weather wind

### 2.1 The model (lib/Weather)

`Weather` gains `windSpeed` (m/s at 10 m, the mean) and `windAngle`
(radians, the direction the wind blows TOWARD, as lib/Wind), and
`gustiness` (0 .. 1). All three are pure functions of the same inputs as
the rest of `weatherAt`:

- **A seasonal base** by latitude: the mid and high latitudes are windier
  in autumn and winter (storm season: about 1.6 times the summer mean),
  the tropics have a steadier trade wind and a calm season. The base mean
  is about 3 m/s in summer and 5 m/s in winter at 50°.
- **The front.** Wind rises as a front arrives: the speed adds a term in
  the front's rate of change (the noise's derivative, which is cheap from
  the same eased noise) and a term in the cloud cover. A passing rain
  front carries the strongest winds of the day (10 - 18 m/s gusts); a
  clear high-pressure day is light.
- **The day.** Over land the wind is stronger in the afternoon (mixing) and
  calmer at night and at dawn, so the morning mist (weatherMistFactor)
  sits in still air.
- **The direction** follows the latitude's prevailing wind (westerlies at
  the mid latitudes, trade winds from the east in the tropics) plus a slow
  noise of the front, so it veers as a front passes.
- **Gustiness** rises with the speed and in the front's passage, and falls
  in a calm.

Storms are what the front noise's high end already is (the cloud is 1, it
rains hard); they get the strongest wind. There is no separate storm
state.

### 2.2 Into lib/Wind

lib/Wind stays the per-frame wind (its drift and the shaders' vector).
The walkers and the tree lab build its `WindSettings` each frame from the
weather: `windSettingsFromWeather(weather)` sets `strength` (sway units),
`angle` and `gustiness`; the drift and swing are as today. The sway units
are mapped from the speed with one constant,
`windSwayMetersPerSecond` = 5 m/s (a fresh breeze is sway 1, a gale of
15 m/s is 3, capped at 3.5 so the trees' leaves stay on their branches).
Every consumer that wants a speed (the leaves, the rain, the snow) uses
`windMetersPerSecond(vector)` instead of today's `windX * 2.0`. `RAE_WIND`
keeps overriding the strength, for shots.

The weather changes over hours, so `WindSettings` move slowly; the drift
inside lib/Wind still varies them over 90 s.

### 2.3 Gust fronts: one field for trees, grass and leaves

A strong gust is what releases leaves and what makes the scene feel
windy, so it must be the SAME gust in the trees, the grass and the
leaves. One function, written twice (Rae and WGSL, like
`waterIceCover`, with a check that they match):

`windGustAt(x, y, time, vector) -> Float` (0 calm .. 1 the peak of a
gust): bands about 20 - 60 m wide, moving downwind at the wind speed,
spaced by a hash of their index along the wind (so they come irregularly),
each with a strength from the hash. The strong bands (the top few percent,
more often in a gale) are the **storm gusts**: they last 5 - 15 s at a
place and push up to twice the mean. The grass's `grassGustEnvelope` and
the trees' `windLeaf` take their activity from this field (so a gust is
seen crossing the grass and then the crowns), and the leaves' release and
flight read it too.

### 2.4 Controls

The walkers' settings panel and the tree lab get a wind control next to
the weather override: "auto" (from the weather), "calm", "breeze",
"strong", "gale", and `RAE_WEATHER_WIND=<name>` for shots. The panel shows
the wind as "6 m/s from the southwest" next to the weather's name.

## 3. When a tree lets a leaf go

The CPU decides how many leaves each tree near the camera releases this
frame. Per tree (a placement of a deciduous kind within the reach):

```
foliage   = 1 - loss                           # leaves still on it
seasonal  = rate the leaf loss is rising       # autumn: ripe leaves drop
windPull  = hold(kind, loss) * max(0, gust - threshold)^2
release   = foliage * (seasonal * seasonalRate + windPull * windRate)
```

- `loss` and its rising rate come from `propSeasonLoss` (the same value
  that thins the crown). A bare tree releases nothing.
- `hold(kind, loss)`: how easily the wind takes a leaf. Small in summer
  (a summer storm takes a few green leaves), large while the leaf loss
  rises (an autumn gust strips ripe leaves), so the SAME gust gives a few
  leaves in July and a cloud of them in October. Evergreens (the spruce,
  the cypress) hold theirs: `hold` is near 0, a gale takes a few needles
  at most (drawn as small dark flecks, or not at all in the first steps).
  The palm releases nothing in the first steps (its fronds are not
  leaves that fly).
- `gust` is `windGustAt` at the tree, so the release moves through the
  forest with the gust front: a band of trees lets go together.
- The count is a fraction per frame. Each tree keeps no state: the integer
  count is drawn from the fraction with a hash of the tree index and the
  frame's time step, so the expected rate is right and the release is
  reproducible.
- The leaf's color is the kind's color at its season (green in summer,
  the autumn tone, a mix while it turns), with a per-leaf shade.
- The release does NOT thin the crown further: in autumn the crown's leaf
  loss already shows the leaves that went; in summer the few leaves a
  storm takes are too few to see on the crown.

**Emitters.** The CPU writes this frame's releases into a small emit list
(at most 512 entries; each entry is a tree's crown center and radius, a
count, a color and a seed) and uploads it once. The compute pass spawns
`count` leaves per entry at random points of the crown. The same list
also carries **ground lifts** (§5.3).

The tree loop reuses the forest's cell grid round the camera (as
FallingLeaves does today): O(trees in reach), a few hundred, with nothing
per leaf.

## 4. The leaf system on the GPU

### 4.1 Ownership

`LeafParticles` is a resource owned by the app (in the walkers, on
`walkerMarks` as the falling leaves are today; in the tree lab, on its
state), created with a capacity from the quality settings. It holds the
GPU buffers, the pipelines and the CPU emit list. One system,
`stepLeafParticles`, runs per rendered frame: it fills the emit list,
uploads it and the frame's uniform, and records the compute pass;
`drawLeafParticles` draws into the G-buffer's open geometry pass. No other
code touches a leaf.

### 4.2 Buffers

- **Leaves:** a storage buffer of `capacity` records, 32 bytes each:
  position (3 floats), age (1), velocity (3 half floats packed in 2
  floats), spin phase and spin axis (packed), color (packed RGBA8), size
  and kind (packed). 16,384 leaves are 512 KB.
- **Ring:** spawning writes to slots in a ring (a head counter in a small
  storage buffer). A leaf lives at most `leafLifeSeconds` (about 20 s,
  long enough to fall from a tall crown in a gale and lie a while), so
  the oldest leaves are overwritten first and no free list is needed. A
  dead slot has age < 0 and draws nothing.
- **Draw:** an indirect draw of `capacity` instances (or of the live range
  only, if the dead slots cost too much: measured in step 8). The vertex
  shader collapses a dead leaf to nothing, as the trees' lost cards do.

### 4.3 The compute pass (one dispatch)

1. **Spawn:** the threads of the first groups read the emit list and write
   new leaves into the ring: a random point in the crown sphere, the
   kind's color, a size (6 - 12 cm), a random spin, and the wind's
   velocity at the crown as a start.
2. **Fly:** every live leaf integrates one step (the frame's time step,
   capped at 1/30 s):
   - the air velocity at the leaf: the mean wind, scaled by
     `windGustAt`, plus a curl-noise turbulence whose strength grows with
     the gust (the swirls, §5.2);
   - the leaf's own fall: it relaxes toward the air velocity with a short
     time constant sideways and falls at its terminal speed (about
     1 - 1.5 m/s) relative to the air, plus the "falling leaf" motion:
     a side-to-side glide (a pendulum in the leaf's own heading) and a
     tumble that speeds up in a gust;
   - in a strong gust the air's upward part lifts the leaf (updraft at the
     gust front's leading edge), so leaves can rise above the crowns.
3. **Land:** below the terrain height (the analytic height field, the same
   WGSL as the grass) a leaf stops, lies flat on the ground with a random
   heading, and ages out over a few seconds (its fade, §5.4). On the
   water it floats and drifts slowly downwind until it fades. On deep
   snow it lies on the snow's surface.
4. **Cull:** a leaf too far from the camera (beyond the reach plus a
   margin) dies, so a walk does not leave a trail of invisible leaves.

The pass is dispatched only when a leaf is alive or an emit is pending.

### 4.4 Drawing

- One leaf mesh: a card bent along its midrib (two quads, four
  triangles), drawn instanced through the G-buffer pass like the grass,
  with the leaf's pose built in the vertex shader from its position,
  velocity (the leaf's heading) and spin. No CPU matrix per leaf.
- The leaf's outline is a cut-out from a small leaf-shape texture (four
  shapes in one layer array: oval, lobed maple, birch, ginkgo fan), chosen
  per kind, so a maple sheds maple leaves. Alpha-tested, with the same
  dither fade as the trees.
- Two-sided: the back face uses the flipped normal and a little
  translucency, like the trees' leaf cards, so a leaf lit from behind
  glows.
- The toon look uses the trees' toon leaf ramp.
- **No shadows** in the first steps (a leaf's shadow is a few pixels). A
  later option is to add the near leaves to the nearest cascade only.
- Motion vectors from the previous position (position minus velocity
  times the time step), so TAA does not smear them.

## 5. The look

### 5.1 Calm autumn

A slow trickle from each turning tree: a leaf every few seconds per tree,
gliding side to side down to the ground under it. The current
FallingLeaves look, but with real landings and a varied, slower fall.

### 5.2 Wind and swirls

In a breeze the leaves fall at a slant and land downwind of the trees. In
a strong wind they are carried far before they land. The curl noise (a
cheap noise of three octaves, divergence free, so the leaves do not
bunch) gives eddies of a few meters behind the trees and around the
walker; its strength follows the gust, so the swirls come with the gusts.

### 5.3 The epic gust

When a storm gust (§2.3) crosses a stand in autumn:

- every tree in the band releases a burst (the `max(0, gust - threshold)^2`
  term is large only here);
- the gust front's updraft lifts them above the crowns, and one or two
  larger vortices (a few meters across, moving with the front) spin them;
- **ground lifts:** the leaves already lying in the band are lifted again.
  The ground's fallen leaves are not simulated, so a lift is an emit of a
  ground patch (an area at the walker's surroundings where the season has
  leaf litter, i.e. under deciduous trees after their fall starts) instead
  of a crown. Leaves rise from the ground and join the swirl.

The result: a wall of leaves moving through the forest every so often in
a gale, and a few leaves skittering over the ground in a breeze.

### 5.4 Fading

A leaf on the ground stays for a few seconds and fades by the dither (no
alpha blending), as a tree's level fade. The walkers' ground already
shows the season's litter in its palette, so the particles need not pile
up.

### 5.5 Summer and all year

The release formula gives a summer storm a few green leaves per tree in
the strong gusts, and nothing in a calm. Winter's bare deciduous trees
release nothing; in winter the ground lifts carry the snow instead (a
later idea: blowing snow from the same system, with a white flake card).

## 6. Quality, the phone and the browser

- The capacity, the reach and the release rate scale with the renderer's
  quality: 16,384 leaves and 40 m on the desktop's high quality, 4,096 and
  25 m on the phone and in the browser. The release rate is scaled with
  the capacity so a gale fills the ring instead of overflowing it (the
  ring overwrites the oldest leaves, so an overflow shows as leaves
  vanishing in the air: the emit side caps the per-frame total to
  `capacity / leafLifeSeconds * frameTime * 2`).
- The work is one compute dispatch and one indirect draw. Both run on
  WebGPU over Metal on iOS and in the browser, as the grass does.
- Measured with the GPU timing (lib/GpuTiming) in step 8, and recorded
  here.

## 7. Who owns what (ECS, no globals)

- `Weather` (its wind) is computed from the climate's seed and the place,
  as today, by the walkers' frame and the tree lab.
- The walkers' `renderSystem.wind` and `renderSystem.windSettings`
  (app3d/WalkerRenderWalk steps them) take their settings from the weather
  each frame; the tree lab's own pair does the same.
- `LeafParticles` is one resource on the app, threaded through `mod`
  parameters to `stepLeafParticles` and `drawLeafParticles`.
- No module-level state; no entity per leaf.

## 8. Tests

- A Rae case for the weather wind: the seasonal base (autumn windier than
  summer at 50°), a front's passage raises the speed, the night is calmer,
  the direction is the prevailing one, the same inputs give the same wind.
- A case for `windGustAt`: bands move downwind at the wind's speed, a calm
  has no storm gust, a gale has some; and a hardware check that the WGSL
  function gives the same values as the Rae one at a list of points.
- A case for the release: zero for an evergreen, a bare tree and a calm
  summer; a few in a summer storm; many in an autumn gust; the expected
  count over many frames matches the rate.
- A hardware check (checks/) for the leaf system: spawn a burst, step it,
  read back the buffer once (the check only) and see that the leaves fall,
  drift downwind, land on the terrain height and die after their life.
- Pictures and a video in looks-development: a calm autumn, a breeze, an
  epic gust, a summer storm.

## 9. The steps (queued as "[leaves n/8]")

1. **Weather wind** (§2.1, §2.2): `Weather` wind fields, the seasonal and
   front model, `windSettingsFromWeather`, `windMetersPerSecond`; the
   walkers and the tree lab drive lib/Wind from the weather; the rain, the
   snow and the current FallingLeaves use m/s. Case.
2. **Gust fronts** (§2.3): `windGustAt` in Rae and WGSL; the grass and the
   trees' leaves take their gusts from it. Case and hardware check.
3. **Wind controls** (§2.4): the settings panel's and the tree lab's wind
   override and its env variable; the panel's wind text.
4. **GPU leaf system core** (§4): `LeafParticles`, the ring buffer, the
   emit list, the compute pass (spawn, a simple fall, terrain landing,
   cull), the instanced indirect draw of a bent card. Replaces
   app3d/FallingLeaves in the walkers and the tree lab with the same
   release as today. Hardware check.
5. **Release model** (§3): per-kind `hold`, the seasonal and wind terms,
   summer storms, evergreens, the per-frame cap. Case.
6. **Flight and landing look** (§4.3, §5.1, §5.2, §5.4): glide, tumble,
   curl-noise swirls, updraft, water floating, snow surface, dither fade,
   the leaf-shape textures per kind, two-sided lighting, motion vectors.
   Looks-development round.
7. **The epic gust** (§5.3): storm-gust bursts, the vortices that travel
   with the front, the ground lifts. Looks-development video.
8. **Quality and cost** (§6): capacity and reach per quality, the release
   cap, the GPU timing on the desktop (and the phone if one is at hand),
   the numbers written into §6.
