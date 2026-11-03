# Cloud rendering: what the engines and the published work do, and a tiered plan for Rae

Research for clouds 2/4 (2026-11-03). It covers how Unreal, Unity and Godot
draw clouds, the talks behind them (Horizon Zero Dawn / Nubis, Frostbite,
Red Dead Redemption 2), what each costs per frame on desktop, consoles and
phones, and how each scales down. It ends with a proposal for Rae: one
cloud interface fed by `lib/Weather`, with three tiers behind it (a 2D
layer, a 2.5D slab, a full volume), chosen per game and per device.

This is research only. No code changed. Every number below comes from the
source linked next to it. Where no source gives a number (most of all for
iPhones), the text says so and gives an estimate, marked as one.

Contents:

1. What Rae has today
2. The published work (the talks every engine builds on)
3. Unreal Engine
4. Unity
5. Godot
6. Other cheap approaches that games ship
7. Costs side by side, and what a phone can afford
8. Proposal for Rae: tiers behind one weather interface
9. The steps to build it
10. Sources

---

## 1. What Rae has today

- `lib/Weather.rae` gives, for a place and an hour: `cloud` (cover, 0..1),
  `precipitation` (mm/h), `humidity`, `front`, `storm` (0..1), and the wind
  (`windSpeed`, `windAngle`, `gustiness`). `weatherCloudDarkness` makes the
  clouds dark from rain and storm, not from cover (clouds 1/4).
- `lib/sky_hosek.wgsl` `skyClouds` draws the clouds in the sky pass, per
  pixel. It projects the view ray onto a plane overhead, adds three octaves
  of 2D value noise, and thresholds the result by cover. The color is the
  sky's own brightness times a shade from the darkness. It does not use the
  sun's direction, and it has no cloud types and no height.
- `lib/app3d/SettingsSky.rae` dims the sun (`settingsCloudSunLight`, down
  to 20% at full cover) and the sky light (`settingsCloudSkyLight`) by the
  cover. There is no cloud shadow on the ground.
- The renderer has what a volumetric tier would need: compute passes, 3D
  textures (`WGPUTextureDimension_3D` in the generated bindings), TAA with a
  jitter, a dynamic render scale (`lib/app3d/RenderScale.rae`), per-pass GPU
  timing (`lib/RendererPassTiming.rae`) and the thermal state on Apple
  (`rae_ext_thermalState`). `SettingsQuality { low, mid, high, ultra }`
  exists, but today only `ultra` changes anything.

So Rae today has a "tier 0.5": a 2D noise layer that knows the cover and
the darkness, and nothing else.

---

## 2. The published work

All three engines build on the same small set of talks. Read these first.

### 2.1 Horizon Zero Dawn, 2015 (Schneider, Guerrilla)

"The Real-time Volumetric Cloudscapes of Horizon: Zero Dawn", SIGGRAPH
2015, Advances in Real-Time Rendering. This talk set the pattern that
almost every game since uses.

**Cloud field.**
- The clouds are a layer between two altitudes, around a curved planet.
- A **weather texture** (2D, seen from above) says where clouds are
  (coverage), whether it rains, and what type they are.
- A **height gradient per cloud type** (stratus, stratocumulus, cumulus)
  gives the vertical shape: a flat base, a rounded top, and how tall.
- Two tiling **3D noise textures** carve the shape:
  - a 128³, 4-channel Perlin-Worley texture for the base shape;
  - a 32³, 3-channel Worley texture that erodes the edges.
- A 128², 3-channel 2D curl noise distorts the edges into wisps.
- Everything totals about 20 MB of memory.

**Lighting.**
- **Beer's law** for the light that gets through.
- A **Henyey-Greenstein** phase function for the forward scattering. This
  is the silver lining when you look toward the sun.
- A **"powder" term** that darkens the edges facing the light (the
  "powdered sugar" look). It stands in for multiple scattering.
- **Ambient** light from the sky that grows with height in the cloud.
- **Light samples:** 6 per view sample, in a cone toward the sun. The last
  sample is taken far away, so that distant clouds cast shadows.

**Ray march.**
- 64 to 128 view samples per ray.
- Cheap, large steps until the ray meets a cloud, then full-detail steps.
  After several empty samples the march goes back to cheap steps.

**Cost.** Done the plain way, all of this cost about **20 ms** on a PS4. It
fit in about **2 ms** because of one idea: draw into a quarter-resolution
buffer, and each frame update only **1 pixel of every 4×4 block**. The
other 15 pixels are reprojected from earlier frames. Where reprojection
fails (at the screen edge), a low-resolution buffer fills in. Schneider
says this made the shader "10x faster or more".

### 2.2 Nubis, 2017 (Schneider, Guerrilla)

"Nubis: Authoring Real-Time Volumetric Cloudscapes with the Decima
Engine", SIGGRAPH 2017 Advances.

**The cloud map.**
- An RGB texture covering about 100 km².
- R and G hold coverage. B holds the type: 0 is stratus, 0.5 is
  stratocumulus, 1 is cumulus.
- Low-frequency noise makes the large regions where air masses meet;
  higher-frequency noise breaks them into cloud footprints.
- Perlin noise gives connected formations. Perlin-Worley gives separate,
  island-like clouds.
- The weather state ("cloudy", "cloudy, windy") sets skew, anvil shapes and
  movement speed.

**The silver lining.** Two HG lobes, combined with a `max`:
`max(HG(cos θ, eccentricity), silverIntensity × HG(cos θ, 0.99 − silverSpread))`.
Beer's law is also two exponentials combined with `max`, so a thick
cloud's inside does not go black.

**Cost on a PS4**, for clouds covering half the screen:

| Optimization (each added to the ones above) | Cost |
|---|---|
| Temporal reprojection only | 22 ms |
| Adaptive step size and noise level of detail | 8.1 ms |
| Light samples only where density is above zero | 3.34 ms |
| Cull the part of the cloud sphere below the horizon | 1.81 ms |
| Start the march only where the sky may be visible (a conservative depth test) | **1.2 ms** |

