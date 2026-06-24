# Raytracer — Step 2: Diffuse + antialiasing

Builds on step 1 (*Ray Tracing in One Weekend* chapters 7-9): each pixel
averages several **jittered samples** (antialiasing), surfaces are
**Lambertian diffuse** so rays **bounce** (recursive scatter, picking up each
surface's albedo), and the result is **gamma corrected**. Still single-threaded.

At Full HD this is genuinely heavy (samples × bounces × spheres per pixel), so
the **progressive fill is slow and clearly visible** — which is the point of
running at high resolution. Step 4 makes it parallel.

```bash
rae run --target compiled main.rae   # recommended (Live VM is much slower)
```

See `../40_raytracer_1_basic/README.md` for the full step roadmap.
