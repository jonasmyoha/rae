# Frame profiling (Live captures → Perfetto)

A capture-and-view profiler for finding where per-frame time goes on **desktop
and iPhone**. It records nested CPU timing **zones** and **counters** during a
time-boxed capture and writes a **Chrome Trace Event JSON** file you open in
[ui.perfetto.dev](https://ui.perfetto.dev) (or `chrome://tracing`). There is no
in-engine viewer on purpose: Perfetto gives a task/thread-aware timeline **and a
full SQL query engine** over the trace, which is exactly the "capture, then run
queries to see where the time goes" workflow — and it is far better than
anything we would build.

The capture layer is `lib/profile.rae`. Overhead when not capturing is a single
bool test per zone; all string/file work is deferred to the end of the capture,
so it never skews the frames it measures.

## Capturing on desktop

Set `RAE_PROFILE` and run the example — the capture auto-starts at launch and
writes the trace when the window elapses:

```bash
RAE_PROFILE=1 RAE_PROFILE_SECONDS=30 RAE_PROFILE_OUT=/tmp/rae_profile.json \
  ./compiler/bin/rae run examples/114_walker_character
```

Walk around, let the sun set (the case where 114 drops from 60→~20 fps), and at
30 s the trace is written to `/tmp/rae_profile.json`. Env vars:

| Variable | Default | Meaning |
|---|---|---|
| `RAE_PROFILE` | (unset) | Any non-empty value starts a capture at launch |
| `RAE_PROFILE_SECONDS` | `20` | Capture length in seconds |
| `RAE_PROFILE_OUT` | `rae_profile.json` | Output path (relative to the app's cwd) |

## Capturing on iPhone

The app's working directory is its `Documents/` folder (the iOS bootstrap
`chdir`s there), so a capture lands next to `rae.log` and is pulled the same way.
Passing environment variables to a side-loaded app is not as simple as on
desktop — wiring a launch-arg / on-device trigger is tracked as **#527**. Until
then, capture on desktop, which reproduces the same sunset fps drop.

## Viewing

1. Open <https://ui.perfetto.dev> (works offline once loaded).
2. **Open trace file** → pick your `.json`.
3. The `CPU main loop` track shows each frame's passes as nested spans:
   `shadow`, `gbuffer`, `ssao`, `depthPyramid`, `lighting`, `taa`, `composite`,
   `uiOverlay`, `present`. Counters (e.g. `fps`) plot as their own tracks.

## Querying (the point of Perfetto)

Use **Query (SQL)** in the left panel. Total time per pass across the capture —
the "where is the time going" answer:

```sql
select name, count(*) as calls, sum(dur)/1e6 as total_ms, avg(dur)/1e3 as avg_us
from slice group by name order by total_ms desc;
```

How a pass grows over the capture (e.g. `shadow` as the sun lowers) — bucket by
wall-clock second:

```sql
select cast(ts/1e9 as int) as sec, sum(dur)/1e3 as us
from slice where name = 'shadow' group by sec order by sec;
```

If `present` balloons while the CPU passes stay flat, the frame is **GPU-bound**
(present is where the CPU blocks on the GPU) — and the pass whose GPU cost grows
with the sun's elevation is the culprit. The prime suspect for 114 is
`fitShadowCascades` enlarging the light frustum at a grazing sun, so each cascade
rasterizes a larger world-space area (`lib/shadow3d.rae`, `lib/gbuffer_shadow.rae`).

## What the zones measure (and don't)

Phase 1 zones are **CPU wall-clock** around each render-graph pass — they bound
the CPU submit cost and, via `present`, reveal CPU-vs-GPU-bound. They do **not**
attribute time *inside* the GPU. Exact per-pass GPU milliseconds need **WebGPU
timestamp queries** (a second, GPU track in the same trace) — tracked as **#528**
and the real payload for the Instruments pass (#524). The bindings already exist
(`wgpuCommandEncoderWriteTimestamp`, `wgpuComputePassEncoderWriteTimestamp`), so
this stays Rae-over-WebGPU with no new renderer C surface.

## Adding zones to your own app

```rae
import profile
# once at startup:
profile.profileMaybeAutoStart()
# each frame:
profile.profileTick()                 # auto-stops + writes when the window elapses
profile.zoneBegin(name: "myPhase")    # strictly nested (LIFO) begin/end
...work...
profile.zoneEnd(name: "myPhase")
profile.profileCounter(name: "fps", value: fps)
```
