# Meadow tuft cutout

Original albedo/alpha asset generated for this repository on 26 October 2026
with the built-in image generation tool, then resized to 512 × 512 with `sips`.
No pixels from the supplied landscape reference images were used.

Prompt: a single side-view meadow grass tuft on a genuinely transparent
background; about 18 thin curved tapering blades rising from a dense narrow
root, unequal heights and leaning tips, quiet matte olive-green paint, dark
lower fifth and softly lighter tips, no ground, cast shadow, outline, veins,
flowers or seed heads. Upper blades have narrow transparent gaps; the lower
blades connect into a full base. This is an albedo/alpha asset for actual engine
lighting, not a painted dramatic lighting pass.

The shader uses the image's luminance as a restrained multiplier of the app's
existing grass color; it does not replace the app palette. Three fixed crossing
planes share this image and bend around their grounded base. A 0.5 alpha cutoff
and the existing SurfaceImages coverage-preserving mips keep thin edges from
turning into rectangular opaque sheets. There is no emission or alpha blending.
The single tuft motif remains recognizable close up; more artwork variants and
broader ground blending are separate work.
