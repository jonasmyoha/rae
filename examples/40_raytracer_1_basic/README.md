# Raytracer — Step 1: Basic (single-threaded)

A barebones CPU raytracer in the *Ray Tracing in One Weekend* style
(chapters 4-6): spheres on a ground plane shaded by surface normal, over a
sky gradient. One ray per pixel, no materials, no bounces, single-threaded.

It renders at **Full HD (1920×1080)** and shows the image **progressively** —
each frame renders a time-bounded chunk of scanlines into a streaming texture
(`loadStreamTexture` / `updateStreamTexture`), so you watch it fill in
top-to-bottom rather than waiting for one dump at the end.

```bash
rae run --target compiled main.rae   # recommended
rae run main.rae                      # Live VM
```

## The series

This is step 1 of a staged raytracer that follows the three *Ray Tracing in
One Weekend* books, one new concept per step:

1. **Basic** — normal-shaded spheres + sky (this example), single-threaded.
2. **Diffuse + antialiasing** — Lambertian materials, bounce recursion, gamma.
3. Metal + dielectric materials, positionable camera, defocus blur.
4. **Multithreading** — real-thread band workers (the concurrency showcase).
5. Lights + a Cornell-box-style scene.
