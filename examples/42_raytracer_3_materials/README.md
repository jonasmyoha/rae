# Raytracer — Step 3: Materials + camera + multithreading

*Ray Tracing in One Weekend* chapters 10–13 — the iconic Book-1 image:

- **Metal** (reflection + fuzz) and **dielectric / glass** (refraction with
  Schlick-approximated Fresnel) materials, alongside the Lambertian diffuse
  from step 2.
- A real **positionable camera**: position + look-at target + vertical FOV +
  **defocus blur** (aperture / depth-of-field), in **Blender Z-up** world space
  (see `../../docs/coordinate-system.md`).

It is also the first **parallel** step. The screen is split into **four
quadrants, one per worker thread** — each `spawn renderTile(...)` worker renders
only its own quadrant, one small square tile at a time. So the four tiles in
flight are always in four separate regions of the screen (Blender-style
buckets), which makes the parallelism visible rather than four adjacent tiles
that look like one block. Each worker runs on a real OS thread in the Compiled
backend with its own deep copy of the scene + camera and its own seeded RNG
stream, so there is no shared mutable state. The image fills in progressively
via the streaming texture.

```bash
rae run --target compiled main.rae   # recommended — real threads
```

The scene is the classic four-sphere setup: a diffuse ground, a diffuse centre
sphere, a glass sphere on the left, and a fuzzed metal sphere on the right.

See `../40_raytracer_1_basic/README.md` for the full step roadmap (step 4 adds
an interactive camera with continuous progressive accumulation).
