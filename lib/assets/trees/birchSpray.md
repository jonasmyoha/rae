# Silver-birch spray texture

`birchSpray.png` is an original generated and art-directed RGBA albedo cutout,
512 × 512, authored on 26 October 2026. No pixels from the private reference
screenshots were used. The old birch it was made for is gone (the stylized
set's trees are treeGen kinds); `birchSpray.png` and `birchBark.png` stay as
test textures of the GPU checks (checks/surfaceImages, checks/impostorBake).

The outline has unequal connected lobes, pointed small edge leaves and narrow
gaps. The interior uses restrained green variation rather than painted leaf
shadows. Emission is zero; the ordinary material supplies light and shadow.
SurfaceImages uploads it as sRGB with coverage-preserving alpha mips. The
existing 0.5 alpha cutoff is shared by visible, shadow and atlas rendering.

The generator uses the full image, with mirrored placements. This is one spray
image, not a multi-cell atlas. Repetition is still visible close to a single
spray. Do not describe it as multiple authored atlas variants.

Rejected trials included an individually shaded, feather-like spray and an
almost unmarked green mass. Their renders are retained in the private recovery
presentation, not used by the forest.
