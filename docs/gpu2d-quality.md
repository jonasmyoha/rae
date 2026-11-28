# The 2D canvas's quality levers

A game with graphics tiers (high, medium, low) scales textures, the display and effects to the
phone, so a low-end phone holds its frame rate and does not run out of memory while a top phone
gets everything. The 2D canvas (lib/Gpu2d, lib/Gpu2dCanvas) gives the app two levers, set once at
boot from its saved quality setting. MSAA is not a lever of the 2D canvas: it has none (see the
end).

## The image size cap

```rae
Gpu2d.setImageSizeCap(canvas: canvas, threshold: 1536, target: 1024)
```

An image file whose longer side is at least `threshold` pixels is decoded, made small so that side
is `target` pixels (the aspect kept), and uploaded at that size. It holds for every image the canvas
loads from a file: those a scene mount loads (lib/ImageRegistry) and `Gpu2d.loadImageKey` alike.
`threshold` 0 turns it off. Set it on the canvas before the first load: an image loaded already
keeps its size. A draw is not changed: a sprite is as large as its node, so a capped image is drawn
at the same size, from fewer pixels. `Gpu2d.imageKeyWidth` / `imageKeyHeight` give the size it was
uploaded at.

- **Memory:** the biggest lever. An RGBA8 texture is 4 bytes a pixel: a 2048 x 2048 image is 16 MiB
  on the GPU, a 1024 x 1024 one 4 MiB, a quarter. A scene of ten such images goes from 160 MiB to
  40 MiB.
- **Load time:** the image is made small on the CPU after it is decoded, each new pixel the mean of
  the 2 x 2 (in general the box) it covers, weighed by alpha (lib/ImageScale, the same code as the
  thumbnails). A 2048 x 2048 image takes about 10 ms more on an Apple M-class Mac; a phone takes
  longer. It is paid once, at load.
- **Look:** fine detail under the target's resolution is lost; an image drawn larger than half its
  file size on a 3x phone shows it.

The cap is in `canvasLoadImage` (lib/Gpu2dCanvasImage/Load.rae); a picture registered from bytes or
pixels made in the program (`registerImageBytes`, `registerImageRgba`) is not capped: the program
chose its size. Case 1711 checks the rule and the resize without a GPU; checks/gpu2dQuality loads a
2048 x 1024 PNG under a 1024 cap and gets a 1024 x 512 texture, drawn at the size the draw asks for.

## The render scale cap

```rae
Gpu2d.setRenderScaleCap(cap: 1.5)     # before initWindow
Gpu2d.initWindow(width: 393, height: 852, title: "Game")
```

The window is drawn at most `cap` pixels a point and shown scaled up by the system (the CAMetalLayer
stretches the smaller drawable over the window). `Gpu2d.dpr()` is the capped ratio, so the canvas
draws its design units at it. `0` is the display's own ratio. Set before the window it is the size
the surface is first made; set later (from a settings menu), the surface is made again at once and
`Gpu2d.windowResized()` says so, as after a resize.

- **Fill rate:** the pixels drawn are the scale squared: a 3x phone capped at 2 draws 44% of its
  pixels, at 1.5 a quarter; a 2x display capped at 1.5 draws 56%, at 1 a quarter. Every pass of the
  frame shrinks with it (the boxes, the images, the text, a backdrop blur).
- **Look:** text and edges are softer, as the frame is scaled up; at 2 on a 3x phone it is hard to
  see, at 1 it is plain.
- **Platforms:** the Mac and iOS (Metal) and the desktop. Not on Android yet: its window units are
  pixels and its ratio the display density, and a smaller drawable there needs the native window's
  buffers set to that size too (ANativeWindow_setBuffersGeometry); until then the cap is ignored
  there.

checks/gpu2dQuality makes the window under a cap of 1 and reads a ratio of 1, then lifts the cap and
reads the display's (2 on a Retina Mac), with the window resized. A frame under the cap is drawn at
the window's size in points (400 x 300 for a 400 x 300 window on a 2x display, 800 x 600 without
it; read with `RAE_GPU2D_SCREENSHOT`).

## MSAA

The 2D canvas does not use MSAA: its pipelines draw one sample a pixel (`sampleCount: 1`), and a
rounded box, a border, an image's corner and a glyph are antialiased in their shaders (by their
distance to the edge). There is nothing to turn off on a low tier and nothing it costs on a high
one. A 3D renderer over the canvas (lib/gpu) has its own sample count.
