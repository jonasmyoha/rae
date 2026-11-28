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
| the browser | the page's emoji font (Apple Color Emoji, Segoe UI Emoji or Noto Color Emoji) | a 2D canvas's `fillText` (an OffscreenCanvas where there is one), read back with `getImageData` (an EM_JS shim in `runtime_emoji.h`; its bytes are straight alpha) |
| Linux, the BSDs | Noto Color Emoji's file (`RAE_EMOJI_FONT`, else the distributions' usual places) | Rae reads the font itself (`lib/text/ColorEmojiFont`): the code points to glyphs through its cmap (format 12), a sequence to one glyph through its GSUB ligatures (type 4, also inside a type 7 extension), the glyph's PNG through CBLC / CBDT (index formats 1 and 3, image formats 17, 18, 19), decoded by `lib/Png` and fitted by `lib/ImageScale`. The font's tables are read once onto the canvas, a few tens of KB, and each glyph's PNG when it is first drawn; the file is never read whole |
| Windows | none yet | the emoji keeps its place in the line and shows nothing |

`RAE_EMOJI_FONT=<a CBDT font file>` makes every platform draw from that file instead (how the Linux
path is checked on a Mac). A sequence the font has no ligature for shows its first picture.

The CPU framebuffer text (`SdfText.sdfDrawText`) keeps the emoji's place and draws nothing.

## Checked

- Case 1715: the clusters of a mixed line, the measure (the text plus one square an emoji), the
  bounds, a wrap between whole emoji, and the Mac's rasterizer (a warm fire, a ZWJ family).
- `checks/uiTextEmoji`: a page of mixed lines through the real pipeline, every cluster one image
  made once. Run on this Mac (Apple Color Emoji), on an Android 15 emulator (Noto Color Emoji) and
  as a browser build in headless Chrome, with a look at a frame of each. Its page is in its own
  `assets/` and its font is `lib/assets/walker`'s, which a browser build carries.
- Case 1716: `lib/Png` decodes every color type and bit depth (a color emoji font's PNGs are 4-bit
  palettes with alphas), as lodepng does.
- Case 1717: a tiny CBDT font (made by its `makeFont.py`): the cmap, a ligature lookup, one inside
  an extension, the glyphs' pictures, and a missing or broken file.
- `checks/uiTextEmoji` with `RAE_EMOJI_FONT` naming a Noto Color Emoji file: the whole page from the
  font file through Rae, on this Mac (about 1 ms an emoji to decode and fit).
- `checks/webEmoji`: the platform's rasterizer alone (a fire, a flag, a ZWJ family), natively and
  in headless Chrome (`compiler/tools/wasm_webgpu_smoke.sh`).