The march takes 54 to 96 samples, depending on whether the ray points up or
toward the horizon.

### 2.3 Nubis Evolved, 2022, and Nubis³, 2023 (Horizon Forbidden West)

"Nubis, Evolved" (SIGGRAPH 2022 Advances) and "Nubis Cubed: Methods (and
madness) to model and render immersive real-time voxel-based clouds"
(SIGGRAPH 2023 Advances).

**2022: the vertical-profile model.**
- `dimensionalProfile = verticalProfile × coverage`, then
  `density = saturate(noiseComposite − (1 − dimensionalProfile))`.
- Ambient scattering is `pow(1 − dimensionalProfile, 0.5)`. That is a
  cheap ambient occlusion from the cloud's own shape.
- The talk calls the 2015 clouds **2.5D**: they look 3D, but their shape is
  stored as 2D texture data plus a height profile, not as voxels.

**Scaling between the two consoles:**

| | PS4 | PS5 |
|---|---|---|
| Maximum resolution | 960×540 | 1920×1080 |
| Light samples | 6 | 10 |
| View samples | 60–90 | 96–180 |
| Blur scale | 2× | 1× |
| Noise mip level | 1 | 0 |
| Cost | ≤ 4 ms | ≤ 2–3 ms |

An "envelope" ray-march optimization took one scene from 4.2 ms to 1.3 ms.

**2023: voxel clouds** (the Burning Shores storm you fly through).
- The clouds are stored as voxel data, compressed as a signed distance
  field (SDF), with about 0.5 m precision.
- Sampling a dense grid directly cost more than 30 ms. The compressed SDF,
  with adaptive and jittered steps, costs **2.2 to 4 ms**:
  - 4 ms in the 30 Hz mode at 960×540;
  - 2.1 ms in the 60 Hz modes at 960×540 and 480×270.
- Memory grows fast with grid size. A 512×512×64 grid is about 34 MB, and
  1024×1024×128 is about 268 MB.
- Guerrilla published a sample "Nubis Voxel Cloud Pack" (cloud data plus
  the noise generator).

For Rae this is the outer limit: hand-authored, fly-through clouds. It is
not needed for a sky seen from the ground.

### 2.4 Frostbite, 2016 (Hillaire, EA)

"Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite",
SIGGRAPH 2016 Physically Based Shading course.

**Shape.** The same layer, weather texture and noise as Schneider 2015.

**Lighting, made physically based:**
- Real scattering and absorption coefficients.
- An energy-conserving integration of scattering along each step. This is
  correct with 21 samples, where the naive form needs 512 to converge.
- A two-lobe phase function.
- **Multiple scattering as several "octaves" of single scattering**
  (Wrenninge). Each octave has less extinction and a more even phase
  function, so light goes deeper and the cloud looks brighter and softer.

**Ambient.** Rendered from the sky alone (no sun disc) into a 64×64
buffer, then averaged into one ambient color.

**Aerial perspective.** Applied once per pixel, at a depth averaged by
transmittance, so that thickening weather still matches the haze.

**Cost on Xbox One.** The clouds are drawn at half resolution (640×360 for
a 720p frame) with temporal reprojection:

| | 720p | 900p |
|---|---|---|
| Hemisphere sampling (scattering and coverage) | 0.090 ms | 0.090 ms |
| Main view | 0.720 ms | 1.090 ms |
| Planar reflection (optional) | 0.035 ms | 0.048 ms |
| **Total** | **0.835 ms** | **1.228 ms** |

### 2.5 Red Dead Redemption 2, 2019 (Bauer, Rockstar)

"Creating the Atmospheric World of Red Dead Redemption 2: A Complete and
Integrated Solution", SIGGRAPH 2019 Advances. This is the best published
example of clouds driven by **weather**, together with fog and rain, at
30 fps on PS4 and Xbox One.

**The cloud map.**
- One 512×512 R16G16 texture covers the 32×32 km world. R and G are two
  cloud layers.
- It is built each frame from masks, noise, weather and time-of-day
  parameters.

**The height LUT.**
- A 1×128 RGBA8 strip gives each weather its height profile: cumulus,
  altocumulus, stratus.
- It is copied from an atlas, so two weathers can be blended during a
  transition.
- Its third channel sets how soft the noise is.

**Detail.** One 3D Perlin-Worley noise lookup, displaced by two 2D samples
and the wind. The base shape is a Hermite (`smoothstep`) of the map and the
LUT; the detail is carved with an inverse lerp.

**Rain fog.** Fog under raining clouds, placed from the cloud map, with a
fall-off by height. A separate 512×512 fog map (8×8 km) holds the start
height, the fall-off and the density, for things like morning mist over
water.

**Lighting.**
- Single scattering, plus **two light octaves** to approximate multiple
  scattering (Wrenninge).
- One extra transmittance sample toward the sun or moon on cloud samples.
  The weather sets its distance (large, small or flat clouds).
- Ambient: the sky (Rayleigh and Mie) is rendered into a small paraboloid
  texture and reduced to an average weighted toward the sun.
- Lightning: each strike adds a point light to the march.

**Cloud shadows.** A 768×768 R16F exponential shadow map, made by marching
the clouds from the sun. It is blurred with 6 mips, and only **1 ray per
4×4 tile** is updated each frame. Separately, a 128×128 terrain shadow map
is marched over the height field.

**Resolution.** The march runs at half resolution and casts only **1 ray
in each 2×2 tile**. The other three pixels are reprojected from earlier
frames and clamped to the 3×3 neighborhood's color range. The result is
upsampled with blue-noise dither and TAA.

**Cost on PS4** (all the volumetrics, fog included):

| Pass | Cost |
|---|---|
| Cloud and fog maps | 0.04 ms |
| Cloud shadow map | 0.156 ms |
| Shadow volume | 0.204 ms |
| Material volume | 0.33 ms |
| Scattering volume | 0.34 ms |
| Ray march | 1.31 ms |
| Reconstruction | 0.232 ms |

