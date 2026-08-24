// Per-fragment terrain material detail (#539 ported to the fragment stage).
//
// procgen/texture.rae bakes the five biome materials into images by feeding a
// low-frequency and a high-frequency fbm value into a per-material colour ramp.
// The ramps are pure functions of those two numbers, so they can be evaluated
// live per pixel instead — which is what this file does. Same look, no texture
// memory, no atlas bleeding at tile seams, and no sampler bindings (nothing else
// in this renderer binds a texture).
//
// MOBILE. ONE fbm per fragment, and that is the entire per-pixel noise budget.
//
// The biome FIELDS (elevation, moisture, slope) are NOT evaluated here. Sampling
// them per pixel costs about 23 octaves — elevation is 4-octave fbm, moisture 3,
// and slope calls elevation four more times — which measured as a 50% frame-rate
// drop. They are low-frequency and smooth, so the vertex stage computes them and
// interpolates; only raeBiomeClassify runs per pixel, and that is smoothsteps
// with no noise at all. The sharp shoreline comes from the classify step, so it
// survives; the cost does not.
//
// The low-frequency variation `n` reuses the interpolated moisture rather than
// paying for a second fbm — free, and broad damp/dry patches are what it means.
// RAE_TERRAIN_DETAIL_OCT_HI is the quality knob; 1 on low-end parts.
//
// Requires lib/noise.wgsl and lib/world_biome.wgsl prepended.

// TWO SCALES, because one was not enough.
//
// Measured against the reference plate, the ground carried about 5x too little
// variation, and what it had died on downsampling — fine grain, no patches. Clean
// open meadow in the reference sits around std 0.026-0.07 in luminance and holds
// that through a 4x downsample, i.e. the variation lives in PATCHES a hundred-odd
// pixels across, with grain riding on top.
//
// (The 0.14-0.21 figures elsewhere in that image are trees and their shadows, not
// ground. Matching those with flat colour would read as noise soup.)
//
// Cost is two fbm per fragment rather than one. The octave counts are the knob;
// drop OCT_LO to 1 first on low-end parts, it is the cheaper loss.
const RAE_TERRAIN_DETAIL_OCT_HI: u32 = 2u;       // grain
const RAE_TERRAIN_DETAIL_OCT_LO: u32 = 2u;       // patches
const RAE_TERRAIN_DETAIL_HI_SCALE: f32 = 3.10;   // ~0.3 world units per feature
const RAE_TERRAIN_DETAIL_LO_SCALE: f32 = 0.75;   // ~1.3 world units per patch
const RAE_TERRAIN_DETAIL_SEED: u32 = 1337u;

// GRASS FRINGE at the grass/sand edge (#19). A beach does not meet grass at a
// clean line — grass frays into it in irregular tufts. These control that fray:
// REACH  = elevation depth the fringe reaches DOWN into the beach (bigger = wider,
//          fluffier band). SCALE = tuft frequency in world units (bigger = finer
//          strands). STRENGTH = how far the strands tint the sand toward grass.
const RAE_GRASS_FRINGE_REACH: f32 = 0.14;
const RAE_GRASS_FRINGE_SCALE: f32 = 3.8;
const RAE_GRASS_FRINGE_STRENGTH: f32 = 0.82;

// GROUND PALETTE (the calibrated RAE_TERRAIN_* colours + their variation)
// lives in terrain_palette.wgsl now (#13), composed just before this file so
// these constants are in scope. It is per-app overridable; see that file.

fn raeTerrainVary(base: vec3<f32>, amount: f32, t: f32) -> vec3<f32> {
  return base * (1.0 - amount * 0.5 + amount * t);
}

// --- water -----------------------------------------------------------------
//
// The sea is part of the terrain mesh, clamped flat at the water level, so it
// gets its look here rather than from a separate water pass. Three things carry
// it, in order of how much they matter from a top-down camera:
//
//   1. DEPTH. Shallow water over the beach shelf reads lighter and greener,
//      deep water darker and bluer. This is what makes a shoreline look like a
//      shoreline rather than a blue shape with a hard edge.
//   2. FOAM. A bright band where the land meets the sea, broken up by noise so
//      it is a surf line and not a stroke.
//   3. RIPPLE. Two crossing wave sets, cheap, mostly a brightness wobble. From
//      above there is no silhouette to distort, so displacement would be wasted.

