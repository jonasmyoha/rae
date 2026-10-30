# Sky system design — one interface, realistic and stylized behind it

Status: design, not built. Companion to `shadow-system-design.md` and
`deferred-renderer-*`; the same house style — decisions with the reason
attached, and the rejected options recorded so they are not re-proposed.

---

## 1. What we are solving

The renderer currently has no sky at all. The background is a clear
color, and ambient light is a two-color hemisphere constant fed in by
hand (`Light3d.ambSky` / `ambGround`). That is two separate lies:

* The background is not the environment. Nothing in the frame agrees
  with what is behind it.
* Ambient is authored, not derived. A scene under a sunset gets whatever
  hemisphere colors somebody typed, so the lighting and the backdrop
  drift apart every time either is edited.

Both examples we care about want the opposite of each other:

* **109 / 110 / 111 (realistic)** want a real environment — a sky that
  is both visible and the source of indirect light, so a chrome sphere
  reflects the actual clouds above it.
* **113 (stylized)** wants a *painted* sky. It now has cel shading and
  a grass ground, and dropping a photographic HDRI behind it would
  produce the exact failure this document exists to avoid: **3D models
  pasted onto a photograph**. The mismatch is not subtle, and no amount
  of tuning the character fixes it.

So the requirement is not "add a skybox". It is: **one sky interface,
several very different implementations, and the lighting path must not
know which one is loaded.**

---

## 2. The one decision that makes the rest easy

**Every sky implementation must be able to answer two questions, and
nothing else:**

```
radiance(direction)        -> what the camera sees looking that way
irradiance(normal)         -> what a surface facing that way receives
```

The first is the background and the reflection source. The second is
ambient. Everything below is an implementation of those two functions.

This is what lets a painted sky and an HDRI be swapped without the
lighting pass changing a line, and it is why this is worth designing
before building rather than adding a skybox and retrofitting lighting
later.

**The deferred frame does not care.** A sky is read by the lighting pass
and by a background pass; neither is affected by the G-buffer's
existence. This is not a reason to touch the deferred/forward question.

---

## 3. Three implementations behind that interface

### Layer A — HDRI environment (realistic)

An equirectangular HDR image, sampled directly for background and
prefiltered for lighting.

**Source: Poly Haven.** CC0, which matters more here than quality: this
repository has already had to rewrite its history once over an asset
whose license could not be established (see
`examples/136_walker_character/assets/ATTRIBUTION.md`). CC0 removes that
class of problem entirely. Their **"Pure Sky"** variants — sky isolated
from the ground and surroundings — are the correct pick for a game
skybox; a full-scene HDRI bakes someone else's terrain into our horizon.

Three products from one image:

```
HDRI ──┬── background     (sample by view direction)
       ├── irradiance     (SH or a small prefiltered cube)
       └── sun extraction (brightest region -> direction + color)
```

**Sun extraction earns its place.** Without it the artist sets a sun
direction that disagrees with the sky, and every shadow in the scene
points the wrong way relative to the visible light source — an error
that reads as "the lighting is broken" without anyone being able to say
why. Deriving the sun from the image makes that disagreement
unrepresentable.

**Irradiance as 9 spherical-harmonic coefficients**, not a cube.
Diffuse irradiance is a very low-frequency signal; SH9 reconstructs it
to within a few percent, costs 9 vec3 in a uniform, needs no sampler,
and replaces the hemisphere constant with a drop-in evaluation. A
prefiltered cube is only needed for *specular* reflection, which is a
later increment (see §7) and should not gate the diffuse win.

**Format.** `.hdr` (RGBE) rather than EXR — a decoder is ~200 lines and
we already own a pure-Rae image path (`lib/png.rae`, `lib/compress`).
EXR is a much larger specification for no benefit at sky dynamic range.

### Layer B — Procedural physical sky (realistic, no asset)

Analytic sky from a sun direction — Preetham or Hosek-Wilkie.

Worth having even with Layer A, for reasons that are not about looks:
it is **an asset-free default**, it **animates** (time of day, which a
static HDRI cannot do at all), and it gives the test suite a sky with no
binary dependency. It is also the honest answer for "I just want a
sky" — the case where downloading a 100 MB HDRI is absurd.