About **2.6 ms** in all, partly on async compute. Bauer says the cost
depends most on the data: long rays are the worst case, so the cover and
the fog that artists set matter as much as the code.

### 2.6 The multiple-scattering approximation

Clouds have an albedo close to 1: light bounces hundreds of times before
it leaves. Single scattering alone makes clouds too dark and "dusty".

Every engine above approximates the bounces with Wrenninge's octaves
(Wrenninge et al., "Oz: The Great and Volumetric", 2013). Octave `n` uses
extinction × aⁿ, scattering × bⁿ and phase eccentricity × cⁿ, with a, b
and c below 1. One or two octaves is enough:
- Unreal recommends one octave for games.
- Rockstar ships two.

Schneider's "powder" darkening and Hillaire's "lower the albedo at the
edges" both fake the one effect the octaves miss: the dark edges facing
the light.

---

## 3. Unreal Engine

### 3.1 Volumetric Cloud (UE 4.26 and later, UE5)

Source: Hillaire, "Physically Based and Scalable Atmospheres in Unreal
Engine", SIGGRAPH 2020 PBS course, and the Volumetric Cloud docs.

**Cloud field.**
- A layer between "Layer Bottom" and "Layer Height" (km), around the
  planet.
- The shape is **a volume material graph**, not fixed noise. Artists
  combine a weather/cloud map, height gradients and 3D noise however they
  like, and could make tornadoes or any other shape. The default material
  follows the Schneider pattern.

**Lighting.**
- Wrenninge octaves: 0 to 2. Zero means single scattering only; Epic
  recommends one for games.
- A two-lobe HG phase function (G, G2, a blend).
- An optional secondary trace toward the ground, for light bounced up from
  below.
- The atmosphere's transmittance, either per step (better sunsets) or once
  per pixel.

**Cloud shadows**, two kinds:
- **Ray-marched self-shadow**: sharp and colored, but a limited distance.
- **Beer shadow map** (BSM): Hillaire's improvement on an exponential
  shadow map. It stores the front depth, the mean extinction and the
  maximum optical depth, so a shadow does not darken forever with distance.
  It is cascaded, reaches far, and the docs say it is "usually enough for
  clouds viewed from the ground".

The directional light can cast the cloud shadow onto the surfaces and the
atmosphere (light shafts): "Cast Cloud Shadows", strength, extent in km, a
resolution scale capped by `r.VolumetricCloud.ShadowMap.MaxResolution`.

**Sky light.** "Cloud Ambient Occlusion" dims the sky light under the
clouds, from the same kind of map.

**Resolution and reconstruction** (`r.VolumetricRenderTarget.Mode`):
- **Mode 0** traces at quarter resolution, reconstructs at half, and
  upsamples. It is the recommended setting, and supports flying through
  the clouds.
- **Mode 1** traces at half resolution. It is meant for ground views and
  costs more.
- **Mode 2** is fast and sharp, but cannot intersect opaque meshes.

Hillaire compares the two shipped schemes this follows:
- Schneider updates 1 pixel per 4×4 tile. It looks full resolution, but has
  16 frames of latency.
- Bauer traces at quarter resolution with TAA upsampling. The result is
  instant, but blurrier.

**Scaling down.**
- Per-view sample-count scales, capped by
  `r.VolumetricCloud.ViewRaySampleMaxCount` and the related variables.
- "Stop Tracing Transmittance Threshold" ends a ray early when the cloud in
  front is opaque enough.
- "Conservative Density" skips empty space.
- A grayscale material, no ground contribution, and shadow maps instead of
  ray-marched shadows all reduce cost.
- Device profiles set these values per platform.

**Mobile: not supported.** The platform table in Epic's docs lists
Volumetric Cloud as "NO" on mobile. It works on XB1/PS4 with caveats (use
the shadow map, not secondary tracing). Hillaire's 2020 slides say it
plainly: "Only cloud rendering is not reasonably achievable on mobile. For
now."

### 3.2 Sky Atmosphere, and why it does scale to phones

Source: Hillaire, "A Scalable and Production Ready Sky and Atmosphere
Rendering Technique", EGSR 2020.

The sky (not the clouds) is computed into small lookup textures: the
transmittance, a multiple-scattering LUT, a sky-view LUT and an
aerial-perspective volume. Its cost does not depend on the screen
resolution. It is scaled by the LUT resolution and the sample count, and
the paper targets everything from an iPhone 6s up to high-end PCs.

It is supported on mobile through a sky-dome mesh. Its LUTs can also hold
the cloud shadow on the atmosphere (the "SkyView LUT with shadows" option
in the 2020 slides).

So in Unreal the atmosphere scales down to phones and the volumetric
clouds do not. On mobile, Unreal games fall back to a painted or 2D
cloud dome.

### 3.3 Plugins and community

- **Ultra Dynamic Sky** (Fab marketplace) is the sky and weather system
  most Unreal games buy. Its sky modes are volumetric clouds (on top of
  Volumetric Cloud), "2D dynamic clouds" (panning 2D textures) and "static
  clouds" (a texture made to look like the volumetric ones, at a much lower
  cost). When the project uses the mobile renderer, the modes mobile does
  not support (the volumetric clouds) are replaced with compatible ones,
  set in a Mobile category. So the shipped Unreal answer for phones is
  exactly a tier fallback.
  <https://www.fab.com/listings/84fda27a-c79f-49c9-8458-82401fb37cfb>
