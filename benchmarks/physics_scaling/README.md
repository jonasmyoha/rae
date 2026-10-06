# Physics scaling benchmark

Step time of the physics playground's box pyramid (Box3D's large pyramid) on
both physics tracks, against body count (1k to 20k) and worker count (1 to 8):

- **track A**: the Rae port of Box3D, `examples/122_physics_playground_port`
- **track B**: Box3D's own C library through Rae's ECS, `examples/123_physics_playground_c`

The cases run the playground itself, headless: `RAE_PLAYGROUND_SCALING=<counts>`
builds the pyramid at each count, turns sleeping off (as Box3D's own
large-pyramid benchmark does), settles 20 fixed 60 Hz steps (4 sub-steps) and
prints the mean and median of the next 100. The worker pool's size is per
process (`RAE_WORKERS`), so each worker count is its own run of the same build.

## Run

```sh
sh benchmarks/physics_scaling/run.sh                       # every count and worker count
COUNTS=1000,5000 WORKERS="1 8" sh benchmarks/physics_scaling/run.sh
```

Each track is built once with `rae run` (about 100 s; the binary is kept in
`build/` through `RAE_RUN_KEEP_BINARY`) and rerun for the other worker counts.
The script refuses a loaded machine (`BENCH_ALLOW_LOAD=1` overrides) and
records the load in `results/metadata.json`. It writes `results/raw.csv` and
regenerates `site/index.html`, which the devtools Benchmarks tab shows
("Physics scaling"). The analysis is in `docs/physics-performance-plan.md` §10e.
