# 40 — Multithreaded raytracer

A small CPU raytracer in the *Ray Tracing in One Weekend* style: spheres on a
ground plane, shaded by surface normal over a sky gradient, displayed in a
raylib window.

It demonstrates **Rae's concurrency**. The image is split into four horizontal
bands, each rendered by a `spawn renderBand(...)` worker:

```rae
let ta: Task(List(Int)) = spawn renderBand(scene: buildScene(), width: w, height: h, y0: 0,  y1: r1)
let tb: Task(List(Int)) = spawn renderBand(scene: buildScene(), width: w, height: h, y0: r1, y1: r2)
...
let pa: List(Int) = ta.get()   # join
```

Each worker takes the scene by `own List(Float)`, so it receives its **own deep
copy** with no shared state, and returns its rows as a `List(Int)` of packed
`0xRRGGBB` pixels. In the **Compiled (C) backend** each worker runs on a real
OS thread; in the **Live (bytecode VM)** backend the same code runs
sequentially with identical results.

## Run

```bash
rae run --target compiled main.rae   # real OS threads
rae run main.rae                      # Live VM (sequential, identical image)
```

## Notes

- The scene uses a flat `List(Float)` (`[cx,cy,cz,r, ...]`, struct-of-arrays)
  so the list element type stays primitive (unboxed) across the thread
  boundary.
- The render core (`hitSphere` / `rayColor` / `renderBand`) is pure compute and
  is verified headlessly — `tests/cases/361_spawn_raytracer_bands` asserts the
  4-band parallel render is byte-identical to the single-threaded reference on
  both backends.