- An 80.lv article ("Creating Volumetric Clouds for Mobile in Unreal
  Engine 5") gives one real phone measurement. A custom ray-marched cloud
  shader with 64 steps took about **300 ms a frame on an Adreno 540**
  (a 2017 phone GPU). It came down to about **40 ms**, and the author found
  that a lower resolution was the most effective cut. The point is not the
  exact number. It is that a naive desktop-style march is two orders of
  magnitude over a phone's budget.
- GDC 2026: "Demystifying the Clouds of Borderlands 4" (Luebbert,
  Disbelief) covers adapting UE5 volumetric clouds to an open world: art
  direction, dynamics and performance. Only the session listing is public.

---

## 4. Unity

### 4.1 HDRP Volumetric Clouds

Source: the HDRP manual, Volumetric Clouds override.

**Cloud field.** Three control modes:
- **Simple**: presets Sparse, Cloudy, Overcast and Storm Clouds, plus
  curves for density, erosion and ambient occlusion.
- **Advanced**: one map per cloud type (cumulus, altostratus,
  cumulonimbus, rain).
- **Manual**: the game supplies a **cloud map** and a **cloud LUT**:
  - the cloud map's R is coverage, G is rain (how dark an area is), and B
    is the type, an index into the LUT;
  - the LUT's R is coverage by height, G is how much erosion shapes the
    cloud, and B is ambient occlusion.

The shape is large-scale noise, then a smaller "erosion" noise on the
edges: Schneider's pattern.

**Lighting.**
- A multiple-scattering factor, a powder-effect intensity, erosion
  occlusion and a scattering tint.
- An ambient light-probe dimmer and a sun-light dimmer.

**Wind.** A global horizontal wind speed and orientation, with separate
multipliers for the map, the shape and the erosion, and vertical wind
for the shape.

**Shadows.** A cloud shadow over an area around the camera, with a
resolution, a distance and an opacity.

**Quality.**
- "Num Primary Steps" and "Num Light Steps" are the march sample counts.
- A temporal accumulation factor (less noise, more ghosting) and ghosting
  reduction.
- The cloud map's resolution: lower is faster, but transitions between
  cloud types are less precise.

**Platforms.** HDRP targets consoles and desktop GPUs. It does not run on
phones, so these clouds are never a mobile option.

### 4.2 HDRP Cloud Layer: Unity's own cheap tier

A 2D cloud map drawn on the sky, one or two layers. Its lighting is useful
for Rae:
- An optional **2D ray march across the cloud map toward the sun**
  approximates self-shadowing, with a step count.
- Its result is **baked into a texture, and recomputed only when a
  relevant parameter changes**, not every frame.
- It can **cast its shadow as the sun light's cookie**, a projected
  texture on everything the sun lights. That is a ground shadow for the
  price of one texture lookup.
- Wind is either one direction or a flow map.

This is the best shipped model of a lit, cheap 2D tier.

### 4.3 URP (the pipeline that runs on phones)

URP has **no built-in clouds**. Games that ship on URP, which covers most
mobile Unity games, buy an Asset Store system:

- **Enviro 3 – Sky and Weather**: ray-marched clouds plus 2D and flat
  layers. The author recommends Enviro 3's volumetric clouds on mobile
  "only for newer faster mobile devices".
- **COZY: Stylized Weather 3**: a stylized sky and weather system. Its
  default clouds are painted or 2D, and its volumetric clouds come as a
  separate add-on, **COZY: Plume**.
- **Altos – Sky and Clouds for URP**: "volumetric clouds, 2D clouds,
  skybox clouds" in one package. That is exactly the tier list this
  document proposes.
- **Sky Master ULTIMATE**: volumetric clouds and weather. One user reports
  good mobile performance on a Samsung S8 (a single report).
- **UniStorm**, **InfiniCLOUD** (it claims both mobile and desktop) and
  others.

The pattern across all of them: **every mobile-capable system ships a 2D
or skybox cloud mode next to its volumetric one**, and the volumetric mode
on phones is "newer devices only". None of them publishes per-frame
costs on phones.

---

## 5. Godot

**Built in.** Godot 4 has **no cloud system**. A `Sky` with a sky shader
draws the background:
- the shader can run in a **half-resolution or quarter-resolution
  subpass** (`render_mode use_half_res_pass` / `use_quarter_res_pass`,
  read back as `HALF_RES_COLOR` / `QUARTER_RES_COLOR`);
- it also renders into the **radiance cubemap** that lights the scene;
- the `Sky` resource's process mode (Automatic, Quality, Incremental,
  Realtime) sets how often that cubemap updates.

The docs advise drawing a slow sky into the cubemap and sampling it, so
that the full-screen pass stays cheap. Clouds are something you write in
`sky()`. The official `ProceduralSkyMaterial` and `PhysicalSkyMaterial`
have none.

**Community.**
- **clayjohn/godot-volumetric-cloud-demo** (and v2), by a Godot rendering
  maintainer. It shows volumetric clouds written in a sky shader, using the
  subpasses and the cubemap to keep the cost low.
- **Bonkahe/SunshineClouds2** (MIT, Godot 4.5 and later): a ray-marched
  cloud system you can fly through. It occludes world geometry, so it works
  as volumetric fog when the camera is inside a cloud. Version 1 was
  "SunshineClouds".
- **gdquest-demos/godot-4-stylized-sky**: a stylized sky with animated
  clouds that uses the half-resolution pass.
- Godot Shaders "AAA Fake Volumetric Clouds", the Mahamaya Sky Shader and
  similar shaders. These are 2D or thin-slab tricks in a sky shader.

So Godot's "engine answer" is the subpass-and-cubemap structure. It is a
good structure for Rae's cheap tiers: the clouds change slowly, so they do
not need to be redrawn at full resolution every frame.

---

## 6. Other cheap approaches that games ship

- **Painted or HDRI cloud domes with flow maps.** This is what Frostbite
  had before 2016: panoramic textures with "visual flow" (Guerrette,
  "Moving the Heavens", GDC 2014, about the skies of The Last of Us), plus
  translated planar layers. They are very cheap but static: they suit a
  fixed time of day and do not respond to the weather.
- **Particle and impostor clouds.** Harris, "Real-Time Cloud Rendering for
  Games", GDC 2002: clouds are made of particles, lit once, and drawn as
  impostor billboards. Clouds can be placed by hand and can come close to
  the camera.
