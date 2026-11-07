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
  ./compiler/bin/rae run examples/136_walker_character
```

Walk around, let the sun set (the case where 114 drops from 60→~20 fps), and at
30 s the trace is written to `/tmp/rae_profile.json`. Env vars:

| Variable | Default | Meaning |
|---|---|---|
| `RAE_PROFILE` | (unset) | Any non-empty value starts a capture at launch |
| `RAE_PROFILE_SECONDS` | `20` | Capture length in seconds |
| `RAE_PROFILE_OUT` | `rae_profile.json` | Output path (relative to the app's cwd) |

## Capturing on iPhone

Pick **iOS device (profiler)** in the devtools target dropdown, or run:

```bash
RAE_IOS_PROFILE=30 sh tools/ios/run-ios.sh examples/136_walker_character
```

It builds + installs + launches with the capture on, prints `PLAY NOW`, and you
walk around / let the sun set for ~30 s. `run-ios.sh` then pulls the trace off
the device to `/tmp/rae_profile.json` (override with `RAE_PROFILE_LOCAL`) — open
it at ui.perfetto.dev exactly like a desktop capture.

Mechanics: `run-ios.sh` launches via `devicectl … process launch -e '{"RAE_PROFILE":"1",…}'`;
the iOS bootstrap points `RAE_PROFILE_OUT` at the app's **writable** `Documents/`
dir (the cwd is the read-only bundle), so the trace lands next to `rae.log`; it
is retrieved with `devicectl device copy from --domain-type appDataContainer`.

The plain **iOS device** target (no profiler) launches normally with no capture.

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

## One command: profile a run and read the summary

```sh
tools/profile-desktop.sh examples/143_walker_forest/Main.rae 40      # this machine
APP=<bundle id> tools/ios/profile-ios.sh <device id> 80              # a phone
```

Both print the same summary (`tools/profile-summary.py`), one block a place: the frames, the CPU
time, what each pass drew and the GPU slots. So a comparison can start on the Mac. Both stop at the
first run that fails and at a total time limit (`TOTAL_SECONDS`, 30 minutes), say so with a `STOPPED`
line, summarize what they have and exit 1. A failed launch stops the phone's profile at once: a locked
phone fails the same way at every place. Options are in each script's header.

Compare the `scale` of two summaries before their times. Dynamic resolution stays on in a profile, so
a run can settle at a lower scale: 143 on a Mac settled at 0.50, uncapped. `RAE_AB` holds the scale still.

## What to trust

**A `[gpu passes]` slot is not what a pass costs.** It is the time between two timestamps, and on a
tile-based GPU (every phone, and an Apple Silicon Mac) a pass's vertex and tile work, and the GPU's
idle time, land in later slots. On a phone a slot read "ssao 31 ms" while switching SSAO off saved
1 ms. On a Mac, in one launch of 143 with nothing changed (2026-11-06):

| what | read |
|---|---|
| the `underwater` slot, with the camera above the water | 178 to 3,607 us |
| the `antialias` slot | 218 to 2,968 us |
| SSAO off: its slot, the `lighting` slot, the frame | down 2.2 ms, up 1.5 ms, 0.11 ms saved |

**A timestamp at each encoder's start and end does not fix it.** The clouds' passes have exactly that
(`CloudTiming`, the `[clouds gpu]` line), and with nothing changed one pass read 395 to 5,751 us over
one launch, and the tier's total read 0 to 6,859 us. So the other passes were not moved to it. That is
measured on a Mac only, and on the clouds' passes only.

| trust | for |
|---|---|
| `RAE_AB` | what a feature costs: the same case gave 1.54 and 1.82 ms in one launch |
| the frame times of `[frames]` | whether a frame fits, over a whole run |
| the `[draws]` counts | what was drawn: exact, checked against the renderer's own counters |
| the `cpu` time of `[frame]` | the CPU's part of a frame |
| a `[gpu passes]` slot | only a hint at where to look first; use `RAE_AB` before acting on one |

## What each pass drew: the `[draws]` lines

`RAE_FRAME_TIMING=1` in a walker app logs, after each `[frames]` line (every 5 s), one line a pass
with what the last frame drew, by kind (lib/DrawStats, app3d/FrameLog):

```
[draws] gbuffer: 10479 instances, 1027988 triangles; ground 314 instances 411360 triangles, rocks ...
[draws] shadow cascade 0: 981 instances, 421598 triangles; rocks 235 instances 23500 triangles, ...
```

Resident triangles say what is loaded. These say what a frame costs, so read them first when a frame
is slow: 7 million triangles of flowers once went unnoticed because only resident triangles were
logged.

- **The passes:** `gbuffer` on the deferred path; `pre-pass` and `forward` on the forward path; one
  `shadow cascade <n>` line each. Every cascade draws every caster today, so the cascade lines are
  equal, and the real shadow cost is one line times the cascade count.
- **The kinds:** `ground`, `rocks`, `bushes`, `meadow`, `forest` (with its billboards, two triangles
  each), `characters`, `cubes`, `grass`, `marks` (footprints, particles, rain and snow) and
  `referenceTree`. A draw whose caller named no kind is `other`. A new thing the walker draws should
  name its kind, or it lands in `other`.
- **Grass** is drawn by a count on the GPU, so once a line the log waits for one read of it. That can
  add one slow frame in each 5 s under `RAE_FRAME_TIMING`, and none without it.
- **Not counted:** the water, what is drawn after the scene passes, and metaball casters. An app with
  its own frame loop gets no lines unless it calls `frameLogDraws`, and its draws are all `other`.

## Comparing features in one launch: `RAE_AB`

To learn what a feature costs, switch it off. `RAE_AB` does that in one launch of a walker app, on any
device, with no restarts and no tools (app3d/AbTest):

```sh
RAE_FRESH_START=1 RAE_PRESENT_MODE=immediate \
  RAE_AB="shadows;forest,grass;ssao" ./rae run examples/143_walker_forest/Main.rae
