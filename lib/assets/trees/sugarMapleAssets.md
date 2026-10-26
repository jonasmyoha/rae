# Sugar maple autumn-orange assets

Original production albedos, generated with the built-in imagegen tool on
26 October 2026. `sips` reduced the selected images to 512 × 512. No reference
screenshot or botanical photograph supplies production pixels.

- `sugarMapleSpray.png`: RGBA connected irregular spray with small five-lobed
  maple edges. The first two versions had too many bright individual marks;
  the selected third version has quiet copper-orange interiors. It is one
  cutout, varied through mirrored UVs, vertical flips, size and placement.
- `sugarMapleBark.png`: opaque grey-brown long irregular ridges and plates.
  Its broad scale is chosen for the existing small game-tree preset.

Both use the existing surface-image owner. Foliage uses the same alpha cutoff,
coverage-preserving mips, two-sided lighting, twig wind, shadows and posed atlas
path as the recovered foliage. There is no emission or shader exception.

## Prompt set

Built-in generation, not the CLI fallback:

1. Create one connected sugar maple branch spray on genuine transparency;
   many small five-lobed Acer saccharum leaves; spreading asymmetric lobes,
   small edge leaves and narrow holes; muted copper, burnt orange and apricot;
   flat albedo without individual highlights, deep shadows, stems or text.
2. Keep its silhouette and alpha while halving interior contrast, making
   bright yellow leaves muted apricot and dark regions lighter copper.
3. Keep sharp maple edge leaves and holes, but suppress the internal
   yellow/dark checker pattern into broad painted copper-orange regions;
   only faint leaf boundaries, about 10% value variation; no blur, shine,
   background or emission. This is the selected foliage version.
4. Seamless flat sugar maple bark albedo: warm grey-brown, long irregular
   interlocking ridges, occasional lifted narrow plates, medium-scale painted
   surfaces, sparse creases; no moss, knots, text, perspective or directional
   highlights. This is the selected bark image.

The bark retains some painted relief. The foliage retains repeated leaf motifs
at very close range. These are documented limitations, not new lighting effects.