- **Art-directed meshes without a ray march.** Sea of Thieves (Rare,
  SIGGRAPH 2018 talk "The Technical Art of Sea of Thieves") draws
  "three-dimensional, art-directed cloudscapes without using expensive
  raymarching techniques". The clouds are geometry with distortion maps,
  noise blending and a sharp alpha threshold in the distance. High- and
  low-pressure zones control the cover. This is a 2.5D tier with a strong
  look.
- **Sky: Children of the Light** (thatgamecompany, iOS and Android first)
  is the best-known mobile game built around clouds. Its GDC talks are
  about art direction and lighting. No cloud-rendering technique or cost is
  published, so it is a reference for the look, not for a method.

---

## 7. Costs side by side, and what a phone can afford

### 7.1 Published costs

| System | Hardware | Cloud resolution and steps | Cost per frame |
|---|---|---|---|
| HZD 2015, naive | PS4 | full res, 64–128 view samples, 6 light | ~20 ms |
| HZD 2015, shipped | PS4 | ¼ res, 1 of 16 pixels per frame | ~2 ms |
| Nubis 2017 | PS4 | 54–96 view samples, all optimizations | 1.2 ms |
| Nubis Evolved 2022 | PS4 / PS5 | 960×540 / 1080p, 60–90 / 96–180 samples | ≤ 4 / ≤ 2–3 ms |
| Nubis³ 2023 (voxels) | PS5 | 960×540 or 480×270 | 2.1–4 ms |
| Frostbite 2016 | Xbox One | ½ res (640×360 of 720p) | 0.835 ms |
| RDR2 2019 (with fog) | PS4 | ½ res, 1 of 4 rays per frame | ~2.6 ms |
| UE5 Volumetric Cloud | consoles and PC | ¼ res trace, ½ res reconstruct | not published; not supported on mobile |
| Custom UE5 shader (80.lv) | Adreno 540 phone | 64 steps → lower resolution | 300 ms → ~40 ms |

The lessons, which all of these agree on:

1. **The naive march is about 10–20 times over budget everywhere.**
   Shipping cost comes from four cuts: a lower resolution (½ or ¼),
   **temporal amortization** (1 pixel of 4 or of 16 per frame), cheap
   steps through empty space, and skipping light samples where the density
   is zero.
2. **The light samples are the expensive part.** The fast systems take 5 or
   6 light samples (or a shadow-map lookup), never a second full march.
3. **Shadows on the ground and on the sky light come from a map** (a Beer
   or exponential shadow map, or a light cookie), updated slowly
   (1 ray per 4×4 tile each frame in RDR2). They are never computed per
   pixel per frame.
4. **The cost follows the content.** Overcast skies and horizon views have
   the longest rays. A budget has to be measured on the worst weather, not
   on a clear day.

### 7.2 Phones (iPhone-class GPUs)

**No source publishes a volumetric-cloud cost on an iPhone.** Epic does not
support it, Unity's HDRP does not run there, and the URP assets say "newer
devices only" without numbers. What follows is an estimate, to be replaced
by measurements in clouds 4/4.

- A recent iPhone GPU (A17 Pro / A18 class) has raw shader throughput of
  roughly the same order as a PS4. But it has much less memory bandwidth,
  and it must hold that speed for minutes without overheating. In practice
  a phone can count on about half its peak (Rae's `RenderScale`
  controller exists because of throttling).
- A 3D-noise march is bound by bandwidth: a texture fetch for each sample,
  and a 128³ RGBA8 noise texture is 8 MB. That is the phone's weak spot.
- So the 1.2–2.6 ms console figures would become about **3–6 ms** on a
  phone at the same internal resolution (an estimate). That is too much
  for a 16.7 ms (60 fps) frame next to Rae's deferred renderer, and
  borderline for a 33 ms (30 fps) one.
- What does fit on a phone (estimate: well under 1 ms):
  - a lit **2D layer**: one or two texture lookups per pixel, lighting baked
    when the weather changes;
  - a **2.5D slab** with 8–16 steps, a 2D cloud map, a height profile and
    at most one small 3D noise. It runs at ¼ resolution with 1-in-4
    temporal amortization and is upsampled by the existing TAA.
- A full volumetric pass on a phone needs every cut at once: ¼ resolution,
  1 pixel in 16 per frame, about 32 view samples, 2–3 light samples, a
  shadow map and no ground bounce. Even then it belongs only on the
  newest devices, behind a measured fallback, as the URP assets ship it.
- The browser target (WebGPU) follows the device it runs on, and needs the
  same automatic fallback.

---

## 8. Proposal for Rae: tiers behind one weather interface

### 8.1 The tiers

| Tier | Name | What it is | Cost target (estimate) | Default on |
|---|---|---|---|---|
| 0 | `off` | No clouds; the weather still dims the sun and the sky | 0 | headless and tests |
| 1 | `layer` | Today's `skyClouds` grown up: a 2D cloud map in the sky pass, with types, sun-lit edges and a horizon band | < 0.3 ms phone | every phone, the fallback everywhere |
| 2 | `slab` | 2.5D: a short ray march through a thin cloud slab, from the same cloud map and a height profile, at ¼ resolution with temporal reuse | < 1 ms phone, < 0.5 ms Mac | desktops, newer phones |
| 3 | `volumetric` | The Schneider / Hillaire volume: 3D noise, 1–2 scattering octaves, a Beer shadow map, ¼ resolution, 1-in-16 temporal update | ≤ 2 ms Mac | desktops that measure fast enough |

Why these three:
- Tier 1 is the HDRP Cloud Layer and Godot's sky-shader clouds.
- Tier 2 is Guerrilla's own name for HZD's clouds ("2.5D"), cut down to
  phone size.
- Tier 3 is what every desktop engine ships.

All three draw the **same cloud field**, so changing tier changes the
detail, not where the clouds are or what weather they show. That is the
property that makes a fallback invisible in play.

### 8.2 The one interface: `CloudField` from `lib/Weather`

