# Physical light units and an exposure stage

Status: planned (decision 2026-10-27). Three queue steps, in
order: units + camera exposure, automatic exposure, lit mist.

## Why

The renderer works in relative light units: every light is scaled so that
an exposure of 1 looks right. The sun colour is about 1, the ambient is
authored, the Preetham sky carries a hand scale, and the exposure is a
fixed number. That works while every light shares the same hand scale, and
it broke the first time a light did not: the Hosek-Wilkie sky gives
physical radiance, 15 to 60 times the Preetham sky's, so example 143 came
out white (the sky and the mist that takes its colour from the sky).

The stopgap (0.2.173) was one unit conversion, `Sky.skyUnitsPerKcd`, our
units per kcd/m2, used by both sky models (the Hosek-Wilkie RGB radiance is
first turned into kcd/m2 at 683 lm/W). The two skies agree because they
share units. This plan replaces that conversion with real units and a
camera, as physically based engines do (Frostbite's "Moving Frostbite to
PBR", 2014; Unreal and Unity HDRP work the same way).

## The target

- **Lights in real units.** The sun as illuminance in lux (about 100,000
  lux at noon on a clear day, less through more air near the horizon), the
  skies as luminance in cd/m2 (both models give it: Preetham in kcd/m2,
  Hosek-Wilkie at 683 lm/W), artificial lights in lumens or candela.
  `skyUnitsPerKcd` and the 0.05 in `sky_hosek.wgsl` go.
- **A camera exposure.** The frame's light is scaled to the screen by an
  exposure value (EV100: daylight about 14-15, a lit room about 7, moonlight
  about -2), before the tone curve. One number per frame in the light
  uniform; the HDR targets must hold the range (16-bit float: a 100,000 lux
  sun times exposure is fine, the sun before exposure is not, so exposure
  is applied where light is added, as the engines above do).
- **Automatic exposure.** The frame's mean log luminance (a small reduction
  pass or a histogram in compute), turned into a target EV with a
  compensation setting, approached slowly (seconds, faster toward dark than
  toward bright, like the eye). A manual EV stays possible for screenshots
  and tests (`RAE_EXPOSURE`). Without it, a noon scene and a moonlit one
  cannot both be right with the same lights.
- **Lit mist.** The height mist (RendererMist, `applyMist` in
  `deferred_shade.wgsl`) adds the light it scatters toward the eye: the sky's
  light (its irradiance from above, or the sky in the view direction) plus
  the sun's, with a phase function so it glows toward the sun. Today it
  copies the sky colour just over the horizon, which is only right while
  the sky is in relative units and has no very bright horizon.

## What it touches

- `lib/Sky.rae`, `lib/SkyHosek.rae`, `lib/sky_hosek.wgsl`: sky luminance in
  cd/m2, sun illuminance in lux (`sunColorForHeight` becomes a colour times
  an illuminance).
- The light uniform and the light pass: exposure where light is summed, the
  sun disc and the moon in the same units.
- `lib/RendererMist.rae`, `deferred_shade.wgsl`: in-scattered light.
- Every walker example's lighting settings (136, 137, 143), the tree lab
  (146) and the settings dialog (an exposure compensation in place of
  exposure).
- The forward renderer path, which shares the sky code.

## Step 1, done (0.2.174)

`lib/LightUnits.rae` holds the units: `exposureScale(ev100)` =
1 / (1.2 * 2^EV100), and `referenceEv100` = 14.024678, the exposure at
which the old relative units are real light (one old unit =
`relativeLightUnit` = 20,000 cd/m2, or lux). Every light value was
converted by that factor, so a scene at `referenceEv100` looks exactly as
before.

- `Light3d.sunColor` is the sun's illuminance in lux per channel,
  `ambSky`/`ambGround` the sky's and the ground's luminance in cd/m2, and
  `Light3d.ev100` the camera (default `referenceEv100`; scene files may set
  `"ev100"`). `Light3d.exposure` stays as a linear compensation after the
  lighting.
- Both sky models are in cd/m2: Preetham's kcd/m2 times 1000, Hosek-Wilkie's
  W/(m2 sr) times 683 (`Sky.hosekCdPerUnit`). `skyUnitsPerKcd` and the 0.05
  are gone. The ambient of a Hosek-Wilkie sky, its below-horizon fade, the
  moon's disc (`moonDiscIntensity`), the night sky and the night ambient are
  in cd/m2 too.
- Exposure is applied where light is summed: `GbufferPasses.uploadSceneLight`
  multiplies the sun and the ambient by the scale and sends it to the shader
  in `LightU.camPos.w`, where the sky models, the moon and the night sky are
  multiplied by it (`sky_hosek.wgsl` skyColor, `deferred_shade.wgsl`
  mistSkyColor). The water and the underwater caustics expose their sun and
  sky light the same way. So the 16-bit HDR targets hold the same range as
  before.
- Still in relative units (display colours): the stylised and solid-colour
  skies' painted colours on screen, the clear colour, the old distance fog,
  point lights and emissive materials. Where a painted colour lights the
  scene (a stylised sky's ambient, the solid sky's ambient) it is converted
  with `fromRelativeLight`.
- The walker scenes' sun is `SettingsSky.settingsSunNoonLux` = 20,000 lux at
  noon: their long-standing balance of sun to sky and ambient. A real clear
  noon is about 100,000 lux with the same sky, so the walker sun is about 5
  times weaker than real. The tree lab's sun is 66,000 lux, examples 140 and
  141 have 50,000-110,000 and 60,000. Moving the walker to a real sun is a
  look decision (the sunlit ground stays as it is at EV100 about 16.3, but
  the sky, the shade and the mist then come out 5 times darker); it is
  queued separately.

## Steps (QUEUE)

1. Units and a camera exposure: sun in lux, both skies in cd/m2, a fixed
   EV100 per scene, the walker examples' looks kept (before/after
   screenshots at noon, evening and night with both skies).
2. Automatic exposure: the reduction, the slow adaptation, compensation in
   the settings dialog, `RAE_EXPOSURE` for a fixed value.
3. Lit mist: in-scattered sky and sun light in place of the horizon colour;
   the same amount of mist with every sky (143 with Preetham and
   Hosek-Wilkie at noon and evening).
