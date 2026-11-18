# Profiling Rae programs

Rae measures a running program with one recorder and reads it in several places. This page says
what each piece is, how to use them for a game, an audio app and a threaded program, and which
numbers to trust. The design and its history are in docs/perf-tools-design.md.

## The pieces

| piece | what it is | where |
|---|---|---|
| the recorder | small fixed-size events (a zone's begin and end, a counter, a mark, a wait, a GPU span) written by each thread into a ring of its own, collected once a frame into a window of the last seconds; a write costs one atomic load and a push, and nothing allocates | `lib/perf/Perf`, through `lib/Profile` in an app |
| what records by itself | each ECS system that runs (and a mark when it is skipped), the worker pool's chunks and tasks with their spawn flows, every wait (Task.get, a Signal, a lock, a channel), named threads, every presented frame (start, present, pacing counters), GPU pass spans and Metal command buffers, memory and allocations each frame, the audio engine's blocks and nodes (when the app calls `recordAudioCapture`), real-time rule breaks | the runtime, `lib/ecs/Schedule`, `gpu/FramePacer`, `lib/Profile`, `audio/AudioCapture` |
| the capture file | a capture saved as `.raeperf`: the header (program, machine, Rae version), the names, the lanes, and the events in blocks | `RAE_PERF_OUT=<file>`, `lib/perf/PerfFile` |
| the live link | the same capture streamed over TCP while it runs, to a viewer on any machine; a slow network drops blocks and counts them, it never makes the program wait | `RAE_PERF_LIVE=<host>:<port>`, `lib/perf/PerfLive` |
| the perf HUD | a panel over any app with a debug overlay: frame times against the refresh budget (CPU and GPU), a bar per thread (busy, waiting, idle), the audio block's share and the rule breaks, memory and allocations a frame, and a button that saves the last 10 s as a `.raeperf`; closed, it costs nothing | the debug button, then "Performance"; `RAE_PERF_HUD=1` opens it at launch (`lib/ui/PerfHud`) |
| Rae Perf | the viewer: Frames, Timeline, Flame graph, Threads (with the slowest frame's critical path), GPU, Audio, Memory, Systems, Compare (two captures, or an `RAE_AB` run's settings) and Sources (live programs, a folder of captures) | `examples/151_perfViewer`, the devtools page's Perf tab |
| reports and budgets | a capture's report in text, a profile run's summary, a frame-time check against this machine's baseline, and budget checks a fixture calls | `tools/perfReport`, `lib/perf/PerfBudget` |
| Perfetto | the deep dive: every event on a timeline with SQL over it | `rae run tools/perfExport/Main.rae -- capture.raeperf trace.json`, or the `.json` a capture writes beside it |

## The commands

```sh
# a capture of the first 20 s, as a trace and as a capture file
RAE_PROFILE=1 RAE_PROFILE_SECONDS=20 RAE_PROFILE_OUT=/tmp/run.json RAE_PERF_OUT=/tmp/run.raeperf \
  ./compiler/bin/rae run examples/143_walker_forest/Main.rae

# look at it
RAE_PERF_OPEN=/tmp/run.raeperf ./compiler/bin/rae run examples/151_perfViewer/Main.rae
./compiler/bin/rae run tools/perfReport/Main.rae -- capture /tmp/run.raeperf

# live: start the viewer (it listens on port 7470), then the program
./compiler/bin/rae run examples/151_perfViewer/Main.rae
RAE_PROFILE=1 RAE_PERF_LIVE=127.0.0.1:7470 ./compiler/bin/rae run examples/143_walker_forest/Main.rae

# Perfetto
./compiler/bin/rae run tools/perfExport/Main.rae -- /tmp/run.raeperf /tmp/run.json
```

| variable | default | meaning |
|---|---|---|
| `RAE_PROFILE` | unset | any value starts a capture at launch (an app that calls `Profile.maybeAutoStart`: 106, 121, the walkers) |
| `RAE_PROFILE_SECONDS` | `20` | how long the capture runs; a program that ends sooner writes what it has |
| `RAE_PROFILE_OUT` | `rae_profile.json` | the Perfetto trace (on iOS in the app's Documents folder) |
| `RAE_PERF_OUT` | unset | also the capture file; under `RAE_AB` one per setting beside it |
| `RAE_PERF_LIVE` | unset | stream the capture to `<host>:<port>` while it runs |
| `RAE_PERF_HUD` | unset | `1` opens the perf HUD at launch |
| `RAE_PERF_OPEN`, `RAE_PERF_COMPARE`, `RAE_PERF_VIEW` | the walker sample | the viewer's capture, a second one to compare, the first view |
| `RAE_PERF_LISTEN`, `RAE_PERF_FOLDER` | `7470`, the samples | the viewer's port (`0` any free one, `off` none) and the folder its Sources tab lists |

The devtools page's **Perf** tab shows whether Rae Perf runs, the programs streaming to it, and a
button that opens it (the viewer writes its state to `/tmp/rae-perf-viewer.json`,
`RAE_PERF_STATUS`, once a second).

## How to: a game

1. **Look first.** Open the perf HUD (the debug button, Performance). The frame graph says whether
   frames fit the refresh budget and whether the CPU (blue) or the GPU and the display (orange)
   takes the time. The thread bars say whether the workers are busy or the main thread waits.
2. **Capture the slow moment.** Press "Save last 10 s" right after it, or run with `RAE_PROFILE=1
   RAE_PERF_OUT=…`. Open the file in Rae Perf: Frames shows the slow frames and the spread, Systems
   the zones by total time (and how often a system was skipped), the Timeline what ran in a frame,
   and GPU how many frames the CPU, the GPU or the drawable bounded.
3. **Find what a feature costs** with `RAE_AB` (below), not with a GPU pass's slot: on a tile GPU a
   slot is not a cost. With `RAE_PERF_OUT` set, the viewer opens the run's capture as the baselines
   against the first case and its Compare view lists every zone's change, most certain first.
4. **Hold the line.** `rae run tools/perfReport/Main.rae -- regress run.raeperf` compares the frame
   p95 with the program's baseline on this machine (kept in `~/.rae/perf-baselines`, never in the
   repository) and exits 1 beyond its spread.

## How to: an audio app

1. **The rules.** Run with `RAE_REALTIME_CHECK=1`: an allocation, a lock or a log on the audio
   thread is counted, a mark in a capture names where it happened, and the perf HUD shows the count
   in red. `perfRealtimeBudget()` (lib/perf/PerfBudget) fails a fixture that broke one.
2. **The block against its period.** Call `recordAudioCapture(profiler:)` (audio/AudioCapture) once
   a frame while capturing: each block is a span on an "audio render" lane, each node of the render
   plan a zone ("sum 0", "reverb 7"), with `audio.render share %`, the underruns, the latency and the
   clock drift. The viewer's Audio view lists the nodes and graphs the share; the HUD shows it live.
3. **A budget in a test.** Render the scene offline and check
   `perfAudioBudget(renderNanoseconds: audio.takeRenderNanoseconds(), periodNanoseconds:
   audio.renderPeriodNanoseconds(), share: 0.5, calibrationNs: perfCalibrationNs())`, the calibration
   timed before the scene: a slower or busier machine gets a larger allowance, never a smaller one
   (cases 1401, 1450, 1456 do).

## How to: a threaded program

1. **Nothing to add.** A capture gives each pool worker a lane with its `parallelLoop` chunks and
   tasks, each `spawn thread` a lane named after its function, a thread C attached a lane named at
   `rae_thread_attach_named`, and every wait a span of its kind.
2. **Where the frame went.** The viewer's Threads view shows each lane's busy, waiting and idle
   share, the waits by kind, and the slowest frame's critical path: the main thread's work, the task
   on another thread a wait was for (found as the span that ended last inside the wait), and that
   task's time in the queue back to its spawn. A path that is mostly "waiting" or "in the queue"
   wants more parallel work or fewer waits, not faster code.
3. **Your own zones** on another thread: `profileThreadRing(profiler:, name:)` at setup gives the
   thread a ring and a lane; it writes with `perfZoneBegin`/`perfZoneEnd`/`perfCounter`
   (lib/perf/Perf), which never allocate or lock.

## Zones and counters in your own code

```rae
import Profile
# once, on the App: var profiler: Profiler = Profile.createProfiler(), then
Profile.maybeAutoStart(profiler: app.profiler)
# each frame:
let unusedWritten: Bool = Profile.tick(profiler: app.profiler)   # collects; ends a timed capture
Profile.zoneBegin(profiler: app.profiler, name: "physics")      # nest freely, begin and end in pairs
stepPhysics()
Profile.zoneEnd(profiler: app.profiler, name: "physics")
Profile.counter(profiler: app.profiler, name: "bodies", value: bodyCount)
```

Outside a capture each call is one Bool test. An app with a debug overlay also passes its profiler
to it once a frame after `tick` (`debugOverlayPerf`), which gives the perf HUD its threads and its
save button.

## One command: profile a run and read the summary

```sh
tools/profile-desktop.sh examples/143_walker_forest/Main.rae 40      # this machine
APP=<bundle id> tools/ios/profile-ios.sh <device id> 80              # a phone
```

Both print the same summary (`tools/perfReport`'s `logs`), one block a place: the frames, the CPU
time, what each pass drew and the GPU slots, so a comparison can start on the Mac. Both stop at the
first run that fails and at a total time limit (`TOTAL_SECONDS`, 30 minutes), say so with a
`STOPPED` line, summarize what they have and exit 1. A failed launch stops the phone's profile at
once: a locked phone fails the same way at every place. Options are in each script's header;
`tools/profile-tools-test.sh` checks both without a phone.

Compare the `scale` of two summaries before their times. Dynamic resolution stays on in a profile,
so a run can settle at a lower scale: 143 on a Mac settled at 0.50, uncapped. `RAE_AB` holds the
scale still.

## Capturing on an iPhone

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

A capture file comes off the phone the same way: set `RAE_PERF_OUT` to a name, and copy it from
the app's Documents folder. The perf HUD's save button writes there too. Live, a phone streams to a
Mac on the same network with `RAE_PERF_LIVE=<the Mac's address>:7470`.

## Perfetto: the deep dive

Rae Perf answers the usual questions. For anything else, export the capture (`tools/perfExport`)
or use the `.json` a capture writes, and open it at <https://ui.perfetto.dev> (Open trace file).
Process `CPU` has a lane per thread, process `GPU` the GPU spans and the `gpu.*` counters; waits are
spans of the category `wait`, and spawns are flow arrows.

### Querying

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

**Every frame's split: `RAE_FRAME_TIMING=all`** logs each frame as `RAE_FRAME_TIMING=1` logs only the slow ones
(`[frame] slow: N ms = cpu + drawable acquire + present + gpu wait`), for a run where the pattern from frame to frame
is the question. The clock reads whole milliseconds on an iPhone.

**The 2D canvas's draw calls: `RAE_GPU2D_DRAWS=1`** logs `[gpu2d] draws: N last frame, M a frame over 120 frames` every
120 frames: the draw calls of every render pass the canvas recorded (boxes, images, text, backdrops). On an iPhone 15
Pro 106's Home is 15 a frame and a walker's HUD 6 or 7 (docs/ui-layers-design.md §11).

**The 2D canvas: `RAE_GPU2D_TIMING=1`** (lib/Gpu2dCanvasTiming) times the one render pass a 2D frame draws in by
its own begin and end timestamps, read back without waiting, about every other or third frame: a `[gpu2d] frame N us`
line per sample and a `[gpu2d] last second` summary. It is one pass, so the overlap above does not split it. On a Mac
it follows the load (`checks/gpu2dTiming`: 4 full-screen boxes 0.25 ms, 300 boxes 5.5 ms). **On an iPhone 15 Pro the
pass's own reading is not reliable:** its end-of-fragment timestamp is often not written, so the slot keeps a value
an earlier frame wrote. The end is then before the begin (the same end value was read in two frames in a row), and
the reading is 0. When it is written the value is right (SUMU's zoom: 2.3-4.2 ms). So there a 0 means "no reading",
not "under a millisecond", and the median is meaningless.
`RAE_GPU2D_TIMING=marks` times it by marks before and after the pass instead, as `[gpu passes]` does; those do not wait
for the pass's pixels on a Mac or a phone (300 boxes read under 0.1 ms), so they are only for comparing.

**The Metal command buffers' GPU time (Apple, with `RAE_GPU2D_TIMING`):** a second line each second,
`[gpu2d] last second: N Metal command buffers, GPU us total T, longest L`. It is every command buffer's own
`GPUEndTime - GPUStartTime`, which Metal measures, and on an iPhone it is the number to read: SUMU's zoom showed 757
buffers, 486 ms of GPU time and a longest of 7.7 ms in one second, while the pass's median read 0. On a Mac its longest
matches the pass's maximum (`checks/gpu2dTiming`: both 11.2 ms). wgpu-native does not expose its `MTLCommandBuffer`,
so `compiler/runtime/runtime_gpu_metal_timing.c` replaces `-commit` of the command buffer class this device uses with
one that adds a completed handler, then calls the original. It is installed only when `RAE_GPU2D_TIMING` is set,
never in a normal run. The total counts every buffer of the program, and the frame's work is the longest one.

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

`RAE_AB_HOLD_SECONDS=<s>` holds each phase that long instead (its first third skipped, at most 3 s).
Under a capture (`RAE_PROFILE=1 RAE_PERF_OUT=run.raeperf`) each frame records its setting
(`ab.case`: 0 the baselines, n the n-th case), and the capture is also written split by it,
`run-baseline.raeperf` and `run-<case>.raeperf` beside the whole (perf/PerfSplit). The viewer
(`examples/151_perfViewer`) opens the whole as two captures, the baselines against the first case,
and its Compare view lists each zone's change, most certain first.

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