Each frame, one function turns the `Weather` (plus the place, the hour and
the app's tuning) into a small value that every tier reads. For example,
`weatherClouds(weather:, ...) ret CloudField`. The tiers do not read
`Weather` directly.

| Field | Meaning | Where it comes from | Tier 1 | Tier 2 | Tier 3 |
|---|---|---|---|---|---|
| `cover` | 0..1 share of sky covered | `Weather.cloud` (exists) | yes | yes | yes |
| `darkness` | 0..1 how dark the bases are | `weatherCloudDarkness` (exists, clouds 1/4) | yes | yes | yes |
| `storm` | 0..1 thunder day: anvils, taller towers, lightning | `Weather.storm` (exists) | tint only | height | shape and lightning |
| `type` | 0 stratus, 0.5 stratocumulus, 1 cumulus (Nubis's blue channel) | new: from cover, humidity, the hour (cumulus grows in the afternoon) and the season | shape of the noise | height profile | height profile and erosion |
| `cirrus` | 0..1 thin high streaks, a separate high layer | new: from the front approaching (cirrus comes before a warm front) | yes | yes (as a 2D layer) | yes (as a 2D layer) |
| `baseHeight`, `thickness` | the slab's bottom and depth, in meters | new: from the type (stratus low and thin, cumulus about 1–2 km tall, a storm taller) | parallax only | yes | yes |
| `windVelocity` | the drift of the cloud map, m/s and direction | `Weather.windSpeed` and `windAngle` (exist), scaled for the cloud height | yes | yes | yes, plus skew |
| `rain` | 0..1 where rain falls under the clouds (rain shafts and rain fog, as in RDR2) | `Weather.precipitation` (exists) | darker base | rain shafts | rain shafts |
| `seed`, `offset` | the deterministic map position | the weather seed and the drift over time | yes | yes | yes |

The light comes from the `Sky` the clouds already sit in:
- the sun's direction and color;
- the sky's average ambient color (rendered small and averaged once a
  frame or slower, as in Frostbite and RDR2);
- the moon at night.

The field also feeds one shared output: the **cloud shadow map**, a 2D
texture of how much sun gets through, seen from the sun. It replaces
today's uniform `settingsCloudSunLight` dimming:
- the deferred lighting multiplies the sun by it at each point on the
  ground, so cloud shadows move across the land;
- the same map's average dims the sky light, as Unreal's "Cloud Ambient
  Occlusion" does.

Tier 1 makes it by projecting the cloud map along the sun (as Unity's
light cookie does), and tier 3 by a Beer shadow map. The ground shading
code reads one texture, whatever the tier.

This keeps the rule from clouds 1/4: **cover makes the sky gray-white, and
darkness comes from rain and storm.** Each tier implements that rule with
its own means: a shade in tier 1, extinction and the darkness-scaled
albedo in tiers 2 and 3.

### 8.3 Lighting in each tier (what the research says to keep)

- **All tiers:**
  - a silver lining from Nubis's two-lobe HG (`max` of a broad and a
    narrow forward lobe), so clouds toward the sun glow at the edges;
  - a gray-white gradient from base to top;
  - ambient from the sky;
  - the aerial-perspective haze applied once at the cloud's depth.
- **Tier 1:** a 2D march of 4–8 steps across the cloud map toward the
  sun, for self-shadow (HDRP Cloud Layer). It is baked into a small
  texture and redone only when the sun moves noticeably or the weather
  changes. Nothing is done per pixel per frame except the lookups.
- **Tier 2:**
  - 8–16 view steps through the slab;
  - the light from 2–3 steps toward the sun in the 2D map, plus the
    height profile;
  - the powder darkening on the sunward edges;
  - one Wrenninge octave.
- **Tier 3:**
  - 32–96 view steps (by quality), 4–6 light samples in a cone;
  - 1–2 Wrenninge octaves;
  - Hillaire's energy-conserving step integration (correct at low sample
    counts);
  - a Beer shadow map for long-distance shadows and the ground;
  - an optional ground-bounce term on `ultra` only.

### 8.4 Choosing the tier: per game, per device, at run time

There are three layers of choice. Each can only lower the result of the
one before it.

1. **The game's ceiling.** The game declares the highest tier it wants,
   plus its look:
   - a stylized game may cap at `layer`;
   - a flight game may require `volumetric` and accept the cost.

   Rae's rule is to prefer default compiler behavior over a `.raepack`.
   So the recommended place is **the app's settings value**: a
   `CloudSettings` resource on the app (`maxTier`, `preferredTier`, the
   look knobs), given in code like the walkers' other settings. A
   `.raepack` field (for example `graphics { clouds: "slab" }`) should be
   added only if a build tool needs to know the tier before the program
   runs, for instance to leave the 3D noise textures out of a phone build.
   That is a design choice to confirm when tier 3 is built (clouds 4/4).
2. **The device's starting tier.** At start-up, a run-time check (never
   `when`, because the device is a run-time question):
   - a phone or tablet, or a browser on a mobile OS, starts at `layer`;
   - an Apple-silicon Mac or a desktop GPU starts at `slab`, and at
     `volumetric` if the game allows it and the user's `SettingsQuality`
     is `high` or `ultra`.

   `SettingsQuality` maps onto it: `low` → `layer`, `mid` → `slab`,
   `high`/`ultra` → the game's ceiling. This gives `low`/`mid`/`high`
   their first real job.
3. **The automatic fallback.** `RendererPassTiming` measures the cloud
   pass. When its GPU time stays over the tier's budget (for example
   1.5 ms on a phone, 2.5 ms on a desktop, over a window of 120 frames),
   or the thermal state reaches "serious", the tier steps down one level
   and logs it once:
   `[clouds] volumetric 3.4 ms > 2.5 ms budget: now slab`.
   - It steps back up only after a long good stretch, with the same
     cooldown pattern as `RenderScale`, so it never flips back and forth.
   - Because all tiers draw the same field, a step down changes detail,
     not the sky.
   - The cloud pass's budget is separate from the dynamic resolution. The
     resolution controller should not shrink the whole frame because the
     clouds alone are slow.

