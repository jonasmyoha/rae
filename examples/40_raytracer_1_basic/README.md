# Raytracer — Step 1: Basic (single-threaded, SDL3)

A barebones CPU raytracer in the *Ray Tracing in One Weekend* style
(chapters 4-6): spheres on a ground plane shaded by surface normal, over a
sky gradient. One ray per pixel, no materials, no bounces, single-threaded.

This is the **SDL3** port — the same render core, presented through an SDL3
window + streaming texture (`lib/sdl3.rae`) instead of raylib. It renders at
**1280×720** and shows the image **progressively** — each frame renders a
time-bounded chunk of scanlines into the streaming texture
(`sdlUpdatePixels` / `sdlPresent`), so you watch it fill in top-to-bottom
rather than waiting for one dump at the end.

Compiled-target only (the Live VM has no SDL bindings). Needs `brew install sdl3`.

```bash
rae run --target compiled main.rae
```

Headless render (no window — saves the final frame as a BMP):

```bash
SDL_VIDEODRIVER=dummy RAE_SDL_SCREENSHOT=/tmp/out.bmp RAE_SDL_HEADLESS_MS=4000 \
  rae run --target compiled main.rae
```

## The series

This is step 1 of a staged raytracer that follows the three *Ray Tracing in
One Weekend* books, one new concept per step:

1. **Basic** — normal-shaded spheres + sky (this example), single-threaded.
2. **Diffuse + antialiasing** — Lambertian materials, bounce recursion, gamma.
3. Metal + dielectric materials, positionable camera, defocus blur.
4. **Multithreading** — real-thread band workers (the concurrency showcase).
5. Lights + a Cornell-box-style scene.