Irradiance comes from integrating the same analytic function into the
same SH9 form, so Layers A and B are interchangeable downstream.

### Layer C — Stylized sky (the Ghibli direction)

**Not an HDRI.** This is the substantive design point of the document.

A painted panorama is better than a photograph for a cel-shaded scene,
but it is still *static*, and it still cannot change with time of day or
give parallax. The better structure is **layers composited in a shader**:

```
                    ☀ sun / moon          crisp disc, artist color
                         │
              ┌──────────┴──────────┐
              │   painted clouds    │     scrolling, parallaxed
              └──────────┬──────────┘
                         │
                atmospheric gradient      zenith -> horizon ramp
                         │
                     horizon haze
                         │
             distant silhouettes          hills, treeline
```

What this buys over a painted panorama, and why it is worth the extra
work:

* **Time of day** — the gradient and the cloud tint are parameters, so
  dawn to dusk is an animation rather than six 8K images.
* **Parallax** — cloud layers move at different rates against camera
  motion, which is most of what makes a sky feel like a place rather
  than a wall.
* **The palette is ours.** A cel scene's sky has to agree with the
  character's shadow tint. With a gradient we set both from the same
  palette; with a purchased panorama we grade the character to match
  someone else's sky.
* **It is tiny.** A gradient plus two cloud textures, against 8K
  equirectangular images per time of day.

Cloud shapes can come from a tiling painted texture or from
value-noise-with-a-threshold; the threshold version is the one that
composes with the cel look, because a hard cloud edge is the same
decision as a hard terminator.

**Stylized irradiance is a lie, deliberately.** Do not integrate the
painted sky. Take the ambient straight from the gradient's two anchor
colors, because a cel scene wants *chosen* ambient — the whole point of
§1's "shadows shift toward the sky ambient" in the toon path is that the
artist controls that hue.

**Assets, if a painted panorama is wanted anyway.** LineKotsi's
hand-painted equirectangular skies and the free QMS Cartoon Skybox Pack
are both reasonable, but **both need their licenses read before
anything is committed** — neither is CC0, and the Poly Haven reasoning
above applies with more force to a paid asset. Layer C's whole design is
chosen so that shipping *no* purchased asset is the default path.

---

## 4. Where it sits in the frame

Two touch points, both small:

```
shadow → gbuffer → ssao → depthPyramid → lighting → taa → composite → present
                                            ▲          ▲
                                            │          │
                                     irradiance()   sky background
                                                    (where depth == far)
```

**Background is drawn in the lighting pass, not as a separate pass.**
Lighting already early-outs on `depth <= 0.0` (reverse-Z far) and
returns the clear color — that branch becomes `skyRadiance(dir)`. One
touched line, no extra pass, no extra bandwidth, and it cannot desync
from the depth test that selects it.

**Irradiance replaces the hemisphere blend** in the same shader:
`mix(ambGround, ambSky, N.z*0.5+0.5)` becomes `skyIrradiance(N)`. The
existing constants remain as the fallback when no sky is bound, so this
is additive and every current example keeps working untouched.

The sky must be in **linear HDR before tonemapping** — it is inside the
`hdrColor` target, so a 20,000-nit sun goes through ACES with everything
else rather than clipping to white first.

---

## 5. Interface sketch

```rae
enum SkyKind { none, hdri, procedural, stylized }

type Sky {
  kind: SkyKind
  exposure: Float
  yawDeg: Float          # rotate the environment without re-exporting
  sunDir: Vec3           # extracted (A), authored (B/C)
  sunColor: Vec3
  irradianceSh: Array(Vec3, cap: 9)
  # ... per-kind payload
}

func skyIrradiance(sky: view Sky, n: view Vec3) ret Vec3
func bindSky(sky: view Sky)      # uploads whatever the GPU path needs
```

`yawDeg` is not a convenience: it is the difference between an HDRI
being usable and being re-exported in Blender because the sun is behind
the camera.

---

## 6. Build order

Each step is independently useful and independently verifiable:

1. **The interface plus the hemisphere fallback.** No visual change;
   proves the seam. Test: lighting output identical before/after.
2. **Layer B, procedural.** First real sky, zero assets, testable in
   CI. Test: zenith bluer than horizon, sun disc where it was placed.