**Overrides for testing:** `RAE_CLOUD_TIER=off|layer|slab|volumetric`
forces a tier and turns the fallback off. The choice and its reason are
logged at start-up, so a screenshot run states which tier it shows.

### 8.5 What `lib/Weather` needs to add

Only three things. The rest exists already.

1. **`type`**: a cloud type from the existing numbers:
   - stratus under a thick front with high humidity;
   - cumulus on a fair, warm afternoon with partial cover;
   - cumulonimbus (cumulus plus a high `storm`) on a thunder day.
2. **`cirrus`**: thin high cloud ahead of a front, from the slope of the
   front noise (the front rising).
3. **`baseHeight` / `thickness`**: from the type, and the storm makes
   towers taller.

Each is a small function of numbers `Weather` already has, so the weather
stays deterministic from its seed (docs/seasons-and-weather-design.md §5).
The cloud **map** (the 2D field of where clouds are) is not part of
`Weather`. It is drawn on the GPU from the field's numbers and the seed,
as Nubis and RDR2 build theirs every frame.

---

## 9. The steps to build it

Each step is one queue task that leaves the walkers working.

1. **The shared field and the lit layer (tier 1)**: clouds 3/4, done
   (2026-11-03). The field's kind, cirrus, height and size are Weather
   fields (`cloudType`, `cirrus`, `cloudHeight`, `cloudScale`) carried to
   the shader by lib/SkyClouds (LightU `cloudShape`); the layer is
   `skyClouds` in lib/sky_hosek.wgsl. Its cost on the Mac was below the
   pass timing's noise (under about 0.05 ms); on the iPhone it is still to
   be measured.
   - Add `CloudField` and the three new weather numbers.
   - Grow `skyClouds` into the tier 1 layer: cloud types (cumulus puffs
     with flat bases, a stratus sheet, cirrus streaks), the two-lobe silver
     lining and the base-to-top gradient from the sun's direction, and the
     horizon band of cumulus.
   - Add type and height settings to the panel, bound like the weather
     sliders.
2. **Cloud shadows on the ground and the sky light**: done (2026-11-03).
   lib/CloudShadow draws a 256 x 257 RG16Float map (8 km of the clouds'
   plane round the point above the eye along the key light, snapped to a
   texel; the last row holds the average) from skyCloudSunThrough each
   frame, right after the light uniform upload, in its own submission.
   deferred_shade.wgsl cloudSunLight multiplies the sun by it (four loads,
   no sampler), cloudSkyLight dims the sky light by its average share of
   cloud, and a toon band shades only where the map is darker than its
   average. The uniform dimming in app3d/SettingsSky is gone; the clouds'
   own light and the water dim by the cover instead. Under a full cover
   143 at noon keeps its old brightness (ground gray 76 against 74, rain
   87 against 87).
   - A small 2D cloud shadow map projected along the sun from the same
     field.
   - The deferred sun light multiplies by it; its average dims the sky
     light.
   - This replaces the uniform `settingsCloudSunLight` dimming. It is
     shared by every later tier.
3. **The tier switch and the fallback**: done (2026-11-03). lib/CloudTier
   (CloudTier, CloudSettings, CloudTierControl: the start is the lowest
   of the game's maxTier / preferredTier, the device's tier, the quality's
   and cloudTierBuilt, today the layer; cloudTierFrame steps down one tier
   when 90 of a window of 120 measured frames are over budget, 1.5 ms on a
   phone and 2.5 ms on a desktop, or the thermal state is serious, logs it
   once, and steps back up after 3600 good frames, with 600 frames of
   cooldown after any change; RAE_CLOUD_TIER forces a tier). The walkers
   use it through app3d/SettingsClouds (a phone, or a browser with a coarse
   main pointer, starts at the layer; Low = layer, Mid = slab, High/Ultra =
   the game's ceiling) and hand the tier to the renderer (setCloudTier,
   LightU clouds.w). Off draws no clouds; the sky and the land are dimmed
   evenly by the cover. The measurement is the "clouds" span of
   RendererPassTiming (only while it runs, RAE_GPU_PASS_TIMING or a
   capture): the shadow map drawn as its own step before the lighting or
   forward pass. On the Mac under load that span was 0.6-0.8 ms for
   both the layer and off, its fixed cost rather than the layer's
   work (1.7 ms before a forward pass, which needs a submission of its
   own); the layer's sky drawing stays in the lighting pass.
   - `CloudSettings` (`maxTier`, `preferredTier`), the per-device starting
     tier, the `SettingsQuality` mapping, the `RendererPassTiming`
     fallback with its log line, and `RAE_CLOUD_TIER`.
   - With only tiers 0 and 1 present at first, the switch is proved before
     the expensive tiers exist.
4. **The 2.5D slab (tier 2)**: new.
   - A separate ¼-resolution pass with 1-in-4 temporal reuse, upsampled
     into the sky, from the same cloud map plus a height profile LUT (the
     RDR2 1×128 strip).
   - Measure it on the Mac and the iPhone, and compare with tier 1 in 143
     at noon, at sunset and under a front.
5. **The volumetric tier (tier 3)**: clouds 4/4, already queued.
   - Tiling Perlin-Worley 3D noise (generated once, for example with the
     method of sebh/TileableVolumeNoise), Wrenninge octaves, a Beer shadow
     map, ¼ resolution with a 1-in-16 update.
   - It plugs into the switch from step 3, and the phone falls back to
     tier 2 or 1 by measurement.
6. **Later, only if a game needs it:**
   - fly-through voxel clouds in the manner of Nubis³;
   - lightning lights inside storm clouds (RDR2);
   - rain shafts and rain fog under the clouds, from `rain`.

Steps 2, 3 and 4 are added to QUEUE.md as their own tasks, in order
around clouds 3/4 and 4/4.

---

## 10. Sources

**Talks and papers**

- Schneider, A. "The Real-time Volumetric Cloudscapes of Horizon: Zero
  Dawn." SIGGRAPH 2015 Advances in Real-Time Rendering.
  <https://advances.realtimerendering.com/s2015/index.html> (slides:
  "The Real-time Volumetric Cloudscapes of Horizon - Zero Dawn - ARTR.pdf")
