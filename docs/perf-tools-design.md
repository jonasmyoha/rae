# Performance tools for Rae — design

Status: design (2026-11-13). The work is queued as `[perf 0/14]` to `[perf 14/14]` in QUEUE.md.
Step 0 merges the `sound/audio-engine` branch: most steps build on what it added.

## 1. The short answer

Rae gets one recorder and one set of views for every kind of program:

- **One recorder** (`lib/perf`). Every thread, the real-time audio thread too, writes fixed-size
  events into its own lock-free ring: zones, counters, frame marks, waits, GPU timestamps. Writing
  an event never allocates or locks, so a `realtime` function may record. When nothing is recording
  a zone costs one branch.
- **Sources that feed it with no work in the app.** These are recorded where they happen:
  - the ECS schedule's systems;
  - the worker pool's chunks, `spawn` and `Task.get`, `Channel` waits, locks;
  - the GPU passes, through timestamp queries;
  - the audio engine's blocks, through the real-time recorder;
  - the frame loop's present;
  - memory.
- **One file and one live link.** A capture is a binary file (`.raeperf`). A
  running program, a phone included, can also stream its events to a viewer over the network.
  Perfetto export stays.
- **Views written in Rae UI** (`lib/ui` chart widgets), used in two tools:
  - **the perf HUD**, which any app shows over itself;
  - **Rae Perf**, a viewer app runnable from devtools, which opens captures and live streams, shows
    timelines, flame graphs and histograms, and compares two captures.
- **Headless reports and budgets** for the test gates: zero allocations on real-time threads, the
  audio block's worst time against its period, frame-time regressions against a saved baseline.

## 2. What exists today

On `main`:

| tool | what it does | limits |
|---|---|---|
| `lib/Profile.rae` (`RAE_PROFILE`) | CPU zones and counters, written as Chrome Trace JSON for Perfetto | one thread; a capture, not live; strings in the hot path |
| `app3d/FrameLog` (`RAE_FRAME_TIMING`) | a `[frames]` line: median, p95, missed budget, scale, thermal | text only |
| `RendererPassTiming`, `CloudTiming`, `Gpu2dCanvasTiming` | WebGPU timestamp slots per pass | a slot is not a pass's cost on a tile GPU (docs/profiling.md) |
| `runtime_gpu_metal_timing.c` (`RAE_GPU2D_TIMING`) | Metal command buffer GPU times | Apple only; per buffer |
| `DrawStats` (`[draws]`) | draws, triangles, instances per pass | text only |
| `RAE_AB` | two settings alternated in one launch, compared | text only |
| `lib/ui/FrameStats`, `DebugOverlay`, `app3d/DebugHud` | fps and memory readouts in an app | a number, no history |
| `RAE_MEM_STATS`, `RAE_MEM_STATS_EVERY_MS` | live String and buffer counts | text only |
| `tools/profile-desktop.sh`, `tools/ios/profile-ios.sh`, `profile-summary.py` | a profile run and its summary | Python, outside Rae |
| devtools web (`tools/devtools-web`) | builds, tests, examples; `runtime_metrics.jsonl` of builds | not the running program |

On `sound/audio-engine` (not merged yet):

| addition | what it gives the perf tools |
|---|---|
| `realtime` and `noAlloc` keywords, checked by the compiler; clang function effects; RealtimeSanitizer gate | the recorder's write calls can be declared `realtime` and proven safe |
| `lib/Atomic.rae` (`Atomic(T)`), `lib/Signal.rae`, `lib/collections/SpscRing.rae` | the per-thread event ring, written in Rae |
| `lib/Realtime.rae`: `RealtimeRecorder` (a no-allocation histogram of call times), `markRealtimeThread`, the runtime guard (`RAE_REALTIME_CHECK`), `promoteToRealtime` | the audio thread's timing and its rule breaks, ready to show |
| `spawn thread` (a dedicated, named thread), `mainThread`, `cExport` and `rae_thread_attach()` (a foreign thread such as an audio callback calling Rae) | every thread kind the recorder must name and give a lane |
| the audio package: the device callback renders Rae code, the render plan of nodes, streams, input, MIDI, the offline driver | the audio sources: per block and per node |
| docs/video-design.md | the frame path and its clock, which the video sources will use |