```

Cases are separated by `;`, and each is a comma list of what is off for that case. The app holds a
baseline (nothing off), the first case, a baseline, the next case, and so on, ending on a baseline.
Each is held 9 s, of which the first 3 s are skipped. One line a phase, then a table, and the app ends:

```
[ab] baseline: 15.78 ms over 382 frames
[ab] forest,forestShadows,props,grass: 11.69 ms over 515 frames
[ab] baseline: 16.07 ms over 375 frames
[ab] the time saved, against the baselines before and after each case:
[ab] forest,forestShadows,props,grass: 11.69 ms, baselines 15.78 ms and 16.07 ms, saved 4.24 ms
```

Each case is judged against the two baselines beside it, so a slow drift such as a phone heating up
cancels. Those were real numbers, and that run's baselines moved by up to 1.8 ms: separate launches
cannot see a saving of that size.

| switch | what stops |
|---|---|
| `shadows` | the whole shadow pass (the lighting still samples the last shadow maps) |
| `forestShadows` | the forest's shadows only |
| `props` | the rocks, the bushes and the meadow: gathering, drawing and shadows |
| `forest` | the forest: gathering, drawing and shadows |
| `ground` | the ground's draws |
| `grass` | the GPU grass: its compute and its draw |
| `characters` | the characters' draws and shadows (their animation still runs) |
| `cubes` | the physics cubes' draws and shadows |
| `marks` | footprints, particles, rain and snow |
| `ssao`, `bloom`, `depthPyramid` | that pass (what reads its result reads the last one) |
| `water` | the FFT waves, the water's draw and the opaque snapshot it reads |
| `clouds` | the sky's cloud layer: the sky is drawn with no cover and no cirrus (the cloud shadow map still draws) |

An unknown name logs `[ab] error:` with the list, and the app ends at once. A new feature worth
comparing gets a field in `AbSwitches`, a line in `abSwitchOff` and an `if` at its draw.

**Reading it:**

- **Say where to stand.** A saving depends on what is in view. A windowed run restores the walker's
  saved place, which may have no trees in it, so `forest` would save nothing. `RAE_FRESH_START=1` starts
  at the spawn. `RAE_WALKER_CAMERA` fixes the view.
- **Uncap the frame.** With vsync a frame that fits the display's interval still takes the whole interval,
  so a saving does not show. On a Mac use `RAE_PRESENT_MODE=immediate`. A phone over budget needs nothing.
- **Learn the noise.** Add a case that does almost nothing (`marks`): what it "saves" is the noise of that
  run. It was -0.07 ms in the run above, but savings under about 1 ms were noise on a busy machine. Close
  other work first.
- **A switch cannot find a cost outside its name.** With every switch off at once, a 4 megapixel Mac frame still
  took 13 ms: the anti-aliasing, the lighting, the clouds and the UI have no switch.
- **The dynamic resolution holds still** for the whole run, or it would hand a saving back as resolution, and
  frames are always drawn, so a skipped frame does not count as a wait. A measured fall in the cloud tier
  can still happen; it would show as a step in the baselines.
- With `RAE_AB` unset the frame reads a few false Bools and nothing else changes.

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
