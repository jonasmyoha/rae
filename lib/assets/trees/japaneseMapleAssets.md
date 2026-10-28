# Bloodgood Japanese maple assets

Original images generated with the built-in imagegen tool on 26 October 2026.
Both selected images were resized to 512 × 512 using `sips`. Production pixels
are original; botanical photographs and T04 provide direction only.

- `japaneseMapleSpray.png`: RGBA cutout, one irregular horizontally spreading
  spray with pointed palmate leaf edges and two unequal open notches. The first
  image had too much separate leaf shading. The selected edit preserves its
  alpha silhouette but removes fine grain and most internal contrast. Burgundy
  and muted crimson remain in broad groups. No baked directional highlight,
  emission, shadow exemption or custom renderer path is used.
- `japaneseMapleBark.png`: opaque, horizontally repeating albedo. Smooth muted
  reddish/gray young bark at the top blends into older gray-brown ridges below.
  The generator's height coordinate selects the age region. This is painted
  color, not geometric bark relief or a normal map.

Prompt record (built-in mode, no CLI fallback):

1. Original transparent Bloodgood canopy spray, wide asymmetrical branching
   silhouette, connected tapered lobes, fine five-to-seven-point palmate leaf
   edges, quiet wine-red/burgundy interior, warm tips and cooler plum interior;
   unlit albedo, no black pockets, gloss, backdrop or text.
2. Edit only interior color/lighting: preserve alpha and outline, remove fine
   grain, black cracks and separate highlights; broad calm crimson/burgundy,
   roughly ten-percent interior lightness variation and faint reddish twigs.
3. Original seamless-horizontal maple bark swatch: smooth reddish-brown and
   mauve-gray young wood above, subdued older gray-brown ridges below, gradual
   transition, pale lenticels, low contrast, no black cracks or baked shadows.

Selected original outputs: `exec-cb83e6a2-c359-460d-b702-18c20f871912.png`
(spray) and `exec-e249e1b1-0071-42e2-9af4-fb503e3dafe1.png` (bark).
First spray: `exec-874c2add-878c-4af5-965f-fa78c290a44e.png`.
The recovery presentation retains its actual rendered comparison.

The existing surface-image loader supplies mip chains and the existing foliage
material supplies cutout/two-sided/shadow handling. The asset still shows
repeated leaf marks at very close range. Bark ridges can stretch at forks.