const RAE_WATER_SHALLOW: vec3<f32> = vec3<f32>(0.075, 0.330, 0.430);
const RAE_WATER_FOAM: vec3<f32>    = vec3<f32>(0.900, 0.950, 0.960);
// How far below the water line counts as "deep", in elevation units.
const RAE_WATER_DEEP_AT: f32 = 0.16;
// Width of the surf band, in water weight.
// How deep the surf reaches, in the same units as RAE_WATER_DEEP_AT.
const RAE_WATER_FOAM_DEPTH: f32 = 0.38;
// How opaque the surf gets at its strongest.
const RAE_WATER_FOAM_STRENGTH: f32 = 0.80;
const RAE_WATER_RIPPLE_SCALE: f32 = 5.5;
const RAE_WATER_RIPPLE_SPEED: f32 = 0.55;

// WET SAND (QUEUE #3). A damp band just above the water line, darker and a
// little more saturated than dry beach.
//
// The reference has it and it is doing more work than it looks: a beach that
// meets the sea at one flat tone reads as two shapes touching, while a damp
// strip reads as one surface the water has been over. Driven by elevation above
// the water line, which is smooth and monotonic across the shore -- the biome
// weights are nearly a step there and cannot express a gradient (see AGENTS.md).
const RAE_WET_SAND_RISE: f32 = 0.150;
const RAE_WET_SAND_DARKEN: f32 = 0.52;

// How far the surf washes UP the beach, in the same elevation units. The foam
// band alone sits at the water's edge; this is what lets it run onto the sand.
const RAE_WATER_SWASH_RISE: f32 = 0.038;
const RAE_WATER_SWASH_SPEED: f32 = 0.45;
const RAE_WATER_SWASH_STRENGTH: f32 = 0.40;

fn raeWaterColor(p: vec2<f32>, elev: f32, wWater: f32, t: f32) -> vec3<f32> {
  // Depth from how far the ground sank below the water line.
  let depth = clamp((RAE_BIOME_WATER_LEVEL - elev) / RAE_WATER_DEEP_AT, 0.0, 1.0);
  var col = mix(RAE_WATER_SHALLOW, RAE_TERRAIN_WATER, depth);

  // Two crossing wave sets. Cheap sines rather than fbm: this rides on top of
  // the one noise evaluation the ground already pays for.
  let w1 = sin(p.x * RAE_WATER_RIPPLE_SCALE + t * RAE_WATER_RIPPLE_SPEED * 1.7);
  let w2 = sin((p.x * 0.6 + p.y) * RAE_WATER_RIPPLE_SCALE * 0.8
               - t * RAE_WATER_RIPPLE_SPEED * 1.1);
  col = col * (1.0 + 0.06 * (w1 * 0.5 + w2 * 0.5));

  return col;
}

// How much surf covers this spot, 0..1.
//
// SEPARATE FROM THE WATER COLOUR, and applied after the material blend. Foam
// belongs exactly where water and sand are equally weighted, so folding it into
// the water term meant multiplying it by wWater ~ 0.5 and then blending half of
// that away against the sand — a surf line that was mathematically present and
// visually absent. The reference's is a strong white band; this has to survive
// the blend to look like one.
fn raeWaterFoam(p: vec2<f32>, elev: f32, wWater: f32, t: f32) -> f32 {
  if (wWater < 0.04) { return 0.0; }
  let depth = clamp((RAE_BIOME_WATER_LEVEL - elev) / RAE_WATER_DEEP_AT, 0.0, 1.0);
  // SURF COVERS SHALLOW WATER, not just the line where the weights cross.
  //
  // This used to key off `abs(wWater - 0.5)`, which is a knife edge -- one thin
  // stroke exactly where water and sand are evenly weighted. The reference's
  // surf is nothing like that: it is a wide lacy field over the whole shallow
  // shelf, breaking in bands that roll shoreward. Depth is the right driver
  // because it is what actually decides where waves break.
  let shallow = 1.0 - smoothstep(0.0, RAE_WATER_FOAM_DEPTH, depth);
  // Bands rolling toward the beach.
  let roll = sin((p.x * 0.55 + p.y * 0.85) * RAE_WATER_RIPPLE_SCALE * 0.45
                 - t * RAE_WATER_RIPPLE_SPEED * 1.6);
  let bands = clamp(0.28 + 0.90 * roll, 0.0, 1.0);
  // A finer break-up across the bands so the edges are ragged rather than ruled.
  let fine = 0.5 + 0.5 * sin((p.x * 1.7 - p.y * 1.1) * RAE_WATER_RIPPLE_SCALE * 1.25
                             + t * RAE_WATER_RIPPLE_SPEED * 1.9);
  // Crossing sines alone read as CORDUROY -- regular diagonal stripes, which is
  // what they are. One fbm sample, drifting with the clock, turns the bands into
  // lace. Paid only on water pixels, and only where the surf is already non-zero.
  let lace = clamp(raeNoiseFbm2(p * 1.9 + vec2<f32>(t * 0.20, t * -0.13),
                                2u, 2.0, 0.5, RAE_TERRAIN_DETAIL_SEED + 4441u)
                   * 0.5 + 0.5, 0.0, 1.0);
  return clamp(shallow * bands * (0.50 + 0.50 * fine) * (0.35 + 0.85 * lace), 0.0, 1.0);
}