3. **SH9 irradiance from B**, replacing the constants. Test: SH
   reconstruction against direct integration, to a few percent.
4. **Layer A, `.hdr` decode + background + SH.** Test: decode a known
   RGBE image, and assert extracted sun direction on a synthetic sky
   with one bright spot.
5. **Sun extraction driving the shadow cascades**, closing the loop
   between visible sun and cast shadow.
6. **Layer C, stylized**, in example 113 alongside toon shading.
7. **Prefiltered specular cube** (deferred, see §7).

Steps 1–3 need no binary asset at all, which is the argument for that
ordering: the first three land as pure code with real tests.

---

## 7. Deliberately deferred

* **Specular IBL** (prefiltered cube + split-sum BRDF LUT). Diffuse is
  the larger visual win and far cheaper; adding a roughness-mipped cube
  before diffuse works is optimizing the second-order term first.
* **Aerial perspective / height fog.** Related and wanted, but a scene
  effect, not a sky, and it belongs with the volumetrics work.
* **Cloud shadows** on the ground. Cheap once Layer C exists (project
  the cloud texture along the sun) but it is a shadow feature.
* **Dynamic environment capture** for reflections of scene geometry.
  Different problem, different cost class.

---

## 8. Risks

* **The stylized path drifting into "just an HDRI".** It is the easy
  option every time. The mitigation is that §3C's layers are the
  *specification*, and example 113 is the acceptance test.
* **License drift.** Anything not CC0 needs its terms recorded in an
  `ATTRIBUTION.md` next to it, before commit. This repository has
  already paid for getting that wrong once.
* **Ambient double-counting.** When sky irradiance lands, the authored
  hemisphere constants must stop contributing rather than adding to it.
  Detected by step 1's identical-output test.
* **Sky in the G-buffer.** It must not write geometry. It is a lighting
  read at far depth; a sky that ends up in the depth buffer breaks SSAO
  and TAA at the horizon.

## 9. Moons and eclipses (2026-10-30)

- **The earth's moon** (lib/Moon) follows an ephemeris: the mean elements
  of its orbit with the main perturbations of its longitude, latitude and
  distance (Paul Schlyter's series), the sun's longitude and distance from
  the same source, terrestrial time ahead of universal time by delta T
  (Espenak and Meeus' fits by year: 50 s in 1980, 203 s in 2100), and the
  moon seen from the observer's place (the sidereal time and its parallax,
  up to a degree). Against the published eclipses from 2015 to 2028 it
  stood within about 1 - 2 arc minutes of the sun at their moments and
  places of greatest eclipse.
- lib/Eclipses holds 20 real solar eclipses from 1980 to 2099 (the dates
  and kinds published in NASA's canon). Their moments and places were
  refined with the model, along each central path near the published
  greatest eclipse, so the sky shows each at its deepest: every total one
  fully covered, with the corona (case 1258).
- **Placed relative to the sky's sun**, whose own model is simpler (local
  solar time and a fitted declination): each moon's hour angle and
  declination are the sky's sun's plus the ephemeris' differences, so an
  eclipse covers the sun the sky shows.
- **Drawn at its real size near the sun**, its game size (2.5 times) away
  from it. The moon's disc hides the sun's disc (the sky shader's
  `moonsCover`); the light and the sky dim by the covered share
  (`SettingsSky.settingsEclipseLight`), the moon's disc goes dark and,
  past 97% covered, the corona shows round it. What is left of the sun
  keeps its surface brightness.
- **More moons:** `MoonBody` describes a moon; `inventedMoons` gives two
  unlike the earth's (Ember, small and rust-red, an 11.7-day orbit tilted
  9°; Tide, big and pale blue-green, 61 days, 14°). `Sky.moons` holds up to
  three extra moons, drawn by the sky shader in their own size and color
  and lit by the same sun; the settings' "More moons" button (`RAE_MOONS=3`)
  adds them.
- **The eclipse list in the settings:** "Previous eclipse" / "Next
  eclipse" step through lib/Eclipses (wrapping round), setting the date,
  the local solar time of the greatest eclipse and the place, and holding
  the clock there; `RAE_ECLIPSE=<n>` starts at one.
