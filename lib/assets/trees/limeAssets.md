# Small-leaved lime assets

`limeBark.png` and `limeSpray.png` are original assets made with the built-in
image generation tool on 26 October 2026, then reduced to 512 × 512 with `sips`.
No botanical photograph or private reference screenshot supplies production pixels.

The bark is opaque grey-brown albedo, with broad unequal vertical furrows.
The spray is RGBA: small rounded/heart-shaped edge leaves, connected interior,
and a few sky holes. The second pass reduced its internal contrast. It is one
cutout, not an atlas of multiple variants. Seeded quarter-turns, mirrors and
unequal extents vary its placement. Very close views can still reveal repetition.

Both use the existing sRGB surface-image owner. Foliage uses the shared 0.5
cutout and coverage-preserving mip chain in forward, deferred, shadow and atlas
passes. There is no emission, extra shader, global lighting change or disabled
shadow. The same texture layers serve all instances.

Generation prompts, built-in tool:

- Bark: seamless opaque flat albedo of small-leaved lime, warm grey-brown,
  approximately twelve unequal vertical ridges, occasionally forked dark
  furrows, soft painterly surface, restrained contrast, no moss, birch marks,
  text, perspective or baked directional highlights.
- Foliage: one broad irregular lime spray on genuine transparency, small
  heart-shaped serrated edge leaves, connected interior, unequal lobes and a
  few narrow holes, subdued medium greens, no twig, strong pointed outline,
  individual shiny leaf lighting, black shadows or emission.
- Foliage refinement: preserve the silhouette and alpha edges; reduce the
  interior tonal variation to about one-third by raising dark pockets and
  slightly lowering bright leaves. Real-time lighting supplies the shading.

These are stylized assets. Individual painted interior leaves are not a precise
botanical identification aid. The species identity also depends on crown,
branching, bark, proportions and the documented preset.
