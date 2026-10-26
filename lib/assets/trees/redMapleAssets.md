# Red maple scarlet assets

Original albedo assets generated with the built-in imagegen tool on 26 October
2026, then resized to 512 × 512 with `sips`. No reference crop or botanical
photograph provides production pixels.

- `redMapleSpray.png`: one irregular connected RGBA spray, scarlet/vermilion
  three-lobed maple edges and quiet red interiors. Shared cards mirror the
  cutout and flip alternate sprays. Alpha is preserved.
- `redMapleBark.png`: warm silver-grey albedo. The bottom is older ridged bark;
  the top is smoother young bark. The mesh samples this gradient once along
  height instead of repeating the rough base on every branch.

## Generation prompts

Built-in generation mode; no CLI/API fallback:

1. Original RGBA game albedo, transparent background; a broad asymmetrical
   connected Acer rubrum branch spray, unequal fingers, small notches and tiny
   sky holes; fine small serrated three-lobed edge leaves; broad quiet painted
   interior regions with less than ten percent contrast. Muted warm scarlet,
   dusty vermilion, brick/coral; no pure red, dark burgundy, yellow, individual
   highlights, deep cracks, stems, lighting, text or background.
2. Edit the foliage while preserving silhouette and alpha: reduce saturation
   by about one third toward dusty scarlet/soft brick red, target interior
   RGB around (185,91,82); halve interior leaf-mark contrast. This second
   version is selected after matched application renders.
3. Full-bleed opaque red-maple bark albedo, horizontally seamless; bottom
   quarter long shallow uneven vertical ridges/flaky plates, gradual middle
   transition, top half smooth silver-grey with restrained mottling. Low
   contrast warm grey/taupe, no deep cracks, directional light, knots, moss,
   white birch, text or perspective.

The leaf image still has some interior marks. Bark has painted relief rather
than a normal map. Those limitations are evaluated in the actual renders.