docs/profiling.md says "no in-engine viewer on purpose: Perfetto is far better". That stays
true for a deep look at one capture on a desktop. Rae's own views are for what Perfetto
does not do:

- live data while the program runs, from a phone too;
- views of Rae's own model: the ECS systems, the real-time roots against their period, the
  audio render plan, the dynamic-resolution scale;
- the same views inside the app itself (the HUD);
- checks in the test gates.

Perfetto export stays, written from the same events.

## 3. The recorder (`lib/perf`)

### 3.1 The event

One fixed-size value type, 32 bytes, so a ring is a plain array and a write is one store of a
record and one atomic index store:

```rae
type PerfEvent {
  time: Int          # nowNs(), the monotonic clock
  kind: PerfKind     # zoneBegin, zoneEnd, counter, mark, waitBegin, waitEnd, gpuSpan, flow ...
  nameId: Int32      # an id from the name table (3.3)
  value: Int         # a counter's value, a GPU span's end, a flow's id, a byte count
  extra: Int32       # a frame number, a node index, a chunk index
}
```

A zone is a begin and an end event. A closed span is not stored, because a thread can stop at
any point. The viewer pairs the begins and ends.

### 3.2 One ring per thread, owned, not global

- A `PerfHub` is a **resource** on the App, like every other single thing (AGENTS.md: no
  globals).
- It holds one `SpscRing(PerfEvent)` per thread: the main thread, each worker of the pool,
  each `spawn thread`, each attached foreign thread.
- A thread gets its ring as a value it is handed:
  - the main thread from the App;
  - a `spawn thread` from its spawn arguments;
  - a pool worker by its worker index (the pool knows the index already: `g_pool_worker_index`);
  - a foreign thread when it attaches.
- The writer is the thread. The single reader is the collector on the main thread, which
  drains every ring once a frame into the hub's window (3.4).
- When a ring is full, the writer drops the event and counts the drop. It never waits. The
  view shows the drops.

### 3.3 Names without strings in the hot path

A name is registered once, at setup, on the main thread:

```rae
const systemPhysics: String = "physics"
let physicsZone: PerfName = perfName(hub: app.perf, name: systemPhysics)
```

The returned `PerfName` (an id) is what a hot path passes. A `realtime` function can only use ids
made before it runs. Registering a name in real-time code is a compile error (it allocates).
Zones created by the compiler or the runtime (the schedule, the pool, the GPU passes) register
their names when they are created.

### 3.4 The API

```rae
perfZoneBegin(ring: thread, name: physicsZone)        # realtime, noAlloc
perfZoneEnd(ring: thread, name: physicsZone)
perfCounter(ring: thread, name: queuedBlocks, value: count)
perfMark(ring: thread, name: frameStart, extra: frameNumber)
perfFrame(hub: app.perf)                                # once a frame, main thread: drains the rings
```

- Each write function is `realtime` and `noAlloc`. When recording is off it returns after one
  load of the ring's `on` flag.
- `perfFrame` moves the rings' events into the hub's **window**: the last N seconds (default 10)
  in fixed blocks, so memory stays flat, plus the frame index (a frame's start and end per
  thread).
- Clock: `nowNs()` everywhere. GPU times are converted to it (section 7).
- `lib/Profile.rae`'s functions become thin calls into the hub, and its Chrome trace writer
  reads the hub (step 2). Existing call sites keep working.

### 3.5 Cost

The design targets are:
- under 30 ns a zone while recording, on an M-series Mac;
- one predictable branch when off;
- zero allocations, checked by the realtime guard in a fixture.

Step 1 measures this and puts the numbers in this document.

## 4. Sources

### 4.1 Frames and the frame loop

- **Frame marks:** the frame loop (`EventLoop`, `mainLoop`, the walker and UI loops) marks a
  frame's start, its present call and its present time.
- **Present time:** taken from the display link where there is one (iOS `CAMetalDisplayLink`,
  0.3.43), else the time the present call returns.
- **Dynamic resolution:** its scale, the frame cap and the thermal state become counters, from
  FrameLog's numbers.
- **What frame pacing shows:** the time between presents, its jitter, and the frames that
  missed their refresh.

### 4.2 The ECS schedule

