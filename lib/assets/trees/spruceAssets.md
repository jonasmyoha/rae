# Norway spruce textures

Original images made with the built-in imagegen tool on 26 October 2026.
Selected outputs were resized to 512 × 512 with `sips`, retaining alpha.
No botanical photographs or private reference pixels are included in the assets.

- `spruceSpray.png`: one connected needle spray with fine irregular cutout
  edges, modest holes and a quiet gray-green interior. The second generation
  reduces interior contrast; the first was too busy on the actual bough mesh.
- `spruceBark.png`: repeating gray-brown thin scales. This is painted albedo,
  not geometric relief. The wood remains rigid.

Prompt record (built-in mode, no CLI fallback):

1. Original transparent Norway spruce bough, fine short needles along a
   discreet vertical twig, a connected broad middle tapering at both ends,
   quiet sage/gray-green masses, small gaps, flat diffuse lighting, fine
   needle perimeter, no broad leaves, long pine needles, shadows or text.
2. Edit only interior color: preserve alpha, silhouette and needle edges;
   remove pale central twig and outlined needles; reduce internal contrast
   about 75 percent, keep the average gray-green and connected painted masses.
3. Seamless gray-brown spruce bark albedo, small irregular rounded overlapping
   scales with modest vertical orientation, warm gray/tan and cooler plates,
   shallow brown fissures, restrained contrast, no moss, black cracks, cast
   shadows, perspective or text.

Selected output identifiers:
`exec-feebeb9d-81e1-40df-b398-b87a240f6725.png` (spray),
`exec-601cfa94-df4a-4951-8a2d-f86854530cbd.png` (bark).
First spray: `exec-c54e9cf4-215c-453b-8682-73bd549b74db.png`.

The existing shared surface-image path owns both images, linear-light mips and
coverage preservation. Color, shadows and impostor capture use the same
alpha cutoff. No emission, normal-map contrast, renderer exposure adjustment
or species shadow exemption is used.
