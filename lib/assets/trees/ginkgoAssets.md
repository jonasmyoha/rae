# Golden Globe ginkgo assets

Original images made with the built-in imagegen tool on 26 October 2026,
resized to 512 × 512 with `sips`. No botanical/reference photograph supplies
production pixels. RGBA transparency is retained in `ginkgoSpray.png`.
`ginkgoBark.png` is opaque. Both use the existing surface-image owner.

Prompts used (built-in mode, no CLI/API fallback):

1. Connected irregular golden autumn ginkgo spray on a genuine transparent
   background; about 90 small scalloped fan leaves with shallow central notches
   and wedge bases, short shoots, uneven branching silhouette and small holes.
   Quiet painted diffuse albedo, restrained contrast, no outlines/highlights,
   no ground/text/background, transparent margin, no circular ball or sheet.
2. Edit that spray, preserving silhouette/alpha/composition: remove nearly all
   baked leaf shadows, veins, highlights and dark twig contrast. Unlit painted
   buttery gold, about 12 percent value variation, broad gentle groups. Keep
   perimeter fans legible while interiors form calm connected color. This
   revision is selected after actual matched app renders.
3. Seamless horizontally tileable gray-brown ginkgo bark albedo: narrow uneven
   shallow vertical furrows and connected ridges. Older ridges at bottom fade
   to smooth pale young wood above. Quiet painted medium-scale relief, even
   light, no deep cracks/highlights/moss/text or prominent repeated marks.

Selected originals remain in the imagegen output directory as
`exec-4df21d15-5f31-4caf-99bb-4ca5769d8825.png` (spray) and
`exec-b38f5626-7815-490a-a910-df24cd0e7bd9.png` (bark).
The first spray, `exec-c75ce08d-9f75-4d08-856c-764fee843b7f.png`, had too much
individual leaf contrast. Comparison frames remain outside git.

The spray still contains a small amount of painted value separation. Bark
is color only, not geometric relief or a normal map. These are stated visual
limits, not claims of measured physically accurate reflectance.
