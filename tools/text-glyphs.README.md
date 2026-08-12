# tools/text-glyphs.txt — the shared UI text charset

Fed to `msdf-atlas-gen -charset` by every example that bakes a Roboto MSDF
atlas: `examples/106_mobile_ui`, `examples/112_gpu3d_ui`,
`examples/114_walker_character`.

**One file, on purpose.** Those examples mount the same `lib/ui` and
`lib/app3d` components, so a glyph one of them needs the others need too. The
failure mode of disagreeing is silent: a missing glyph is *skipped* by the
renderer, so text quietly loses a letter rather than showing a box. Three
per-example lists would drift within a week — 110 and 113 had already drifted
from 106, which is why "Röyksopp" rendered as "Ryksopp".

**No comments in the file itself.** `msdf-atlas-gen` rejects a charset file
containing `#` lines with "Failed to load character set specification", hence
this README rather than inline notes.

## What the ranges cover

| entry | why |
|---|---|
| `[0x20, 0x7e]` | ASCII |
| `0xab`, `0xbb` | « » guillemets, French quotation |
| `0xb0` | ° degree sign, used by the latitude readout |
| `[0xc0, 0xff]` | the Latin-1 letters — French accents and cedilla, Nordic vowels and ring, German umlauts and sharp s, with Spanish and Portuguese tildes coming free |
| `0x152`, `0x153` | Œ œ ligature, French |
| `0x160`, `0x161` | Š š caron, for Hošek-Wilkie |
| `0x178` | Ÿ, French |

A range is used for Latin-1 rather than a list of individual codepoints because
the atlas cost is small and the alternative is a list nobody maintains.

## Re-baking

Needs `msdf-atlas-gen` (`brew install msdf-atlas-gen`) and ImageMagick's
`magick`. Run each example's `tools/bake-text-atlas.sh`. **Re-bake all three
together** after editing this file, or they go out of step.