`lib/ecs/Schedule` already knows each system's name and whether it ran. It records a zone per
system that runs, and a mark per system it skipped (nothing it reads changed). So 106 and the
walkers show where a frame goes by system, with no code in the app.

### 4.3 Threads, tasks and parallel work

| source | what is recorded | where |
|---|---|---|
| `parallelLoop` | a zone per chunk on the worker's lane; the loop as a zone on the caller's lane | the worker pool (runtime ABI exception: the pool) |
| `spawn` / `Task.get` / `taskScope` | a flow from the spawn to the task's run; the time `get` waited | the task entry points |
| `spawn thread` | the thread's name and lane; its loop's zones | `spawn thread` |
| `Channel` | send and receive waits (blocked time only) | the channel's lock |
| locks (`RAE_LOCK`) | the wait to take a contended lock | the lock macro |
| `Signal` | the wait and the wake | `lib/Signal` |
| foreign threads (`rae_thread_attach`) | a lane named by the attach call | the attach |

The views of this (section 9):
- each thread's lane;
- the pool's use over time: busy, waiting and idle shares per worker;
- the frame's critical path: the longest chain of dependent work in it.

### 4.4 Real-time threads and audio

- **Recorders:** each `RealtimeRecorder` (lib/Realtime) becomes a hub source: its histogram, p99.9,
  maximum and the share of the period used. Its calls also go on the thread's lane as zones,
  sampled one in N when many.
- **Rule breaks:** the runtime guard's hits (`RAE_REALTIME_CHECK`: an allocation, lock or `log`
  on a marked thread) become events with their entry point. The HUD shows them in red.
