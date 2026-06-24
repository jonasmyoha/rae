# Raytracer — Step 3: Materials + camera + multithreading

*Ray Tracing in One Weekend* chapters 10–13 — the iconic Book-1 image:

- **Metal** (reflection + fuzz) and **dielectric / glass** (refraction with
  Schlick-approximated Fresnel) materials, alongside the Lambertian diffuse
  from step 2.
- A real **positionable camera**: position + look-at target + vertical FOV +
  **defocus blur** (aperture / depth-of-field), in **Blender Z-up** world space
  (see `../../docs/coordinate-system.md`).

It is also the first **parallel** step. Each frame, a band of scanlines is
split across **four `spawn renderBand(...)` workers** that run on real OS
threads in the Compiled backend — each gets its own deep copy of the scene +
camera and its own seeded RNG stream, so there is no shared mutable state. The
image still fills in progressively, a band per frame, via the streaming
texture.

```bash
rae run --target compiled main.rae   # recommended — real threads
```

The scene is the classic four-sphere setup: a diffuse ground, a diffuse centre
sphere, a glass sphere on the left, and a fuzzed metal sphere on the right.

See `../40_raytracer_1_basic/README.md` for the full step roadmap (step 4 adds
an interactive camera with continuous progressive accumulation).