- Schneider, A. "Nubis: Authoring Real-Time Volumetric Cloudscapes with
  the Decima Engine." SIGGRAPH 2017 Advances.
  <https://advances.realtimerendering.com/s2017/index.html>
- Schneider, A. "Nubis, Evolved." SIGGRAPH 2022 Advances.
  <https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-NubisEvolved-NoVideos.pdf>
  and <https://www.guerrilla-games.com/read/nubis-evolved>
- Schneider, A. "Nubis Cubed: Methods (and madness) to model and render
  immersive real-time voxel-based clouds." SIGGRAPH 2023 Advances.
  <https://advances.realtimerendering.com/s2023/Nubis%20Cubed%20(Advances%202023).pdf>
  and <https://www.guerrilla-games.com/read/nubis-cubed>
- Hillaire, S. "Physically Based Sky, Atmosphere and Cloud Rendering in
  Frostbite." SIGGRAPH 2016 Physically Based Shading course.
  <https://blog.selfshadow.com/publications/s2016-shading-course/>
- Hillaire, S. "Physically Based and Unified Volumetric Rendering in
  Frostbite." SIGGRAPH 2015 Advances.
  <https://advances.realtimerendering.com/s2015/index.html>
- Bauer, F. "Creating the Atmospheric World of Red Dead Redemption 2: A
  Complete and Integrated Solution." SIGGRAPH 2019 Advances.
  <https://advances.realtimerendering.com/s2019/index.htm> (slides:
  `slides_public_release.pptx`)
- Hillaire, S. "Physically Based and Scalable Atmospheres in Unreal
  Engine." SIGGRAPH 2020 Physically Based Shading course.
  <https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf>
- Hillaire, S. "A Scalable and Production Ready Sky and Atmosphere
  Rendering Technique." EGSR 2020, Computer Graphics Forum 39(4).
  <https://diglib.eg.org/handle/10.1111/cgf14050>
- Wrenninge, M., Kulla, C., Lundqvist, V. "Oz: The Great and Volumetric."
  SIGGRAPH 2013 Talks (the multiple-scattering octaves). Cited by all of
  the above.
- Ang, "The Technical Art of Sea of Thieves." SIGGRAPH 2018 Talks.
  <https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf>
- Harris, M. "Real-Time Cloud Rendering for Games." GDC 2002.
- Guerrette, K. "Moving the Heavens: An Artistic and Technical Look at the
  Skies of The Last of Us." GDC 2014.
  <https://gdcvault.com/play/1020474/Moving-the-Heavens-An-Artistic>
- Weinrauch et al. "Clouds in the Cloud: Efficient Cloud-Based Rendering
  of Real-Time Volumetric Clouds." HPG 2023.
  <https://diglib.eg.org/items/2668054e-618d-4fb8-bbf7-d079a3ed4e75/full>
- Luebbert, S. "Demystifying the Clouds of Borderlands 4." GDC 2026
  (listing only).
  <https://schedule.gdconf.com/session/demystifying-the-clouds-of-borderlands-4-adapting-volumetric-cloud-technologies-to-a-ue5-open-world-game/915656>

**Engine documentation**

- Unreal: Volumetric Cloud component.
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine>
  and its properties page
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-properties-in-unreal-engine>
- Unity HDRP: Volumetric Clouds override.
  <https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.1/manual/volumetric-clouds-volume-override-reference.html>
- Unity HDRP: Cloud Layer override.
  <https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.1/manual/cloud-layer-volume-override-reference.html>
- Godot: sky shaders.
  <https://docs.godotengine.org/en/stable/tutorials/shaders/shader_reference/sky_shader.html>

**Plugins, assets and repositories**

- sebh/UnrealEngineSkyAtmosphere (the sky technique, with a path-traced
  reference): <https://github.com/sebh/UnrealEngineSkyAtmosphere>
- sebh/TileableVolumeNoise (tiling Perlin-Worley and Worley 3D noise for
  clouds): <https://github.com/sebh/TileableVolumeNoise>
- takram three-clouds (MIT; WebGL clouds with Beer shadow maps and
  temporal upscaling, a close model for a WebGPU port):
  <https://www.npmjs.com/package/@takram/three-clouds>
- clayjohn/godot-volumetric-cloud-demo: <https://github.com/clayjohn>
- Bonkahe/SunshineClouds2: <https://github.com/Bonkahe/SunshineClouds2>
  (Godot Asset Library: <https://godotengine.org/asset-library/asset/edit/20259>)
- gdquest-demos/godot-4-stylized-sky:
  <https://github.com/gdquest-demos/godot-4-stylized-sky>
- Enviro 3 – Sky and Weather (Unity):
  <https://discussions.unity.com/t/released-enviro-3-sky-and-weather-system/930064>
- COZY: Stylized Weather 3 (Unity):
  <https://assetstore.unity.com/packages/vfx/shaders/cozy-stylized-weather-3-271742>
  and COZY: Plume
  <https://assetstore.unity.com/packages/tools/particles-effects/plume-volumetric-clouds-for-cozy-2-243905>
- Altos – Sky and Clouds for URP:
  <https://assetstore.unity.com/packages/tools/particles-effects/altos-sky-and-clouds-for-urp-221227>
- Sky Master ULTIMATE (Unity), mobile report:
  <https://discussions.unity.com/t/50-off-sale-hdrp-upr-sky-master-ultimate-physical-skybox-volume-clouds-weather-fx-ocean-realtime-gi-mobile/555535?page=67>
- "Creating Volumetric Clouds for Mobile in Unreal Engine 5" (80.lv;
  Adreno 540 timings):
  <https://80.lv/articles/creating-volumetric-clouds-for-mobile-in-unreal-engine-5>
- Thatgamecompany, "Art of Sky: Children of the Light", GDC 2020:
  <https://gdcvault.com/play/1026903/Art-of-Sky-Children-of>