// SWASH: the sheet of foam that runs up the wet sand and slides back.
//
// Separate from raeWaterFoam because that one lives where water and sand are
// equally weighted -- the water's edge -- and by definition cannot reach onto
// land. This is keyed off elevation above the water line instead, so it covers
// the beach itself, and it BREATHES: the reach oscillates, which is what makes a
// shoreline look alive from a static top-down camera where nothing else moves.
fn raeWaterSwash(p: vec2<f32>, elev: f32, wSand: f32, t: f32) -> f32 {
  if (wSand < 0.04) { return 0.0; }
  let aboveWater = elev - RAE_BIOME_WATER_LEVEL;
  if (aboveWater < 0.0) { return 0.0; }
  // The tide line moves in and out; 0.55..1.0 of the nominal reach.
  let breathe = 0.55 + 0.45 * (0.5 + 0.5 * sin(t * RAE_WATER_SWASH_SPEED));
  let reach = RAE_WATER_SWASH_RISE * breathe;
  let up = 1.0 - smoothstep(0.0, reach, aboveWater);
  // Broken along the shore so it is a series of tongues, not a ruled line.
  let lace = 0.5 + 0.5 * sin((p.x * 0.9 - p.y * 0.5) * RAE_WATER_RIPPLE_SCALE * 0.55
                             + t * RAE_WATER_SWASH_SPEED * 2.1);
  return clamp(up * (0.35 + 0.65 * lace), 0.0, 1.0);
}

