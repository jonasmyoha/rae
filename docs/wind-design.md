# Wind on trees, bushes and grass

How the renderer's props sway in the wind (lib/Wind.rae, lib/wind.wgsl,
lib/RendererWind.rae; fixture 1232_wind). The walker examples (136, 137, 143)
use it.

**The trees' and bushes' leaves turn in the wind again (2026-10-23), with
no gusts: windLeaf uses a steady gust level, because the gust field made
everything that followed it twitch (§5). It was off on 2026-10-22
(`windLeafScale` in lib/wind.wgsl; docs/tree-wind-design.md). The grass
keeps its own wind.** Sections 3 and 4 describe an earlier version.

## 1. What the wind is

- **A global base wind.** One direction and one strength for the whole
  world. They drift slowly about the settings' own values with the time: two
  slow sine waves each, so the drift does not beat in an obvious way
  (`stepWind`, period `driftSeconds`, 90 s by default). The base has no state
  but the clock, so two machines with the same time see the same wind.
- **Local gusts, stored nowhere.** `windGustAt` in lib/wind.wgsl is smooth
  value noise over the world's xy, in cells of about 55 m, moving with the
  wind (faster when it is stronger). Where the noise is high a gust presses:
  up to `1 + 2 * gustiness` times the base on trees and bushes. A gust front
  sweeps across grass and trees together, and looks the same from every
  camera.

Why not a grid of wind areas: a grid would need state, streaming and the same
values on every machine. A noise field computed in the vertex stage is local
in the way that matters (gust fronts tens of metres across pass over the
scene) and costs a few ALU instructions per vertex. Only what is on screen
pays for it. One wind for the whole world, with no gust field, would look
mechanical: every tree in step.

The strength is in "sway units": 1 is a fresh breeze, the strength the trees
are tuned for; 0 is calm; 2 is a strong wind. `RAE_WIND=<strength>` changes
it (`windSettingsFromEnv`), for shots to compare.

## 2. Per frame

```
var wind: WindState = createWindState(settings: windSettings)
...each rendered frame:
stepWind(state: wind, settings: windSettings, deltaTime: deltaTime)
setRendererWind(renderer: renderer, wind: windVector(state: wind, settings: windSettings))
```

`WindVector` is one vec4: direction times strength (xy), gustiness, and the
time wrapped to an hour (`windTimeWrap`). The 64-bit clock keeps hours of
play exact, and the wrapped 32-bit time still resolves a quarter of a
millisecond. Once an hour a gust front jumps.

The renderer carries it in:

- the G-buffer frame uniform (`gbufferFrameFloats` 40: viewProj, prevViewProj,
  jitter, wind). The jitter's z is last frame's wind time, so the motion
  vectors (and TAA) see the sway;
- each shadow cascade's uniform (80 bytes: the light's view-projection, then
  the wind), so a swaying tree's shadow sways with it.

The forward main pass and the grass bind the same frame uniform.

## 3. What sways, and how much

**Only leaves sway. A tree's body (its trunk and branches) never moves.**
The first version bent the whole tree with its height (a lean of up to
metres at a tall crown in a gust) and it looked like a glitch: trunks
rocking, and a jump where a tree changed to its billboard. It was removed
(2026-10-22).

An instance sways by its record's float [43] (GbufferRecord
`setInstanceSway`; 0 rigid). The shadow casters copy [40..43] already, so the
shadow pass reads the same value.

- `app3d/PropField`: each `PropPart` has a `sway` (0 by default). A tree
  level's part 0 is its wood and keeps 0; its leaf parts get
  `groveLeafSway` (app3d/GroveTrees). The field keeps it per batch
  (`batchSways`).
- `app3d/PropInstancer`: its `sway` field, for props drawn one batch per mesh
  (the walker's bushes, which are all leaves).
- A rock, a building or anything else keeps 0 and does not move.

The motion (`windOffset`) is a few centimetres about where a leaf grows: a
slow sway in clusters about a metre across (each cluster its own phase), a
small cross-wind part, and a faster flutter per leaf. It grows with the
push (a gust presses harder) and a little with the height over the foot
(`h / (h + 2)`). At the default wind a leaf moves about 5 cm, in a strong
gust about 15 cm: small enough that the leaves stay on their branches.

## 4. Levels of detail and billboards

- **Near and middle mesh levels** sway the same way. The motion is a
  function of the world position and the foot, not of the mesh, so two
  levels cross-fading (fade_dither.wgsl) move together.
- **Billboards (impostors) do not sway.** The sprite pass writes a depth for
  each texel of the baked picture. Bending the quad would move its outline
  without moving that depth, and it would bend the picture's trunk too. Past
  the middle level's distance (130 m and more) a leaf's few centimetres are
  under a pixel anyway.
- **The far canopy** (app3d/FarCanopy) does not sway, for the same reason.
- At the switch from the middle level to the billboard, the leaves stop
  moving. They move a few centimetres, so at those distances nothing jumps.

## 5. Grass

The grass does NOT follow the renderer's wind (2026-10-23). It sways on its
own settings and its own clock (GrassViewSettings windDirX/Y, windStrength,
windGust; grass_render.wgsl windBend), as it did before this wind was
added.

For a while it followed the renderer's wind and its gust field, and the
blades twitched. The cause is in `windGustAt`: the gust pattern is moved by
`direction * time * (2.5 + 3.5 * strength)`, with the time up to an hour,
while the base wind's direction and strength drift (stepWind). Every small
drift is multiplied by that large time, so the whole pattern jumps metres
from one frame to the next, and everything whose strength follows it
flickers. The trees used the same field (their wind is off, see the top).
A gust field that is used again must move by an offset that ADDS UP each
frame (state on the wind, stepped with deltaTime), never one computed from
the drifting values times the total time.

## 6. Cost and mobile

All of it is vertex-stage arithmetic: two value-noise lookups and a few sines
per vertex of a swaying instance. Rigid instances return early. No new GPU
feature, no texture, no compute pass, no extra buffer beyond 16 bytes of
uniform. It runs the same on WebGPU over Metal on iOS as on the desktop. The
phone's cost is not measured yet. The vertex count of the near tree levels is
the thing to watch there: lowering the near level's distance on a phone, or
setting a kind's sway to 0 there, removes it.

### Textured birch sprays (2026-10-23)

A negative leaf flexibility selects a branch-spray response in `windLeaf`;
its magnitude controls the response. Four to six curved patches in a birch
spray share one pivot and phase. The response was increased after the first
version moved too little: spray flexibility is 0.8–1.2, angular travel is capped
at 0.42 radians, and the hardware check requires visible tip displacement.
A fixed tree phase drives a slow swell;
a fixed branch phase adds a smaller variation. This keeps the spray rigid and
rotates its shading normals with it. Zero wind keeps every vertex unchanged.
No moving noise coordinate depends on the total time times a changing wind
direction. Existing positive-flexibility species keep their current motion.
This is art-directed branch motion, not a flexible trunk or a branch simulation.
