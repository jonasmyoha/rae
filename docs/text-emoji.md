# Color emoji in text

Rae's text is MSDF glyphs from a font atlas (lib/SdfText, lib/Gpu2dText), and an atlas holds no
color emoji. A `Text` node (and every `Gpu2dText` draw) still shows them: an emoji is drawn as a
color image made by the platform's own emoji font.

```json
"Emote": { "Text": { "text": "GG 🔥 👍🏽 🇫🇮", "styleId": "line" } }
```

## What counts as one emoji

A character the font has **no glyph for** that is a picture (Unicode's Extended_Pictographic) or a
flag's letter (a regional indicator) starts an emoji. The emoji is its whole grapheme
(lib/text/Graphemes): a skin tone, a flag's two letters, a ZWJ family (👨‍👩‍👧), a variation
selector (⚡️). It is one piece everywhere:

- measured as one square (`SdfText.sdfMeasureText`, `sdfMeasureTextBounds`, so the layout, the
  text fields' caret and the cut to a box see it): the advance is 1.2 em, the square 1.15 em,
  reaching 0.93 em above the baseline;
- never split by a wrap (`Gpu2dText.nextWrapLine`);
- drawn as one image (`Gpu2dText.emitRange`).

A font that has a glyph for the character (a heart, a star, ©) keeps drawing its own glyph.

## How it is drawn

`Gpu2dEmoji.canvasDrawEmoji` asks the platform to draw the cluster into a square at about its size
on screen (24, 32, 48, 64, 96, 128 or 160 pixels: the smallest at least the size, from the canvas's
pixels per point), uploads the square once as an image under `emoji:<cluster>@<pixels>`, and draws
that image from then on. A cluster the platform cannot draw is remembered
(`Gpu2dCanvas.emojiUnavailable`) and not asked again. The image draw keeps the paint order with the
text around it (the canvas flushes between them).

The text's passes:

- the text itself: the emoji in its own colors, at the text's opacity;
- a shadow: the emoji's silhouette in the shadow's color (not blurred);
- an outline: no emoji (the outline is the glyphs' own; the emoji covers its place).

`emitRun` and `emitRange` take that tint as `emojiTint` (`Gpu2dText.emojiTintOf(color:)` for the
text, the shadow's color, or 0).

## The platforms

| platform | the emoji font | how |
|---|---|---|
| macOS, iOS | Apple Color Emoji | CoreText draws the cluster (`compiler/runtime/runtime_emoji.h`; CoreText is loaded at run time, so a program links nothing new) |
| Android | the system's emoji font (Noto Color Emoji, or the maker's) | Java's text stack draws it (`tools/android/java/org/rae/app/RaeEmoji.java`), reached through JNI |
| Linux, the browser, Windows | none yet | the emoji keeps its place in the line and shows nothing |

The CPU framebuffer text (`SdfText.sdfDrawText`) keeps the emoji's place and draws nothing.

## Checked

- Case 1715: the clusters of a mixed line, the measure (the text plus one square an emoji), the
  bounds, a wrap between whole emoji, and the Mac's rasterizer (a warm fire, a ZWJ family).
- `checks/uiTextEmoji`: a page of mixed lines through the real pipeline, every cluster one image
  made once. Run on this Mac (Apple Color Emoji) and on an Android 15 emulator (Noto Color Emoji),
  with a look at a frame of each.