// Ground albedo, textured per pixel and blended by biome weight. `bio` is the
// interpolated (elevation, moisture, slope) from the vertex stage; `p` is world
// XY for the grain. The weights already sum to 1 (raeBiomeClassify normalises
// them), so this is a straight weighted sum with no renormalise.
fn raeTerrainDetailColor(p: vec2<f32>, bio: vec3<f32>, t: f32) -> vec3<f32> {
  let b = raeBiomeClassify(bio.x, bio.y, bio.z);
  let d = clamp(raeNoiseFbm2(p * RAE_TERRAIN_DETAIL_HI_SCALE,
                             RAE_TERRAIN_DETAIL_OCT_HI, 2.0, 0.5,
                             RAE_TERRAIN_DETAIL_SEED) * 0.5 + 0.5, 0.0, 1.0);
  let blotch = clamp(raeNoiseFbm2(p * RAE_TERRAIN_DETAIL_LO_SCALE,
                                 RAE_TERRAIN_DETAIL_OCT_LO, 2.0, 0.5,
                                 RAE_TERRAIN_DETAIL_SEED + 991u) * 0.5 + 0.5, 0.0, 1.0);
  // Patches carry most of it, grain rides on top, and the interpolated moisture
  // adds a very broad drift so two distant meadows are not the same green.
  // STRETCHED AROUND THE MIDPOINT. fbm output clusters near its mean, so a raw
  // mix of two octaves lands in roughly 0.35-0.65 and raeTerrainVary's factor
  // stays near 1.0 -- the variation was computed, then thrown away by its own
  // distribution. Measured against the plate, grass p10..p90 spanned 0.12 where
  // the reference spans 0.56. Expanding v about 0.5 is what puts the shades back.
  let vRaw = 0.20 * clamp(bio.y, 0.0, 1.0) + 0.55 * blotch + 0.25 * d;
  let v = clamp(0.5 + (vRaw - 0.5) * RAE_TERRAIN_VAR_STRETCH, 0.0, 1.0);
  // Damp where the sea has recently been. Only the sand term: wet grass is not a
  // thing the reference shows, and darkening the whole sum would dim the water.
  let aboveWater = bio.x - RAE_BIOME_WATER_LEVEL;
  let wet = 1.0 - smoothstep(0.0, RAE_WET_SAND_RISE, aboveWater);
  let sandWet = mix(1.0, RAE_WET_SAND_DARKEN, wet);
  let groundBase = raeTerrainVary(RAE_TERRAIN_GRASS, RAE_TERRAIN_VAR_GRASS, v) * b.wGrass
       + raeTerrainVary(RAE_TERRAIN_SAND,  RAE_TERRAIN_VAR_SAND,  v) * sandWet * b.wSand
       + raeTerrainVary(RAE_TERRAIN_MUD,   RAE_TERRAIN_VAR_MUD,   v) * b.wMud
       + raeTerrainVary(RAE_TERRAIN_ROCK,  RAE_TERRAIN_VAR_ROCK,  v) * b.wRock
       + raeWaterColor(p, bio.x, b.wWater, t) * b.wWater;
  // A road lies over normalized biome materials; it is not a sixth biome that
  // steals weight from grass and sand throughout the island.
  let path = raeBiomePath(p);
  let roaded = mix(groundBase,
                   raeTerrainVary(RAE_TERRAIN_PATH, RAE_TERRAIN_VAR_PATH, v),
                   path * (1.0 - b.wWater));
  // GRASS FRINGE (#19). Soft grass strands fraying into the sand at the grass/sand
  // edge. `nearGrass` rises toward the grass elevation (sandHi); weighting by
  // wSand restricts it to the beach and makes it PEAK at the boundary (sand still
  // present, grass just above). A high-frequency fbm breaks the tint into discrete
  // tufts — brush strokes of grass — rather than a smooth band, and its own soft
  // threshold keeps the strand edges feathery. This both softens the hard line and
  // bakes grass into the beach edge. Suppressed where the surf/road already own it.
  let sandHiE = RAE_BIOME_WATER_LEVEL + RAE_BIOME_BEACH_BAND;
  // `nearGrass` rises across the whole upper-beach REACH band up to the grass
  // elevation (not just the thin wSand/wGrass crossfade), so the fringe is a wide
  // fluffy band, strongest nearest the grass and fading down into the sand. On the
  // grass side this is a no-op (grass mixed over grass); on the beach it greens.
  let nearGrass = smoothstep(sandHiE - RAE_GRASS_FRINGE_REACH, sandHiE, bio.x);
  let tuft = clamp(raeNoiseFbm2(p * RAE_GRASS_FRINGE_SCALE, 2u, 2.0, 0.5,
                                RAE_TERRAIN_DETAIL_SEED + 733u) * 0.5 + 0.5, 0.0, 1.0);
  // A soft base bleed (0.35) keeps the edge from reading as a line; the tuft term
  // adds brush-stroke strands on top. Suppressed under water and road.
  let strand = (0.35 + 0.65 * smoothstep(0.30, 0.70, tuft)) * nearGrass
             * (1.0 - b.wWater) * (1.0 - path) * RAE_GRASS_FRINGE_STRENGTH;
  let ground = mix(roaded, raeTerrainVary(RAE_TERRAIN_GRASS, RAE_TERRAIN_VAR_GRASS, v), strand);
  let foam = raeWaterFoam(p, bio.x, b.wWater, t);
  let swash = raeWaterSwash(p, bio.x, b.wSand, t);
  let surf = clamp(foam * RAE_WATER_FOAM_STRENGTH + swash * RAE_WATER_SWASH_STRENGTH, 0.0, 1.0);
  return mix(ground, RAE_WATER_FOAM, surf);
}