- **The audio engine** records:
  - each block's render time against its period;
  - each node of the render plan (a zone per node, so a mix's costly effect shows);
  - xruns and underruns from the device;
  - the queued blocks;
  - input and output latency;
  - the audio clock against the main clock (drift).
- **The offline driver** renders a scene with no device. Its block times are the numbers a
  headless test checks (section 10).

### 4.5 Video

This source is for when video lands (docs/video-design.md). It records:
- per frame: the decode time, the upload to a texture and the color pass;
- the queue of decoded frames;
- the frame shown against the frame due, and the audio and video offset from the shared clock.

The views are the same frame-pacing views as for games, with the video's own period.

### 4.6 GPU

See section 7.

### 4.7 Memory

As counters, sampled once a frame:
- live Strings and buffers, and allocations in that frame, per thread: a zero-allocation frame
  is a goal for games and a rule for real-time threads;
- the GPU resource tables' counts and bytes;
- process RSS.

A slow growth over minutes is what the memory view (and the existing memory test) looks for.

### 4.8 The app's own

`perfZone` and `perfCounter` in app code, for what only the app knows: physics steps, path
finding, network packets.

## 5. The capture file and the live link

- **The file (`.raeperf`):**
  - a header: the format version, the program, the machine, the Rae version, the start time;
  - the name table;
  - the events in blocks, written with the compiler's `toBinary()` for the record types;
  - each block's thread and time range, so the viewer can open a long capture without reading
    all of it.
- **Writing:** a capture is the hub's window written out, or a recording to disk of up to N
  minutes. It goes to `RAE_PERF_OUT`, or on iOS the app's Documents folder, as Profile does
  today.
- **Perfetto export:** `rae perf export capture.raeperf trace.json` (or a function in `lib/perf`)
  writes Chrome Trace JSON. Each thread is a `tid`, the GPU a separate process lane, and flows
  are flow events.
- **The live link:**
  - `RAE_PERF_LIVE=<host>:<port>` makes a program send its drained events over `lib/net`
    (TCP, the WebSocket framing used by devtools), at most every 100 ms, from the collector
    on the main thread.
  - The viewer listens, so a phone and a Mac on the same network work. The phone's address
    is not needed; the viewer's is.
  - Sending happens on the main thread only and never blocks: when the network is slow, blocks
    are dropped and counted.

## 6. The views (`lib/ui` widgets)

New components, each a node in a `.raescene` with its data bound from code. Each draws through
`Gpu2dCanvas` (lines, rectangles, text) and costs time in proportion to what is visible, never
to the whole capture.

| component | shows |
|---|---|
| `Timeline` | lanes (threads, the GPU, the audio blocks) of spans and marks over time; zoom and pan with the mouse, the trackpad or two fingers; select a span |
| `FlameGraph` | the spans of a selection merged by call path, widest first (a selected frame, or all frames) |
| `Histogram` | a value's distribution: frame times, an audio block's time, with lines at a budget and at p50, p95 and p99.9 |
| `LineGraph` | counters over time: memory, scale, queued blocks; several in one graph |
| `StatTable` | sortable rows: per zone count, total, mean, p95, maximum; per thread busy share |

All five follow the Rae UI rules: an ECS component, a system, a scene key per setting, and
`Ms`-named fields where they hold milliseconds. They are general widgets. A program that is not a
profiler can use `LineGraph` and `Histogram` too.

## 7. GPU timing

- **WebGPU timestamp queries** per pass (`RendererPassTiming`, `CloudTiming`) write `gpuSpan`
  events on a GPU lane.
- **Clock conversion:** the GPU's timestamps are converted to `nowNs()` by one
  (GPU time, CPU time) pair read when a frame's queries resolve. The offset is refitted each
  second, and the drift is shown.
- **Metal command buffer times** (`RAE_GPU2D_TIMING`) give the GPU's real busy time per command
  buffer on Apple. They are the truth the pass slots are checked against.
- **Tile GPUs:** the views mark the pass slots "a slot, not a cost on a tile GPU" (docs/profiling.md
  measured why), and put the command buffer total and an `RAE_AB` switch beside them. That is the
  honest way to find a pass's cost.
- **Counters:** draw counts, triangles and instances per pass (`DrawStats`); the resource tables'
  sizes.
- **Bound by CPU or GPU:** the frame view shows, per frame, the CPU's frame time, the GPU's busy
  time and the time spent waiting for a drawable. It names which one bounds the frame.

## 8. The tools

### 8.1 The perf HUD (in any app)

The debug overlay's button opens it. It sits over the app's own UI and costs nothing while closed
(the recorder still runs only if asked). It shows:
- the frame-time graph of the last 5 seconds, with the refresh budget line, split into CPU and GPU;
- one bar per thread: busy, waiting, idle;
- the audio block's share of its period, and the real-time rule breaks;
- memory, and allocations in the frame;
- a button that saves the last 10 seconds as a `.raeperf` (on a phone, into Documents).

It replaces the fps and memory readouts of `DebugOverlay` (they become its first row) and works
in 3D apps (`app3d/DebugHud`) and UI apps alike.

### 8.2 Rae Perf (the viewer app)

A new example with the next free number, `<n>_perfViewer`. It is runnable from devtools'
Examples, built from `.raescene` files with the shared editor chrome where it fits. It:
- listens for live programs: a list of the programs found, each with its device and its name;
- opens `.raeperf` files, and folders of them (a profile run's captures);
- has these views:
  - **Frames:** frame-time histogram and graph, pacing, CPU and GPU;
  - **Timeline:** every lane;
  - **Flame graph;**
  - **Threads:** per worker use, waits, critical path;
  - **GPU:** passes, command buffers, draws;
  - **Audio:** blocks against period, per node, xruns, drift;
  - **Memory;**
  - **Systems:** the ECS schedule;
- compares two captures: the same views side by side, and a table of what changed, by zone, with
  the noise of each (its spread across frames);
- reads `RAE_AB` results as two captures;
- runs on the Mac and on an iPad.

### 8.3 Headless reports and budgets

- **The report:** `rae perf report capture.raeperf` (or a Rae program under `tools/`) prints the
  summary `profile-summary.py` prints today. Its numbers are frames, zones, threads, GPU, audio
  and memory. `tools/profile-desktop.sh` and `tools/ios/profile-ios.sh` switch to it.
- **Budgets a fixture or a gate can check:**
  - no allocation, lock or log on a real-time thread over a run (exact; the realtime guard);
  - an audio scene's worst block under a share of its period on the offline driver (the
    machine's own share, so a slower machine does not fail falsely: the check compares with a
    calibration loop run first);
  - a frame-time regression: a capture's p95 against a saved baseline of the same program on
    the same machine, failing only beyond the baseline's own spread. A baseline is per machine
    and is never committed as a hard number.

## 9. Threads and parallelism: what the views answer

- **Is the work parallel?** Per frame, each worker's busy share, and the share of the frame
  when more than one worker ran.
- **Where does it wait?** `Task.get`, channel, lock and signal waits, as spans of their own
  color, with the thread that made them wait when known (a flow).
- **What is the critical path?** The chain of zones and waits that decided the frame's length.
- **Too fine or too coarse chunks?** For each `parallelLoop`: its chunk count, chunk times, and
  the time the pool spent handing them out.
- **Did a real-time thread break its rules or miss its deadline?** The rule breaks, and the
  recorder's histogram against the period, with p99.9 and the maximum.
- **Did priority work?** A `spawn thread` promoted with `promoteToRealtime`, and whether the OS
  gave it the priority (the promotion's result).

## 10. Testing

- **Fixtures (headless):**
  - the ring under a writer thread and the collector: no loss under capacity, counted loss over it;
  - names and ids;
  - the file round trip (`toBinary`);
  - the Perfetto export's JSON (a golden);
  - the schedule's zones;
  - the pool's chunk zones from a `parallelLoop`;
  - an audio offline scene's block budget;
  - the realtime guard catching an allocation added on purpose.
- **Checks (`checks/`, a real GPU):** the GPU lane's spans exist and are in the frame; the HUD
  draws.
- **Visual gate:** the viewer app with a saved capture: a screenshot of each view (compared to a
  reference as 121 does).
- **The recorder's own cost:** a fixture times one million zones and fails above the budget of
  section 3.5 times three (a loose bound against an accidental allocation or lock).

## 11. Rules this design keeps

- **No globals:** the hub is a resource on the App. A ring is handed to its thread.
- **No new syntax.** The record calls are functions. Automatic zones come from the code that
  already knows a boundary (the schedule, the pool, the passes), not from attributes. A compiler
  flag that puts a zone around every function (`rae build --perf-functions`) is a possible later
  step. It needs a decision (section 13).
- **Rae first:** the ring, the hub, the file, the views and the report are Rae. New C only where
  the runtime already owns the code: the pool's and the task entry points' hooks, and the
  Metal/WebGPU timing that exists.
- **Naming:** fields that hold milliseconds may end in `Ms`; seconds are `Seconds`
  (AGENTS.md, 2026-11-12).
- **ECS:** the viewer is an ECS program. A capture's threads, zones and frames are its entities'
  data, and the views are systems over them.

## 12. Order of work

| step | what | needs |
|---|---|---|
| 0 | merge `sound/audio-engine` into `main` | the branch's agent idle; a person's go |
| 1 | the recorder: `PerfEvent`, rings, hub, names, cost measured | 0 (`SpscRing`, `Atomic`, `realtime`) |
| 2 | `lib/Profile`, FrameLog and the timing modules on the recorder; Perfetto export with thread lanes | 1 |
| 3 | threads: pool chunks, tasks, channels, locks, signals, `spawn thread`, foreign threads | 1 |
| 4 | the ECS schedule's systems | 1 |
| 5 | GPU: timestamp spans, clock conversion, Metal command buffers, draws, CPU or GPU bound | 1 |
| 6 | real-time and audio: recorders, rule breaks, engine blocks and nodes, xruns, drift | 1, 0 |
| 7 | frame pacing and video hooks | 1 |
| 8 | memory and allocations | 1 |
| 9 | the `.raeperf` file and the live link | 1 |
| 10 | the `lib/ui` chart widgets | none (can go early) |
| 11 | the perf HUD | 2, 10 |
| 12 | the Rae Perf viewer app | 9, 10 |
| 13 | reports and budgets in the gates; the profile scripts | 9 |
| 14 | docs/profiling.md and the devtools page | all |

## 13. Decisions still needed

- **The merge (step 0).** Most steps use the sound branch's `Atomic`, `SpscRing`,
  `RealtimeRecorder`, `realtime` keyword and thread kinds. Building them on `main` first would mean
  writing them twice. The queue stops before step 0 until the merge is agreed.
- **`--perf-functions`:** a zone around every function, made by the compiler. It is useful for an
  unknown slow path, and it has a cost. It is not queued.
- **The viewer on the web:** Rae UI runs in the browser. A viewer on the devtools page itself would
  replace a separate app for the desktop. It is not queued: the app comes first.
