# lib/assets/fonts attribution

## MaterialIconsRound-Regular.otf, .codepoints

Material Icons (Round) by Google, Apache License 2.0
(https://www.apache.org/licenses/LICENSE-2.0), from
https://github.com/google/material-design-icons (`font/`). The `.codepoints`
file is the one published beside the font there: one `name hex` line per icon.

## MaterialIconsRound-Regular.mtsdf.json, .mtsdf.png

Baked from the font above by `tools/gen-material-icons.py --bake`
(msdf-atlas-gen: every codepoint, 40 px per em, distance range 10, 2048x2048).
The same tool writes `lib/ui/IconCodepoints.rae` from the `.codepoints` file.
