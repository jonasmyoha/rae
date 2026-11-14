# Audio in Rae — design

Status: **design, no code yet** (2026-11-11). Nothing in this document is
implemented. It decides the layers, the rules of the real-time thread, the API a
program sees, the order of the work and how it is tested. Each section ends with
the decisions it makes; §13 lists the ones that still need an answer before or
during the build.

Companion documents: `tech-stack-and-dependencies.md` (the dependency policy, and
its earlier choice "miniaudio + Rae DSP + AudioWorklet on the web", which this
document makes concrete), `concurrency-model.md` (threads, `Channel`, atomics),
`parallel-stages-design.md` (the planned `Atomic(T)`), `platform-conditional-code.md`
(`when` / `Target`), `ecs-resources.md`, `web-frame-loop.md`, `local-notifications.md`.

---

## 0. The word: "audio", not "sound"

The feature is called **audio**: the package is `audio` (`packages/audio/`,
imported as `audio/...`), the types are
`AudioEngine`, `AudioBlock`, `AudioClock`, the environment variables are
`RAE_AUDIO_*`. Reasons:

- Video comes later, and "audio" and "video" are the pair every platform API,
  container format and editor uses (`AVAudioSession`, `AudioToolbox`, WASAPI's
  "Windows Audio Session", AAudio, Web Audio, "audio track" / "video track").
- "Sound" stays as an ordinary word for one thing in memory that is played: a
  `Sound` is a short clip held in memory (§7). So "load a sound" and "the audio
  engine" both read naturally.

This document was asked for as `docs/sound-design.md`; it is
`docs/audio-design.md` to match the decision.

---

## 1. What exists today, and what this replaces

- `compiler/runtime/runtime_audio_sdl3.c` (137 lines) was added for game proto1:
  SDL3's `SDL_AudioStream`, a fixed pool of 16 streams, WAV only, a global
  mute, and module-level C state. No Rae API in `lib/` wraps it: the game calls
  the `rae_ext_audio_*` externs directly (`rae_runtime.h:913-918`).
- It is the "ad hoc sound" this design exists to stop. It stays untouched until
  step 1 of §11 lands, then game proto1 moves to `packages/audio` (in that repo's own
  queue) and the file is deleted.
- Why not grow SDL3 audio into the engine: SDL3 audio is push-based convenience.
  Its callback runs behind a lock on each stream, it has no control of the
  device period, no exclusive / low-latency modes (WASAPI `IAudioClient3`,
  AAudio exclusive), no synchronized input+output (duplex) device and no output
  timestamps. Those are exactly what a DAW and an editor need.

---

## 2. What proven engines teach (the parts we copy)

Short notes on each design, kept to what changes our design.

- **miniaudio** (C, one file, public domain or MIT-0). Two layers: a low-level
  `ma_device` (one callback that receives and fills interleaved frames, with
  backends CoreAudio, WASAPI, DirectSound, ALSA, PulseAudio, JACK, AAudio,
  OpenSL ES, Web Audio, and a "null" backend) and a high-level `ma_engine` /
  node graph. We take **only the device layer**: it carries a decade of
  per-platform quirks (default-device switching, WASAPI period rules, AAudio
  disconnect) that are platform ABI work, not DSP. The engine above it is ours.
  On the web, `MA_ENABLE_AUDIO_WORKLETS` with Emscripten's `-sAUDIO_WORKLET
  -sWASM_WORKERS` runs the callback inside an AudioWorklet (not the deprecated
  ScriptProcessor); the worklet thread needs a larger stack, and the path has
  open issues, so it gets its own check (§12).
- **JUCE**. `prepareToPlay(sampleRate, maxBlockSize)` before any processing, then
  `processBlock(buffer, midi)` with no allocation. All memory is reserved in the
  prepare step. Parameters are atomics read once per block and smoothed. We take
  the prepare / process split, and the rule that the block size may be anything
  up to the maximum (never assume a fixed size).
- **PortAudio**. The callback gets the time of the first input sample, the time
  the first output sample is heard, and the current time, all on one clock.
  We take this: every block carries both a **sample time** and a **host time**
  (§6).
- **CLAP** (C ABI, MIT). A plugin is `activate(sampleRate, minFrames, maxFrames)`
  (may allocate) / `process` (may not) / `deactivate`. Events (notes, parameter
  changes) come in ONE time-sorted list per block, each with a sample offset
  inside the block, so automation and notes are sample-accurate without
  splitting the host's block. Parameter changes from the UI travel as events,
  not shared memory. Threads are named by role (main / audio / thread pool) and
  each call states which role may make it. We take all of this: our node
  contract (§5) is CLAP-shaped, and CLAP is the first external plugin format we
  host (§11 step 6).
- **VST3** (C++; **MIT since 3.8, October 2025** — the earlier GPLv3 / proprietary
  dual license is gone). Separate processor and controller; sample-accurate
  parameter queues per block. Hosting is possible later behind the same node
  contract; it is C++, so it comes after CLAP.
- **ASIO** (Windows pro drivers): the SDK gained a GPLv3 option next to its
  proprietary license in October 2025. Only needed if WASAPI exclusive mode is not
  enough on real interfaces; decided by measurement, late (§13).
- **Ardour**. The process graph is built on the GUI thread and handed to the
  real-time side as a finished, sorted plan; the real-time side never edits the
  graph. Several real-time worker threads process independent branches in
  parallel inside one callback, triggered by semaphores and joined before the
  callback returns. Disk streaming is a separate "butler" thread that keeps
  per-track ring buffers full ahead of the playhead and handles seeks. Plugin
  delay compensation is computed when the graph is built. We take the plan
  hand-off, the butler, and the delay compensation.
- **Bitwig / Ableton public talks, and the Audio Developer Conference talks on
  real-time audio** (for example "Real-time 101", ADC 2019, and the talks on
  lock-free queues and on using locks safely in audio code). Common points: no allocation, no locks, no system calls, no unbounded
  work on the audio thread; lock-free single-producer single-consumer queues
  both ways; objects that the audio thread drops go back to another thread to be
  freed; denormal numbers flushed to zero; worst case matters, not the average;
  pre-render ("freeze") what is not live; one engine runs both live and offline
  (bounce/export), which is also how it is tested.
- **Platform notes**: Apple's audio workgroups (`os_workgroup`, macOS 11 / iOS
  14) are how extra real-time worker threads tell the scheduler they belong to
  the device's IO thread; Windows uses MMCSS ("Pro Audio" task); Linux uses
  `SCHED_FIFO` through rtkit; Android's AAudio callback thread is already
  `SCHED_FIFO`, and extra workers use the performance hint API (ADPF).

---

## 3. The layers

From the platform up. A program only ever uses the top two; the rest is the
`packages/audio` package and the runtime.

```
  Program (game, SUMU, DAW, video editor)
  ─────────────────────────────────────────────────────────────────────
  L6  Simple API          sounds, music, buses, ducking      (Rae, packages/audio)
  L5  ECS layer           track/bus/send entities, systems   (Rae, packages/audio/*System)
  L4  Engine              render plan, nodes, voices, mixer, (Rae, runs on the
                          scheduler, clock, meters            real-time thread)
  L3  Media               decoders, streams, resampler, MIDI (Rae + vendored C
                          parsing                             + platform codecs)
  L2  Host                session, interruptions, routes,    (Rae policy over
                          now-playing, audio focus, MIDI I/O  one-call shims)
  L1  Device              open/start/stop a device, the      (miniaudio device
                          callback, timestamps, RT priority   layer + one-call shims)
  ─────────────────────────────────────────────────────────────────────
  Core Audio · WASAPI · ALSA/PulseAudio/PipeWire/JACK · AAudio · Web Audio
```

- **L1 Device.** Vendored miniaudio (`packages/audio/native/miniaudio/`, public
  domain / MIT-0, kept with its own license per the copyright rule) built with only the
  device layer (`MA_NO_ENGINE`, `MA_NO_NODE_GRAPH`, `MA_NO_DECODING`,
  `MA_NO_RESOURCE_MANAGER`, `MA_NO_GENERATION`). Plus one-call shims for what
  miniaudio does not expose: output host timestamps per platform, joining the
  Apple audio workgroup, MMCSS, the flush-to-zero CPU flags. This follows the
  runtime C rule: a vendored library used as a library (like `lodepng`), and
  every shim function is one platform call with no policy.
  Vendoring it was decided on 2026-11-11 (§13 D1).
- **L2 Host.** Per-platform session rules (§9) as one-call shims (Objective-C
  on Apple, JNI on Android, COM on Windows, D-Bus on Linux, JS on the web), with
  every decision (when to duck, what to do on an interruption) in Rae.
- **L3 Media.** Decoding, streaming, resampling, MIDI byte parsing (§7, §8).
  Runs on worker threads, never on the real-time thread, except the resampler
  and the MIDI event list, which are pure functions over memory.
- **L4 Engine.** Rae code. The only Rae code that runs on the real-time thread.
- **L5 ECS.** The control side of a mixer as entities and components.
- **L6 Simple API.** What a game or SUMU calls: load, play, stop, fade, buses.

Decision: the device layer is bought (miniaudio), everything with a design in it
is Rae. The engine is never miniaudio's `ma_engine`.

---

## 4. Threads and the real-time rules

### 4.1 The threads

| thread | who starts it | runs | may block | may allocate |
|---|---|---|---|---|
| **main** | the program | ECS systems, the UI, L5/L6, builds render plans | yes | yes |
| **audio (real-time)** | the device (Core Audio IO thread, WASAPI event thread, AAudio callback, the AudioWorklet) | L4 `renderBlock` | **no** | **no** |
| **audio workers** (later) | the engine, joined to the device's workgroup / MMCSS | independent branches of one plan, inside one block | **no** (spin, then wait on a semaphore set by the audio thread) | **no** |
| **streaming ("butler")** | the engine, ordinary priority | file reads, decoding, resampling of streams, refilling ring buffers | yes | yes |
| **reclaim** | the main thread's frame | frees plans, sounds and buffers the audio thread has let go of | yes | yes (frees) |

### 4.2 Where the Rae render code runs (two stages)

Today Rae has no way to hand a Rae function to C as a callback
(`parallelism-first-plan.md` §2, "once Rae-function-to-C callbacks exist").
The device callback therefore cannot call Rae directly yet. The design has two
stages with the **same** Rae render function, so nothing above L1 changes
between them:

- **Stage A — render thread one period ahead (needs no language change).**
  The C callback only copies: it takes the next finished block from an output
  ring (and puts the captured input block into an input ring), then posts a
  semaphore. A Rae thread started by the engine (`spawn`), raised to real-time
  priority and joined to the device workgroup through a shim, waits on that
  semaphore and calls `renderBlock` for the next period. Cost: one extra device
  period of latency (128 frames at 48 kHz = 2.7 ms). If the Rae thread is late,
  the callback plays silence and counts an **underrun**; it never waits. This is
  enough for games, SUMU, a music player, a video editor and a first DAW.
- **Stage B — render inside the callback.** When Rae can pass a function to C
  (a general FFI feature, needed for Box3D's scheduler too), the shim calls
  `renderBlock` from the device callback, and the extra period is gone. This is
  what live monitoring of an input through effects needs at small buffer sizes.

The language feature for stage B is a design question of its own; this document
does not add syntax (§13 D2).

**Stage B landed (2026-11-11, `packages/audio` 0.4.0, compiler 0.3.88):** the
device's callback (`native/rae_audio_glue.c`) calls
`audio/AudioDevice.audioDeviceRender`, a `realtime cExport("rae_audio_render")`
function, with the engine's state (the element of a one-element List the
engine keeps, never grown, so the pointer stays valid) and the device's
buffer, which it fills a period at a time. The callback attaches the
device's thread to the runtime (`rae_thread_attach`) and marks it real-time
on its first call. On a Mac: no underrun, and no frames of latency beyond the
device's own buffer (stage A held 1024 at a 256-frame period); it passes
`RAE_REALTIME_CHECK=1`, the Rae `realtime` check and `rae run --sanitize
realtime`. Stage A stays behind `RAE_AUDIO_STAGE=A`. The glue holds a weak,
empty `rae_audio_render`, replaced by the program's at link time, so a
program that links the glue only for `audio/Sound`'s files still links.

### 4.3 What code on the real-time thread may do

The rule of every engine in §2, in Rae terms. On the audio thread and the audio
workers:

- **No allocation and no free.** No `String` building or interpolation, no
  `List` growth or new `List`, no `own` value created or dropped that owns heap,
  no `log` with interpolation. Locals are plain values (numbers, enums, structs
  of them) or `view`/`mod` aliases into memory reserved in the prepare step.
- **No locks and no blocking.** No `Channel(T)` (it is mutex-guarded today), no
  `Task.get()`, no `spawn`, no file or network I/O, no `sleep`, no system call
  that can wait. Messages cross in lock-free single-producer single-consumer
  rings (§4.4).
- **Bounded work.** No loop whose length depends on data the audio thread did
  not size in advance. Voice counts, event counts and node counts have fixed
  maxima chosen at prepare time; past the maximum, a voice is stolen or an event
  dropped and counted, never a list grown.
- **Denormals flushed.** The shim sets flush-to-zero / denormals-are-zero
  (MXCSR on x86-64, FPCR on ARM64) on every real-time thread, once.
- **No reference counting that can free.** An object the audio thread stops
  using is put on the reclaim ring, and the main thread frees it.

How the rules are held:

- **At run time, first.** Every real-time thread sets a thread-local flag through
  the shim. The runtime's allocation, free, lock and `log` entry points check the
  flag; with `RAE_REALTIME_CHECK=1` (on in the test suite) a hit is counted and
  reported with the function name, and the test fails. Off, the check is one
  thread-local load. This needs no language change and catches the libraries
  too.
- **At compile time, later.** A compiler check that a function and everything it
  calls neither allocates nor locks is possible from the generated call graph
  (the compiler already knows which calls own heap). How a function is marked as
  real-time (a keyword, or a rule such as "every function reachable from
  `renderBlock`") is a language decision (§13 D3). No `@` attribute.

### 4.4 Crossing between threads

All crossings are fixed-size lock-free rings with one writer and one reader,
reserved at engine start:

| ring | writer → reader | carries |
|---|---|---|
| commands | main → audio | timestamped events: play voice, stop, set parameter, transport start/stop/seek, swap plan |
| feedback | audio → main | voice ended, meters (peak/RMS per track), underrun counts, clock pairs (sample time, host time) |
| reclaim | audio → main | plans, voices and sounds the audio thread let go of |
| stream rings | butler → audio (one per streamed voice) | decoded, resampled frames |
| input ring | audio → main/butler | captured input (recording) |

The rings need acquire/release atomics. `Atomic(T)` is planned in
`parallel-stages-design.md` §5 but not built; the audio engine is its second
user, and the ring is written in Rae over it (a `Buffer` plus two atomic
indexes). Until `Atomic(T)` exists, the ring's two index operations are one-call
shims over C11 `stdatomic` (§13 D4).

Decision: the audio thread owns nothing it allocated and changes nothing the
main thread reads except through the feedback ring.

---

## 5. The engine (L4)

### 5.1 Blocks and formats

- Samples are **32-bit float**, non-interleaved (one `Buffer(Float32)` slice per
  channel) inside the engine. The device layer converts to the device's format
  and interleaving at the edge.
- The **sample rate and block size are whatever the device gives.** The engine
  is prepared with the device's sample rate and the maximum block size; a call to
  `renderBlock` may be any size from 1 frame to that maximum (Core Audio and
  AAudio vary it). A change of device sample rate rebuilds the plan on the main
  thread (prepare again), never on the audio thread.
- An `AudioBlock` is a view: `frameCount`, `channelCount`, the channel slices,
  the block's `sampleTime` (§6) and its event list.

### 5.2 The render plan

The graph (tracks, buses, sends, inserts) is edited on the main thread as ECS
data (§10). When its structure changes, the main thread **compiles a render
plan**:

1. sort the nodes so every node runs after its inputs; refuse a cycle;
2. give each edge a scratch buffer from a pool, reusing buffers whose last
   reader has run (liveness, like register allocation), so a 200-track session
   needs tens of buffers, not hundreds;
3. compute each path's latency and insert delay lines so all paths into a bus
   line up (**plugin delay compensation**);
4. mark independent branches for the workers (later);
5. call each node's `prepare` (may allocate).

The finished plan is handed over in one command; the audio thread swaps it in at
the start of a block and returns the old one on the reclaim ring. Parameter and
value changes do **not** rebuild the plan; they are events.

A plan swap keeps the running state of nodes that exist in both plans (a reverb
tail does not cut off when a track is added). Nodes are matched by their entity
id.

**Landed, steps 1, 2 and 5 (2026-11-13, `packages/audio` 0.12.8,
`audio/AudioGraph` and `audio/GraphRender`):** the graph (nodes and edges,
main thread), `compileGraph` (a depth-first order from the output, so each
track and each bus group runs to its end before the next and few buffers are
alive at once; a cycle refused with its nodes, "node 2 <- 1 <- 3 <- 2";
scratch buffers reused once their last reader has run; prepare), and a
`RenderPlan` made only of `Buffer` handles and numbers, so it crosses on a
`SpscRing` in one command and comes back on the reclaim ring
(`GraphRenderer.submitPlan` / `takeReclaimedPlans`). Node kinds, each with one
dense typed state table and run by a `match` on the kind: the source adapters
(`voices`, `stream`, `input`: a slot of a sources pool the engine fills before
the graph runs), `gain` and `send` (a 5 ms linear ramp to a new target),
equal-power `pan`, `sum`, `delay`, `meter`, a peak `limiter` with no look-ahead,
and the `output`. A 200-node session (40 tracks in groups of four into ten
buses) needs 14 scratch buffers. Not yet: the engine does not run a plan (the
mixer is still the one node; the simple API moves onto the graph in step 6's
ECS part), parameter changes at a sample time (events), delay compensation
and plan swaps that keep node state. Fixture 1436.

**Landed, step 3 and the swap (2026-11-13, `packages/audio` 0.12.9,
`audio/GraphCompile`):** a node that reports latency (`latentNode`, standing
for a look-ahead limiter or a plugin) delays its path, and the compiler adds a
delay of the difference on every earlier input of the same node, so the paths
line up sample for sample; the plan records its total `latencyFrames`.
`compileGraphAfter(graph:, previousGraph:, previousPlan:)` matches nodes by
`entityId` and kind; at the swap the audio thread carries each matched node's
state (a gain's current level, a meter, a limiter's gain, a delay line's
contents by exchanging the two plans' line buffers, so nothing is copied or
allocated), only when the new plan was compiled against the plan it replaces
(a serial). A node of the previous graph that is gone but has a tail stays,
fed silence, feeding the nodes it fed, until a later compile drops it.
Fixture 1437: the compensated sum is exactly 2 x(t - 64); a track added
mid-echo and the echo's delay removed mid-echo both leave the output equal,
bit for bit, to never swapping (and without the carry both differ).

### 5.3 Nodes

A node is a kind and a state. The contract is CLAP-shaped:

- `prepare(sampleRate, maxFrames)` — main thread, may allocate;
- `process(state: mod <Kind>State, block: mod AudioBlock)` — audio thread, may not
  allocate; reads `block.events` for its own parameter events and notes;
- `reset(state: mod <Kind>State)` — audio thread (after a seek), may not allocate;
- `latencyFrames` and `tailFrames` — read by the plan compiler.

Built-in kinds, first: gain, pan (equal-power), sum, send (pre/post fader),
delay line (also the delay compensation), meter, sampler voice set, stream
voice, input, resampler, limiter. Then: EQ (biquads), compressor (and the
sidechain duck of §6.4), reverb, synth voices.

**Landed, EQ, compressor and reverb (2026-11-14, `packages/audio` 0.12.16):**
three node kinds (`audio/EffectStates` the settings and states,
`audio/GraphEffects` the audio thread), and the same as ECS inserts
(`eqInsert`, `compressorInsert`, `reverbInsert`).

- `eq`: a biquad from Robert Bristow-Johnson's Audio EQ Cookbook (peak, low
  and high shelf, low and high pass), transposed direct form II a channel.
  Parameter events 2, 3 and 4 move its frequency, gain and Q over 5 ms, the
  coefficients following every 16 frames; a plan swap carries its memory and
  moves to the new plan's values the same way.
- `compressor`: feed-forward and stereo-linked, the static curve with a soft
  knee of Giannoulis, Massberg and Reiss (2012), the gain reduction smoothed
  with one-pole attack and release in decibels, makeup gain. A second input is
  its sidechain (the level is measured there): ducking one bus by another is
  this node. The simple API's in-mix duck (§6.4) stays its own `duck` node, so
  its sound and fixtures 1401-1417 do not change.
- `reverb`: the reverb is not named in this design, so it is **Freeverb**
  (Jezar at Dreampoint, public domain): eight lowpass-feedback combs and four
  allpasses a channel, Jezar's tunings at 44.1 kHz scaled to the sample rate,
  the right channel 23 samples longer, his scaling of room, damping, wet, dry
  and width. Its lines are allocated when the plan is prepared and carried
  over a swap by exchanging buffers.

Fixture 1445 (`RAE_REALTIME_CHECK=1`): each EQ shape's magnitude at 60, 250,
1000, 4000 and 12000 Hz, from a Float64 DFT of its impulse response, within
0.02 dB of the cookbook's formula in Float64 (the coefficients are Float: a
300 Hz high pass is 0.011 dB off at 60 Hz, 28 dB down); a gain event with no
click and on +12 dB after it; the compressor's static curve at seven levels
within 0.01 dB, its attack time constant 481 frames for 10 ms at 48 kHz (480),
a duck by a sidechain; Freeverb equal bit for bit to a reference written once
by an independent Python implementation of the same structure in float32
(`make_reference.py`, beside the fixture), and its tail to -60 dB the same
47366 frames; a plan swap of eq, compressor and reverb that changes nothing.
Not yet: `lib/Float4` for these (whether four combs or two channels at once
are faster is unmeasured; queued).

Rae has no function values and no interfaces, so the plan runs nodes with a
`match` on the node kind over typed state tables (one dense table per kind, ECS
style). How a program adds its **own** node kind without editing `packages/audio` is
open (§13 D5): a generic engine over one program node type, a WASM node (the Rae
plugin ABI), or a function value once stage B exists.

### 5.4 Parameters

- Every parameter has a target and a smoother (linear ramp in gain, over a
  default 5 ms, settable) so a change never clicks.
- A change is an **event** with a sample time; the node applies it at that exact
  sample (§6). Automation is a list of such events from the timeline (§6.3).
- Gain is stored in linear and set in decibels in the API (`gainDecibels`).

Decision: one engine for live and offline; the plan is compiled off the audio
thread; nodes are prepared before they process.

---

## 6. Time: the clock, scheduling and sync with video

### 6.1 The audio clock

- **Sample time** is an `Int` count of frames since the engine started, on the
  output device. It never jumps backward and does not pause when the transport
  stops. It is the one clock of the engine.
- **Host time** is the platform's monotonic clock in nanoseconds (the same one
  `Time` uses). Each block gets the host time at which its first frame will
  **reach the speaker**, from the platform: Core Audio `AudioTimeStamp.mHostTime`
  plus the device and stream latency, WASAPI `IAudioClock::GetPosition` with its
  QPC position, AAudio `getTimestamp`, ALSA `snd_pcm_status` timestamps, Web
  Audio `getOutputTimestamp()`. miniaudio does not pass these through, so this
  is the first shim set of L1.
- Raw timestamps jitter. The engine smooths the (sample time, host time) pairs
  with a **delay-locked loop** (the JACK / PortAudio method) on the main thread,
  giving a stable mapping both ways and the measured real sample rate (a
  "48 kHz" device runs at 47 998.7 Hz against the host clock).

### 6.2 Scheduling at an exact sample

- Every command has a `sampleTime`. The audio thread puts the commands of the
  current block into the block's event list in time order, each with its offset
  inside the block. A node processes up to the event's offset, applies it,
  continues (CLAP's way). So a note or a sound starts at **its sample**, whatever
  the block size.
- A command whose time is already past starts at the block's first sample and
  is counted as late (a test can check that count is zero).
- The main thread schedules "as soon as possible" with
  `sampleTime: audio.clock.nextSafeSampleTime()`: now plus the output latency
  plus one block, so it is never late.
- The **block-size test** (§12) holds this: the same events rendered with block
  sizes 1, 17, 64, 512 and a varying size give the same samples.

### 6.3 Musical time

A `Transport` (play state, position) and a `TempoMap` (tempo and meter changes)
convert beats or ticks to sample time and back, exactly, in `Int` ticks with a
fixed resolution (960 per quarter note, settable). The timeline schedules clips,
notes and automation by converting to sample time a little ahead of the audio
thread (one block of look-ahead is enough for events; streamed clips start their
pre-roll earlier, §7.2). Loops are handled by the timeline, which schedules the
next pass, not by the audio thread.

**Landed, what a timeline needs from the engine (2026-11-13, `packages/audio`
0.12.13):** `playFromAt` (a sound from one of its frames, at a sample: the
command's `startFrame`), `playStreamFromAt` (a stream skipping its first frames,
heard from a sample; the butler starts reading at once, so a later start is
pre-rolled, §7.2), `rampBusGainAt` / `rampBusLinearGainAt` (a bus's gain in a
straight line over a given number of frames: a setBusGain command's
`frameCount`), a stop that reaches a voice before its first sample ends it
unheard, and a command due on a block's first sample joins the event list
behind those already due there, so commands of one sample apply in the order
they were sent at every block size (a note released and started again on one
sample lost its order in blocks of 512). Fixture 1442.

**The timeline itself is not in this package.** Clips, notes, automation, loops
and seeking over a TempoMap (the `timelineSystem` of §10.2) are an editor's
feature, so they live in the media editor's own repository
(`mediaeditor`, its `packages/timeline`), built on the calls above, with
`MusicalTime` and the exact-sample commands of §6.2 that games use too.

### 6.4 Ducking

Two different things share the word:

- **Inside our mix** (SUMU's alarm over SUMU's music): a duck rule on a bus —
  "while any voice plays on `alerts`, `music` goes to −12 dB, 50 ms down, 400 ms
  back". It runs on the audio thread from the alerts bus' activity, so it is
  sample-accurate and needs no main-thread timing.
- **Over other apps' audio** (SUMU's alarm over Spotify): a session request
  (§9): iOS `.duckOthers`, Android `AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK`. Held only
  while the sound plays, then released so the other app comes back up.

### 6.5 Shared clock with video

Audio is the master clock. A video frame's presentation time in the media
timeline is turned into a sample time through the transport; the video player
shows, at each display refresh, the frame whose time is closest to "the sample
time being heard at this refresh's host time" (from the DLL mapping of §6.1, the
display's own presentation timestamp on the other side). Video never pushes the
audio; when they drift, video drops or repeats a frame. In an offline export
there is no device: the exporter advances audio by sample time and asks for the
video frames at the matching times.

Decision: sample time is the engine's clock, host time is reached only through
the smoothed mapping, every scheduled thing carries a sample time.

---

## 7. Sounds in memory and streams from disk

### 7.1 Sounds (short, in memory)

A `Sound` is decoded fully at load time into `Buffer(Float32)` channel data at
its own sample rate, immutable after load, and shared by every voice that plays
it. Loading runs on a worker (`spawn`), returning a `Task(Sound)`; a program
loads at start or behind a loading screen. Freed through the reclaim ring, only
after no voice uses it. Typical use: game effects, UI clicks, SUMU's alarm
sounds, drum samples.

### 7.2 Streams (long files)

A music track, a long recording or a DAW clip is **streamed**: the butler thread
reads and decodes ahead into the voice's ring buffer (2 s by default, settable;
a DAW keeps less per track and more tracks). Seek is a command: the butler
refills from the new position; the voice plays silence (or keeps the old audio,
for a scrub) until the ring holds enough, and reports when it is "ready". For a
timeline, clips that will start soon are **pre-rolled**: the butler fills their
rings before their start sample, so the start is exact. An empty ring during
play is an underrun on that voice: silence, a counter, a feedback event; never a
wait.

Gapless play (an album, SUMU's queue) is the next stream scheduled at the sample
where the current one ends.

### 7.3 Decoders: Rae, a vendored library, or the platform

The AGENTS.md rule applies per format: write it in Rae when the spec is small
and bounded, use a proven library for a hard domain, use the platform when it is
clearly faster or the only legal choice, and keep the Rae version as the fallback
where there is one. Every decoder sits behind one Rae API (`AudioDecoder`:
open, read frames, seek, close, format info), so a program never sees which one
ran.

| format | first implementation | language / license | notes |
|---|---|---|---|
| WAV, AIFF (PCM, float) | **Rae** | Rae | small spec; the reference for the tests |
| FLAC | **dr_flac** first, Rae port later if measured equal | C, public domain / MIT-0 | a Rae port gets the one-line "Ported from dr_flac" notice |
| MP3 | **dr_mp3** (C); Apple: AudioToolbox if measured faster | C, public domain / MIT-0 | MP3 patents have expired |
| Ogg Vorbis | **stb_vorbis** | C, public domain / MIT | |
| Opus (later) | **libopus** + Ogg parsing in Rae | C, BSD-3 | needed for voice and the web |
| AAC (M4A) | **the platform**: AudioToolbox (Apple), Media Foundation (Windows), `AMediaCodec` (Android), WebCodecs `AudioDecoder` (web); Linux: FFmpeg's decoder through the Media API | OS / LGPL (dynamic) | no good permissive C AAC decoder exists (`fdk-aac` has a license that is not open source in the usual sense; `faad2` is GPL), and the codec patents are the platform's to carry |
| MP4 / M4A container | **Rae** | Rae | a box parser, shared with video |

All C decoders are vendored in the package (`packages/audio/native/`) with
their own license files (the copyright rule), behind the Rae API (§13 D1).

### 7.4 Resampling

A sound at 44.1 kHz on a 48 kHz device, a varispeed play, a pitch change in a
sampler: all need a resampler. Ours is **written in Rae**: a windowed-sinc
polyphase filter with fixed quality presets (`fast`: 8 taps, linear between
phases; `good`: 32 taps; `best`: 64 taps, for export). It passes the litmus test
of `tech-stack-and-dependencies.md` (bounded spec, pure maths). It is tested
against libsamplerate (BSD, used only in the tests as an oracle) on THD+N and
aliasing (§12), and later vectorized with `Float4`/`Float8`. A sound is resampled
on load to the device rate when its voice plays at speed 1 (cheap at play time);
a streamed voice is resampled on the butler thread; only a voice whose speed
changes per sample resamples on the audio thread.

Decision: three sources of decoders behind one API; the resampler is Rae.

---

## 8. Input and MIDI

### 8.1 Audio input

- A **duplex** device (one callback for input and output on one clock) is opened
  when the program asks for input. Recording writes the input ring; the butler
  writes it to a file (WAV, then FLAC). A recorded take is placed on the timeline
  at its true sample time: the input latency is subtracted using the device's
  reported latency, checked by a loopback measurement the person can run.
- Permission: iOS and macOS need the microphone permission
  (`NSMicrophoneUsageDescription`), Android `RECORD_AUDIO`, the web a user
  gesture and `getUserMedia`. The engine reports "not allowed" as a state, not a
  crash.

### 8.2 MIDI

- No library: MIDI is a small protocol, and each platform API is one-call shims.
  CoreMIDI (macOS, iOS), Windows MIDI Services (MIDI 2.0, Windows 11) with WinMM
  `midiIn`/`midiOut` as the fallback, ALSA sequencer (Linux), AMidi (Android NDK,
  API 29+), Web MIDI (Chrome and Firefox; Safari has none, so the web build
  says so). Byte parsing, running status, SysEx assembly and later MIDI 2.0
  UMP packets are Rae.
- Each input event is stamped with the host time the platform gives (CoreMIDI
  and ALSA give one; WinMM's is coarser), mapped to sample time with the clock
  of §6.1, and queued to the audio thread. A live note therefore lands at a
  consistent delay (one block plus the output latency), not at "the start of
  whatever block came next", which is what makes playing a soft synth feel even.
- MIDI out is scheduled like any event: by sample time, turned into host time
  for the platform call.
- Devices appear and disappear; that is an ECS event (§10), not an error.

---

## 9. Platforms and their session rules

Device backends (through miniaudio) and the host rules (our shims, L2):

| platform | device backend | low-latency path | host rules we must follow |
|---|---|---|---|
| **macOS** | Core Audio (AUHAL) | `kAudioDevicePropertyBufferFrameSize` down to 32-64 frames; audio workgroups | default-device and sample-rate change notifications; Now Playing (`MPNowPlayingInfoCenter`, `MPRemoteCommandCenter`) |
| **iOS** | Core Audio (RemoteIO) | `setPreferredIOBufferDuration` (e.g. 0.005 s); audio workgroups | the full `AVAudioSession` (below); `UIBackgroundModes: audio` for background play |
| **Windows** | WASAPI | `IAudioClient3` shared low-latency (down to ~2.7 ms on supporting drivers), exclusive mode; MMCSS "Pro Audio" | `IMMNotificationClient` (default device changed, device removed); media keys and lock screen through `SystemMediaTransportControls` |
| **Linux** | PipeWire through its PulseAudio and JACK interfaces; ALSA direct; JACK | JACK (or `pipewire-jack`) for pro use; ALSA period size | real-time priority through rtkit; media keys through MPRIS (D-Bus) |
| **Android** | AAudio (API 27+), OpenSL ES below | AAudio `PERFORMANCE_MODE_LOW_LATENCY`, exclusive sharing mode, MMAP | audio focus, `AudioAttributes` usage, "becoming noisy", `MediaSession` + a foreground service of type `mediaPlayback` |
| **Web** | Web Audio: an AudioWorklet plays blocks the page renders on the main thread (§11 step 3); the engine inside the worklet needs shared memory, opt-in | `latencyHint: "interactive"` | a user gesture before the first sound (autoplay policy; the device resumes on the first one); shared memory needs COOP/COEP headers; Media Session API for the lock screen |

Native PipeWire, Oboe (C++, Apache-2.0) and ASIO are not needed to start; each
comes in only if a measurement on real hardware asks for it.

### 9.1 iOS: the audio session

The program never calls `AVAudioSession`; it states an **intent**, and Rae picks
the category and options:

| intent (`AudioSessionKind`) | category | options | silent switch | other apps' audio |
|---|---|---|---|---|
| `ambient` (game effects, UI sounds) | `.ambient` | — | silenced | mixes |
| `music` (a music player) | `.playback` | — | plays | stops it |
| `alert` (SUMU's alarm) | `.playback` | `.mixWithOthers`, `.duckOthers` | plays | ducked while it sounds |
| `spokenText` (text to speech) | `.playback`, mode `.spokenAudio` | `.duckOthers` or `.interruptSpokenAudioAndMixWithOthers` | plays | ducked (spoken audio paused) |
| `recording` (DAW, voice memo) | `.playAndRecord` | `.defaultToSpeaker`, `.allowBluetoothA2DP` | plays | stops it |

When several intents are active (SUMU playing music and an alarm ringing), the
strongest wins: the session goes to the alarm's options while the alarm
sounds, then back. The session is set **active** only while something plays,
and deactivated with `.notifyOthersOnDeactivation` so another app's music
resumes.

Events, each turned into an ECS event (§10) and handled in Rae:

- **Interruption began** (a phone call, a Siri request, an alarm of the clock
  app): the device has stopped. The engine marks all voices paused at their
  sample time. **Ended** with `shouldResume`: resume music; alerts that were cut
  are not replayed later.
- **Route change**, reason `oldDeviceUnavailable` (headphones unplugged, Bluetooth
  lost): **pause music** (Apple's rule: the sound must not suddenly come out of
  the speaker). New device: the sample rate or channel count may change, so the
  plan is prepared again.
- **Media services were reset**: rebuild the session and the device from
  scratch.
- **Now Playing / remote commands** for the `music` intent: title, artist,
  artwork, duration, position, rate; play, pause, toggle, next, previous, seek.

Android maps the same intents to `AudioAttributes` usage (`USAGE_GAME`,
`USAGE_MEDIA`, `USAGE_ALARM`, `USAGE_ASSISTANT`, `USAGE_VOICE_COMMUNICATION`) and
audio focus requests (`GAIN`, `GAIN_TRANSIENT_MAY_DUCK`), and "becoming noisy" to
the same route-change event. The desktop platforms have no session; the intents
there only choose the default bus volumes and whether Now Playing is set.

### 9.2 SUMU's alarm, exactly

SUMU wants: heard with the phone on silent, ducking music, but **not** heard
during Do Not Disturb or a Focus.

- SUMU running (foreground, or background with an active `music` session):
  play the alarm sound with intent `alert`. Before playing, SUMU checks the Focus
  state: `INFocusStatusCenter` (iOS 15+) answers `isFocused` only with the
  person's permission **and** the Communication Notifications capability; without
  the capability it answers false even during a Focus. If SUMU cannot get that
  capability for its kind of app, the fallback is to treat "unknown" as "play".
  This is the one item to settle with an experiment on the phone (§13 D7).
  **What an iPhone 15 Pro showed (2026-11-11, `checks/audioSession`, audio
  0.3.0, run and read by an agent; nobody at the phone):**
  - `INFocusStatusCenter` is there and answers with no entitlement and no
    permission: authorization 0 (not asked) and `isFocused` false. That false
    means nothing: iOS gives it without access, Focus on or off.
  - Asking for access (`requestAuthorization`) without
    `NSFocusStatusUsageDescription` in Info.plist ends the app (SIGABRT in TCC,
    iOS's privacy layer). The package now asks only when the key is there, and
    `tools/ios/gen-ios.sh` writes it from `RAE_IOS_FOCUS_USAGE`. With the key
    the request is made and iOS shows its prompt; with nobody to answer, the
    status stayed "not asked".
  - After "Allow" was tapped on the phone, `authorizationStatus` reads 3
    (authorized) on later runs.
  - The Communication Notifications capability: `gen-ios.sh` adds it with
    `RAE_IOS_COMMUNICATION_NOTIFICATIONS=1`, and automatic signing with the
    team J44HX74U4S accepted it (the signed app carries
    `com.apple.developer.usernotifications.communication`). So SUMU can hold
    it for development builds; whether App Store review accepts it for an app
    that is not a messaging app is a separate question.
  - Authorized, with and without the capability, `isFocused` read false on
    every run. No run had a Focus known to be on, so this does not yet say
    whether the capability is needed to read true. That run needs a person to
    switch a Focus on (or a Focus shared from this Mac); it is the one open
    answer of D7, queued with the hearing checks as `[audio 2d/7]`.
  - Seen on the way: on iOS, deactivating the session stops the audio unit,
    and activating it again does not start it (the device's callbacks stopped
    for good after the first deactivation, so later music was silent).
    audio 0.3.0 stops the device before it deactivates and starts it after it
    activates, and after an interruption; the music then ran to its end.
- SUMU not running: the sound is the local notification's sound
  (`local-notifications.md`). The system already silences it on silent and during
  a Focus, which is the wanted behavior except for "heard on silent". Making a
  notification sound break the silent switch needs Apple's Critical Alerts
  entitlement, which also breaks Focus; iOS 26's AlarmKit also breaks through
  both. Neither matches the wish, so the not-running case stays a normal
  notification sound (§13 D7).
- Text to speech (later): `AVSpeechSynthesizer.write(_:toBufferCallback:)` gives
  PCM buffers that enter the engine as a stream voice on the `voice` bus, so
  speech ducks music like any alert. Android: `TextToSpeech.synthesizeToFile`
  then a stream. The web's `speechSynthesis` cannot be routed into Web Audio; it
  plays directly and the engine ducks music by itself while it speaks.

Decision: programs state an intent, Rae owns the session; every platform event
becomes an ECS event; pausing music on a lost route is built in.

---

## 10. What a program sees

Sketches, to show the shape. Names settle at implementation; every binding is
typed, every argument named, as the language requires.

### 10.1 The simple API (games, SUMU)

```rae
open audio/Audio

# The engine is a resource on the App (no globals).
let audioSettings: AudioSettings = {
  session: AudioSessionKind.music
  buses: [AudioBusKind.music, AudioBusKind.effects, AudioBusKind.alerts]
}
var audio: AudioEngine = audioStart(settings: audioSettings)
# Ducking inside our own mix, run on the audio thread:
audio.duckWhilePlaying(source: AudioBusKind.alerts, target: AudioBusKind.music, gainDecibels: -12.0)

let alarmSound: Sound = loadSound(path: "assets/alarm.wav").get()
let voiceId: VoiceId = audio.play(sound: alarmSound, bus: AudioBusKind.alerts, gainDecibels: 0.0)
audio.fadeOut(voiceId: voiceId, seconds: 0.3)

# A long file: streamed, seekable.
let trackId: VoiceId = audio.playStream(path: "music/song.mp3", bus: AudioBusKind.music)
audio.pause(voiceId: trackId)
audio.seek(voiceId: trackId, seconds: 61.5)
let position: Float = audio.positionSeconds(voiceId: trackId)

# Once per frame, on the main thread: reads feedback, frees what the audio
# thread let go of, turns platform events into ECS events.
audio.update()
```

`play` is fire-and-forget; the `VoiceId` stays valid after the voice ends (it
then names nothing, and calls on it do nothing), so no program has to track
lifetimes. Calls never fail loudly: with no device (CI, a phone call in
progress) the engine runs silent and says so through `audio.state`.

### 10.2 The ECS layer (a DAW, a video editor)

Tracks, buses and sends are entities on the program's World:

- components: `AudioTrack` (kind, channel count), `AudioGain` (decibels),
  `AudioPan`, `AudioMute`, `AudioSolo`, `AudioOutput` (the bus's `busEntityId`),
  `AudioSend` (`targetEntityId`, decibels, `preFader`), `AudioInserts`
  (node kinds and their parameters), `AudioMeter` (written back from the audio
  thread);
- systems, each in its own folder (`packages/audio/graphSystem/GraphSystem.rae`, ...):
  `graphSystem` (structure changed → compile and send a plan, §5.2),
  `parameterSystem` (values changed → timestamped events), `feedbackSystem`
  (meters into `AudioMeter`, voice-ended and device events into ECS events),
  `timelineSystem` (clips, notes and automation → events and pre-roll, §6.3;
  in the media editor's repository, see §6.3);
- events: `AudioDeviceChanged`, `AudioInterrupted`, `AudioResumed`,
  `AudioRouteLost`, `AudioUnderrun`, `MidiDeviceAdded`, `MidiDeviceRemoved`,
  `VoiceEnded`.

The simple API of §10.1 is itself a fixed small graph built with this layer.

**Landed, the ECS layer (2026-11-13, `packages/audio` 0.12.10):**
`audio/AudioComponents` (AudioTrack with kind audio / bus / master, AudioGain,
AudioPan, AudioMute, AudioSolo, AudioOutput, AudioSend, AudioInserts,
AudioMeter; one `AudioEntities` holds the tables, the program makes the
entities with its own allocator) and three systems, each in its folder:
`graphSystem` (when the tracks, outputs, sends or inserts tables change: build
the graph, each node keyed `entityIndex * 64 + role`, compile it against the
running plan so state carries over, send it; free the plans given back),
`parameterSystem` (a fader's effective level with mute and solo, and pan, as
events for a sample time; the renderer cuts its block at that sample, so a
change lands exactly there at any block size, and a gain ramps over 5 ms) and
`feedbackSystem` (meter readings, pushed by the renderer after each call, into
AudioMeter; and `audioEventSystem`: the session's interruptions and routes,
underruns, ended voices and MIDI sources as `AudioEvent`s of kind
deviceChanged, interrupted, resumed, routeLost, underrun, midiDeviceAdded,
midiDeviceRemoved, voiceEnded: one event type with a kind, like
AudioSessionEvent, rather than eight types). Fixture 1438: a session of
entities null-tested against the formula with a mute inside a block and a
track added mid-run; blocks of 37 equal blocks of 128 bit for bit.

**Landed, the simple API on the graph (2026-11-13, `packages/audio`
0.12.11):** the engine's mixer renders a RenderPlan instead of its fixed bus
sum. Each bus is a `source` node (the voices render into one slot per bus) into
a new `duck` node kind, and the four duck nodes sum into the output. A duck node
is a bus fader with the mixer's 5 ms ramp and its in-mix duck in one product
(gain times duck level), in the old frame order, so the output is the same bit
for bit: an offline scene with a duck, `setBusGain`, `setBusGainAt`, a synth
note and mono output gives the same samples on 0.12.10 and 0.12.11 at blocks of
256, 37 and 64. `setBusGain` is now a ramp on the bus's duck node, applied at
its sample through the mixer's command ring (the ring that already cuts the
block); `duckWhilePlaying` sets the target node's duck rule and reads the
source bus's activity marks. The plan is built directly in `createAudioMixer`
rather than through the ECS layer, because the ECS chain adds a pan and a meter
node per track that the simple API does not have. The §10.1 calls and their
behavior are unchanged (fixtures 1401-1417).

### 10.3 Writing DSP

A node kind is a state struct plus its `prepare` / `process` / `reset`
functions (§5.3), written over `AudioBlock` views. It is ordinary Rae, so it is
tested offline like any function. How a program's own kind joins the engine is
§13 D5.

---

## 11. What lands first

Each step is one or more queue tasks; each ends with its headless tests (§12)
green.

**Where the DAW and video-editor work goes (2026-11-13).** This package holds
what many programs use: the device and the session, sounds and streams, the
mixer and its exact-sample commands, MusicalTime, the render graph and its ECS
mixer layer, a small set of built-in effects, and the interface through which a
program adds its own node kinds. What only an editor needs (the timeline,
hosting third-party plugins: CLAP, VST3, AUv3, and Rae plugins as WASM
modules) lives in the media editor's repository (`mediaeditor`) with its own
queue, built on this package. Steps below that name those parts are history
or now that repository's.

1. **Output and the simple API.** `packages/audio`: miniaudio device layer (macOS,
   iOS first; the other desktops build and get a manual check), stage A render
   thread, the command / feedback / reclaim rings, the engine with gain, pan,
   sum, bus, voice set, smoothing, the in-mix duck; WAV decode in Rae; `Sound`
   loading; the offline driver and the test helpers; the RT check;
   `AudioSessionKind` on iOS (category, options, interruptions, route loss).
   First users: SUMU's alarm sound, then game proto1 (its own queue), then delete
   `runtime_audio_sdl3.c`.
   **Landed, the macOS half (2026-11-11), `packages/audio` 0.1.0:** miniaudio
   0.11.26's device layer (`native/miniaudio/`, its LICENSE beside it), the C
   glue `native/rae_audio_glue.c` (device, the stage-A callback, the thread's
   time-constraint policy, file read and write), and in Rae `audio/AudioMixer`
   (voices, four buses, 5 ms ramps, equal-power pan, the in-mix duck,
   sample-accurate starts, the command / feedback / reclaim rings),
   `audio/Sound` (WAV decode of 8/16/24/32-bit PCM and float, float WAV
   encode, sounds made in code) and `audio/Audio` (the API of §10.1, the
   stage-A render thread, the offline driver with `RAE_AUDIO_DEVICE=offline`
   and `RAE_AUDIO_CAPTURE`). Measured on a Mac: rendering a 256-frame block
   takes about 4 µs of its 5.3 ms; a ring of two periods ran short, because
   Core Audio takes bursts of more than one period, so stage A keeps four
   (up to 21 ms). Fixtures 1401 (null test, block sizes, exact start, stop
   fade, duck, reclaim) and 1402 (WAV files written by Python, a worker load,
   the capture), both under `RAE_REALTIME_CHECK=1`; `checks/audioSine` plays
   on the real output with no underrun. Not yet: the iOS session (step 1's
   second half), `nextSafeSampleTime` with a device, and moving
   `compiler/runtime/runtime_audio_sdl3.c`'s user to this package. For the
   iOS session in Objective-C: a package's `native` build (`build.sh`, `cc`)
   compiles `.m` on the Mac, but the iOS project `tools/ios/gen-ios.sh`
   writes is `project(RaeApp C)`, so a `.m` in `sourcesDevice` would not
   compile for a device; that step adds `OBJC` there (and checks an iOS
   build), or calls Objective-C through C as the runtime's Apple code does.

   **Landed, the session half (2026-11-11), `packages/audio` 0.2.0:**
   `audio/AudioSession` (the intents and the plan of §9.1), the policy in
   `audio/Audio` (the strongest intent of what plays, activation before the
   first sound, deactivation a second after the last, a lost route and an
   interruption pause music, an interruption cuts alerts, its end resumes
   music when the system says so, `resumeMusic`), the events as ECS events
   through `audio/sessionSystem/SessionSystem`, and the headless fake mode
   (`RAE_AUDIO_FAKE_SESSION=1`: the calls are logged, `injectSessionEvent`
   plays the host). The iOS side is Objective-C in the same glue file
   (AVAudioSession's category, mode, options, activation, and the
   interruption, route-change and media-services-reset notifications into an
   event ring Rae polls); a device build compiles it as `src/rae_audio_glue.m`,
   and `tools/ios/gen-ios.sh` now declares `project(RaeApp C OBJC)`.
   miniaudio is told to leave the session alone (category none, no activate
   or deactivate). Checked here: the glue compiles for the iPhone and its
   simulator, a `C OBJC` Xcode project builds for the simulator, fixture 1404
   runs the policy headless, and `checks/audioSine` still plays on a Mac. Not
   yet run on an iPhone: queued as `[audio 2b/7]`, with the Focus experiment.

2. **Streams and the music player.** The butler; dr_mp3, dr_flac, stb_vorbis;
   AAC through AudioToolbox; the Rae resampler (`good`); seek, gapless next;
   Now Playing and remote commands; background play on iOS. SUMU's music player.

   **Landed, the streams half (2026-11-11), `packages/audio` 0.5.0:**
   `audio/Decoder` (MP3, FLAC, Ogg Vorbis and WAV through miniaudio's decoders
   and the `stb_vorbis` that miniaudio 0.11.26 ships, one C call each; the
   dr_mp3 and dr_flac code is miniaudio's own copy, so all three come from one
   pinned release), `audio/Resampler` (windowed sinc, Blackman, a 256-phase
   table with linear interpolation; worst error about 2.5e-5 for a 1 kHz sine
   44.1 to 48 kHz and back with 32 taps), `audio/AudioButler` (the streaming
   thread: a second of ring per stream, gapless next in the same ring, closes
   acknowledged so rings are freed safely) and `audio/AudioStreams`
   (`playStream`, `queueNext`, `pauseVoice` / `resumeVoice`, `seek`,
   `positionSeconds`, `durationSeconds`, `streamIsPlaying`). Offline, the
   butler runs one step before every block, so a test is repeatable. Fixture
   1411: FLAC bit-exact, MP3 and Vorbis within -30 dB, a stream equal to its
   file, gapless at the exact frame, a seek and a pause to the frame. Not yet:
   the MP4/M4A box reader and AAC through AudioToolbox, Now Playing and the
   remote commands, and iOS background play (queued after this step).

   **Landed, MP4 / M4A and AAC (2026-11-12), `packages/audio` 0.6.0:**
   `media/Mp4` in the stdlib (shared with video): the box tree, each track's
   handler, codec, format and codec configuration (esds with its
   AudioSpecificConfig, avcC / hvcC / av1C / alac), its sample sizes, file
   offsets, decode times, composition offsets and sync samples, and the
   playable part from the edit list, or from iTunes' `iTunSMPB` in Apple's
   M4A. Pure functions over bytes. `audio/AacDecoder`: the moov found by
   walking the top-level boxes with `pread` (wherever ffmpeg put it), each
   packet read where the table says and decoded by AudioToolbox's
   AudioConverter (loaded at run time, one call per shim; elsewhere opening
   answers "not supported" until the FFmpeg package), the priming and the end
   padding cut by the edit, a seek two packets early. `audio/Decoder` sniffs
   the first box, so `readAnySoundFile`, `playStream`, `queueNext` and `seek`
   take `.m4a` / `.mp4`. The butler now opens its own decoder from the path in
   the command (with the start frame of a seek), so a decoder's Rae state
   never crosses the ring. Fixture 1414: every packet of four files equal to
   ffprobe's; the decode equal to the platform's own decode of the file
   (afconvert) within 16-bit rounding, 22050 frames, no delay; seeks equal to
   a decode from the start (with ffmpeg's noise substitution off: it decodes to
   random noise); a stream equal to its decode, gapless into an Apple file, a
   seek in a stream to the frame.

   **Landed, Now Playing and background play (2026-11-12), `packages/audio`
   0.7.0:** `audio/NowPlaying` shows what plays (title, artist, album, an
   artwork image file, the current file's length and place, playing or
   paused) through MPNowPlayingInfoCenter, and turns on MPRemoteCommandCenter's
   play, pause, play/pause, next, previous and position buttons, on iOS and
   macOS (the glue is Objective-C on both; MediaPlayer is loaded at run time,
   its keys found by `dlsym`, so nothing new is linked). A button press goes
   into a queue of its own; `takeRemoteCommands` reads it and
   `audio/remoteCommandSystem` sends it as an ECS event. `applyRemoteCommand`
   does what play, pause, play/pause and a new position ask of a stream (a
   paused stream stays paused when moved); next and previous are the
   program's. `streamIsPaused`, `streamFileIndex`,
   `streamFilePositionSeconds` and `streamFileDurationSeconds` say what plays
   in a stream with queued files. The session sends `enteredBackground` and
   `enteredForeground` on iOS. `tools/ios/gen-ios.sh`:
   `RAE_IOS_BACKGROUND_AUDIO=1` adds the `audio` background mode. Fixture
   1415 (the buttons through the ECS system, applied to a stream);
   `checks/audioNowPlaying` on the iPhone: with the app in Settings for 27 s
   the song played on, callbacks kept coming, no underrun. Not checked by an
   agent: pressing the lock screen's and the Mac's media buttons (a person's
   check, queued). `packages/audio` 0.7.1: a `seek` moves within the file
   heard now and keeps the files queued after it (the stream's record keeps
   every path; file numbers stay counted from the stream's first file).
3. **The other platforms.** Android (AAudio, focus, MediaSession, AMediaCodec
   AAC), Windows (WASAPI, SMTC, Media Foundation AAC), Linux (PipeWire through
   Pulse/JACK, MPRIS), the web (AudioWorklet build, COOP/COEP in the web shell,
   decode through WebCodecs). Each with a real-device check in `checks/`.

   **Landed, Android's device (2026-11-12), `packages/audio` 0.9.0:** the
   package builds into an Android app (`tools/android/gen-android.sh`; the
   pack's new `sourcesAndroid` names the C glue, as `sourcesDevice` names the
   Objective-C copy iOS needs), and miniaudio plays through AAudio with stage
   B: `checks/audioSine` passes on the Android 35 emulator (AAudio, the
   callback renders, 0 underrun). `deviceBackendName` says which platform
   system plays ("Core Audio", "AAudio", ...). Not yet, each queued as its
   own step: the audio focus and MediaSession (Android), and Windows, Linux
   and the web, which need a machine or a design decision (see the queue).

   **AAC on Android (2026-11-12), `packages/audio` 0.9.1:**
   `audio/AacPlatform` puts the platform's AAC decoder behind
   `audio/AacDecoder`: AudioConverter on Apple (the esds payload as its
   cookie), the NDK's AMediaCodec on Android (loaded with dlopen, configured
   with the AudioSpecificConfig as csd-0; a packet queued, every ready output
   taken as 16-bit PCM and turned into Float in Rae; at the end an
   end-of-stream packet and more asking until the last frames come, as
   AMediaCodec holds frames back). `checks/audioAac` on the Android 35
   emulator: fixture 1414's two M4A files give 22050 frames, no delay, within
   6e-5 of afconvert's own decodes (one 16-bit step), seeks within -40 dB of
   the whole decode, a stream equal to its decode; the same check passes on
   the Mac.

   **Android's session (2026-11-12), `packages/audio` 0.10.0:** the package
   carries its Android Java (`native/android/.../RaeAudioSession.java`,
   listed in `sourcesAndroid`, compiled by `gen-android.sh` with the app's).
   `audio/AudioSession` turns an intent into the audio focus in Rae: music
   takes it (`GAIN`, media), an alert or spoken text takes it for a while and
   lets others duck (`GAIN_TRANSIENT_MAY_DUCK`, alarm / assistant), ambient
   asks for none; the engine asks when its session activates and gives it back
   a second after the last sound, as on iOS. A focus loss is
   `interruptionBegan`, its return `interruptionEnded`, a duck the new
   `ducked` event, "becoming noisy" `routeLost`. `audio/NowPlaying` is a
   MediaSession (metadata, playback state, play / pause / next / previous /
   position as RemoteCommandEvents). `RaeActivity` hands media keys to the
   system's media handling, as SDL took them as keys. The focus listener has
   a text of its own: Android matched an abandon by a text two Rae apps could
   share (one app's give-back removed the other's focus on the emulator).
   `checks/audioNowPlaying` on the emulator: the focus held and given back
   (`dumpsys audio`), pause / play / next / previous from `cmd media_session
   dispatch` and from `input keyevent KEYCODE_MEDIA_PLAY_PAUSE`, a duck under
   another app's alert, an interruption by another app's music.

   **The web (2026-11-14), `packages/audio` 0.12.14.** The choice, made so
   that no program's web build changes: **an AudioWorklet fed from the main
   thread through its MessagePort**, no shared memory. The worklet plays from
   a small queue of its own (eight periods: 2048 frames at 256 and 48 kHz)
   and asks the page for blocks as it drains; the page's handler renders each
   block with the engine's stage-B entry (`rae_audio_render`, the same
   function the native device callback calls) and posts it, transferred. So
   the browser build needs no SharedArrayBuffer, no COOP/COEP headers, no
   wasm workers and no `-sAUDIO_WORKLET`. miniaudio's device layer is off
   there (`MA_NO_DEVICE_IO`): its Web Audio backend renders in a
   ScriptProcessorNode, or in a worklet only with shared memory. The device is
   `native/rae_audio_web.c` (EM_JS), built into the pack's new `linkBrowser`
   library when emcc is found. A page with no AudioWorklet (plain http on a
   LAN address is not a secure context) gets a ScriptProcessorNode that
   renders in its own callback; `?RAE_AUDIO_WEB_OUTPUT=script` forces it, for
   the check. The browser has no threads, so `audio/Audio` keeps the
   streaming butler's and stage A's `spawn thread` under `when
   Target.isNative`: the web always renders in the callback (stage B), and
   `update` steps the butler. The AudioContext starts suspended until a click
   or a key (the autoplay policy); the device resumes it on the page's first
   one.

   **Measured latency** (headless Chrome 157 on a Mac, muted, 48 kHz, 256-frame
   periods): the worklet's queue 2048 frames (42.7 ms) plus what the context
   reports (`baseLatency` + `outputLatency`) 1815 frames (37.8 ms), 3863
   frames, about **80 ms** from a rendered frame to the speaker; the
   ScriptProcessorNode fallback 2327 frames (512 of its buffer, 48 ms). The
   native stage B is 395 frames (8 ms) on the same Mac. The shared-memory
   build (the engine inside the worklet, rendering a 128-frame quantum at a
   time) is the way below that: queued as its own step, opt-in per program.
   The clock's measured rate is rougher in the browser (48593 Hz over 429
   renders), because the page renders blocks in bursts as the worklet asks.

   **A program that plays sound in a browser runs in a `mainLoop`**
   (docs/web-frame-loop.md): the page renders between frames, and `sleep`
   in the browser busy-waits, so a program that sleeps holds the page and no
   block is rendered. `checks/audioSine` now runs its 2.3 s in a mainLoop (a
   2 ms sleep a turn natively) and passes in headless Chrome: 429 blocks
   rendered on request, both voices ended, no underrun.

   **AAC in the browser: WebCodecs' AudioDecoder**, behind the same per-packet
   C calls as Apple's AudioConverter (`rae_audio_aac_*`, configured with the
   AudioSpecificConfig). A packet is decoded and the call waits (EM_ASYNC_JS,
   through Asyncify) until the decoder has taken it, then copies what came
   out; only the end of the stream flushes, because Chrome resets the decoder
   on a flush and an AAC frame overlaps the one before (a flush per packet
   gave differences of 0.6). `checks/audioAac` in headless Chrome: both M4A
   files 22050 frames, no delay, within 1e-5 of afconvert's decodes; seeks;
   a stream. `compiler/tools/wasm_audio_check.sh` (`make -C compiler
   wasm-audio-check`, not in the full run) builds and runs both checks.

   **The shared-memory build, opt-in per program (2026-11-14, Rae 0.3.120,
   `packages/audio` 0.12.15).** A program's pack says `web: { sharedMemory:
   true }`; no other program's web build changes. Its browser build links with
   `-sAUDIO_WORKLET -sWASM_WORKERS -pthread` (shared memory; `-pthread` only
   for libc's pthread calls, the runtime keeps its single-thread kernel, which
   `RAE_WASM_THREADS` selects) and each package's `linkBrowserShared` library:
   the audio package's is built with atomics and `RAE_AUDIO_WEB_SHARED`, and
   its device (`native/rae_audio_web_shared.c`) runs the engine inside the
   AudioWorklet, on the worklet's own Wasm thread: each 128-frame quantum is
   rendered with `rae_audio_render`, as the native callback does, and the
   commands reach it through the SpscRing as natively. The engine's period is
   the quantum. Shared memory needs a cross-origin isolated page (COOP/COEP
   headers), which static servers rarely send: the build writes `rae-coi.js`
   beside the page, a service worker that adds the headers to every response,
   and the shell loads the program only once the page is isolated (the first
   visit registers the worker and reloads once; over plain http on a LAN
   address there is no service worker and the page says so).

   **Measured** (headless Chrome 157, muted, 48 kHz): 1943 frames, about
   **40 ms**, from a rendered frame to the speaker: the 128-frame quantum plus
   the 1815 frames the context reports, against 3863 (80 ms) for the queue
   path; the context's own 38 ms is now most of it. The clock measures 48000
   Hz exactly (a render per quantum). No underrun can be seen from the page
   (a late quantum is silence the browser does not report); 835-848 quanta of
   the about 860 in 2.3 s rendered, the rest before the worklet had loaded.
   `wasm_audio_check.sh` builds a copy of `checks/audioSine` that opts in and
   runs it under a plain server, so the service worker's isolation is tested
   too.
4. **Time.** Output host timestamps and the DLL clock; sample-accurate event
   lists in nodes; Transport and TempoMap; the block-size test; the video sync
   hook for the Media API.

   **Landed (2026-11-12), `packages/audio` 0.8.0:**
   - `media/MediaTime` in the stdlib (for video too): `Rational` and
     `MediaTime` (an Int count in a stated timebase), rescaling rounded once to
     the nearest (a half away from zero) without overflowing first, exact
     comparison across timebases. The engine's sample time is a MediaTime in
     1/sampleRate (`engineSampleTime`, `toEngineSamples`).
   - The mixer's event list: every command has a sample time; one for a later
     sample waits in a fixed list (256, filled once, no allocation on the audio
     thread) and the block is rendered in parts split at each event's frame.
     The duck follows its source bus per frame. A command for a past sample
     is applied at the next block and counted late. `audio/AudioSchedule`:
     `stopVoiceAt`, `fadeOutAt`, `setVoiceGainAt`, `setVoicePanAt`,
     `pauseVoiceAt`, `resumeVoiceAt`, `setBusGainAt`, with `playAt` and
     `nextSafeSampleTime`. There is no node graph yet; the mixer is the one
     node, and the graph's nodes take the same event list when they come.
   - `audio/AudioClock`: each render records (its first sample, the host time
     when it starts); the main thread adds the output latency and smooths the
     pairs with a second-order delay-locked loop (4 Hz while it locks, then
     0.05 Hz). The latency is the platform's: macOS, the output device's
     latency, safety offset and IO buffer (Core Audio, through the
     AudioObjectGetPropertyData miniaudio loads; AirPods Max report 7680
     frames, 160 ms); iOS, AVAudioSession's output latency plus the buffer;
     elsewhere miniaudio's buffer. It gives `hostTimeOfSample`,
     `sampleTimeAtHost`, `measuredSampleRate` and, for video (§6.5),
     `heardSampleTimeAt(hostNs)`. The host time is read when the render starts,
     not the platform's own timestamp (miniaudio does not pass Core Audio's
     mHostTime, WASAPI's QPC or AAudio's timestamp through); those come with
     each platform's step (§11 step 3).
   - `audio/MusicalTime`: `TempoMap` (tempo as microseconds per quarter note,
     meters, 960 ticks per quarter note; tick to sample summed in Ints and
     divided once; bars and beats) and `Transport` (play from a tick at a
     sample, stop keeping the position).
   - Fixture 1416: MediaTime rounding, tempo and meter changes, the loop on
     simulated timestamps of a 47998.7 Hz device with ±300 µs of jitter (the
     rate within 0.1 Hz, the mapping within 25 µs), and the block-size test:
     a start, a gain, a pan, a pause and resume, a bus gain, a fade, a stop and
     a duck in blocks of 1, 17, 64, 512 and of changing sizes, the same
     samples; each event on its frame; a late command counted. On the Mac's
     device: 6.55 s heard in 6.55 s of host time, no underrun.
5. **Input and MIDI.** Duplex devices, recording to file with latency
   correction, permissions; MIDI in/out on CoreMIDI first, then the others; a
   small synth node to play.

   **Landed, macOS first (2026-11-12), `packages/audio` 0.11.0:**
   - Input: `AudioSettings.inputChannels` opens the device as duplex (the
     default input in the same callback, one sample clock); the callback hands
     each period's input to Rae (`realtime cExport("rae_audio_capture")`,
     `captureInput`) into a ring with the output sample time of its first
     frame. `audio/AudioInput`: `beginRecording` / `recordInto` /
     `finishRecording`; the round trip (output latency plus input latency,
     from Core Audio's input scope on macOS, AVAudioSession's inputLatency on
     iOS) is cut, so frame i of a take lines up with output sample
     `alignedStart + i`. `microphonePermission` / `requestMicrophone`
     (AVCaptureDevice; a check never asks by itself).
   - MIDI: `audio/Midi` parses bytes in Rae (running status, real-time bytes
     inside a message, system exclusive, a note on at velocity 0 as a note
     off, pitch bend); `audio/MidiPorts` is CoreMIDI (loaded at run time):
     sources and destinations by name, input ports, virtual source and
     destination, send; the read callback only queues packets with their host
     time.
   - `audio/AudioSynth`: a 32-voice synth inside the mixer (sine and octave,
     attack / decay / sustain / release), its notes mixer commands through the
     event list, so they start on their sample (`playNoteAt`,
     `releaseNoteAt`, `playMidi`).
   - Fixture 1417 (the parser, the synth on its sample and in blocks of 1, 17,
     64, 512 and changing sizes, playMidi, a recording's alignment and lost
     samples), clean under RealtimeSanitizer. `checks/audioMidi` (a loopback
     through the system's MIDI server) and `checks/audioInput` (2 s of the
     built-in microphone: none lost, round trip 725 frames on a MacBook's
     speakers and microphone) pass on the Mac. Not yet: Android, Windows,
     Linux and the web's input and MIDI, and the Apogee Duet (not plugged in),
     each queued.

   **Android's input and MIDI (2026-11-12), `packages/audio` 0.12.0:** the
   microphone permission in the package's Java (`checkSelfPermission`, a flag
   of our own for "asked and refused", `requestPermissions` on the UI thread;
   `tools/android/gen-android.sh` writes `RECORD_AUDIO` when
   `RAE_ANDROID_PERMISSIONS` names it), the duplex device through AAudio
   (miniaudio; the input's latency is the device's buffer there), and MIDI:
   `RaeAudioMidi` lists and opens devices with MidiManager, and the C reads
   and writes their ports with AMidi (loaded at run time) into the same queue
   CoreMIDI fills, so `audio/Midi` parses it the same. On the Android 35
   emulator: `checks/audioInput` records (permission 0 before `pm grant`, 3
   after; nothing lost; round trip 8720 frames); `checks/audioMidi` finds
   MidiManager and no devices. Android gives a program no virtual MIDI ports
   (they need a MidiDeviceService), so the AMidi read and send path compiled
   but did not run here; a loopback device to test it is queued.

   **The loopback (2026-11-12), `packages/audio` 0.12.1:** `RaeMidiLoopback`,
   a MidiDeviceService whose input port sends to its output port, is the
   package's Android part `midiLoopback` (its manifest service and its
   `xml/` device description), added to an app only when its build says
   `RAE_ANDROID_PARTS="midiLoopback"`. `midiSourcesReady` waits until the
   asked-for sources are open (Android opens a device a moment after it is
   asked; what it sends before is not heard). `checks/audioMidi` on the
   emulator sends its five messages (notes with running status, system
   exclusive, pitch bend, a controller) to the loopback and receives all
   five back through AMidi, as on the Mac.
6. **The DAW engine.** The ECS layer (§10.2) in full, plan compilation with
   delay compensation, EQ / compressor / reverb nodes, parallel audio workers
   with workgroups/MMCSS, stage B (once the FFI callback exists), CLAP hosting,
   then the Rae plugin ABI (native + WASM, `wasm-modules.md`), then VST3 and
   AUv3.

Text to speech (SUMU) attaches after step 2, using the stream voice.

---

## 12. Testing, headless

The engine is a function from (plan, events, inputs) to samples, so it is tested
without a device.

- **The offline driver.** `audioStartOffline(sampleRate:, blockFrames:,
  channels:)` gives an engine with no device: the test calls `renderFrames(count:)`
  and gets the samples, as fast as the CPU goes. With `RAE_AUDIO_DEVICE=offline`
  (set by the test runner, like `RAE_UI_HEADLESS`) a normal program's
  `audioStart` also gets the offline driver, writes what it "played" to
  `RAE_AUDIO_CAPTURE=<file.wav>` if set, and logs `[audio] play <sound> bus=<bus>
  at=<sampleTime>`, so an app test (SUMU: "the alarm played and ducked the music
  by 12 dB") can check both the log and the samples.
- **Reference renders.** A fixture renders a scene and compares it with a
  reference WAV in the fixture folder. Floating point differs between x86-64 and
  ARM64 (fused multiply-add), so the compare is a **null test**: subtract, and
  require the peak of the difference below −100 dBFS (and report it). References
  are short (0.25-1 s, mono or stereo, 16-bit is enough at that threshold) to
  keep the repository small. A reference is made by an independent path where
  one exists (a sine by formula, a WAV decoded by a second decoder), never only
  by the code under test.
- **Block-size invariance.** The same scene at block sizes 1, 17, 64, 512 and a
  random varying size: the outputs must be equal within the null-test threshold.
  This is the test of sample-accurate scheduling (§6.2).
- **Real-time safety.** Every audio fixture runs with `RAE_REALTIME_CHECK=1`:
  zero allocations, frees, locks and `log` calls on the audio thread, or the case
  fails (§4.3). The normal leak check at exit also applies.
- **Decoders.** Each against reference PCM: the FLAC test vectors bit-exact, MP3
  against the ISO/IEC 11172-4 compliance limits, Vorbis and AAC against a second
  decoder with a tolerance. Seek accuracy: seek to a frame, compare with the
  frames from a full decode.
- **Resampler.** Sine sweeps at each preset: THD+N and aliasing below set limits,
  compared with libsamplerate's numbers.
- **Clock.** The DLL fed with simulated timestamps that jitter and drift: the
  estimated rate and offset within limits.
- **MIDI.** Byte streams with running status, SysEx split across packets,
  realtime bytes inside SysEx: parsed events equal the expected list.
- **Platform events.** The iOS session shim has a fake mode in headless runs
  that injects interruption and route events from the test, so the Rae policy
  (pause on a lost route, resume after a call) is unit-tested.
- **On real hardware, not in the unit suite** (`checks/audio*`): open the
  default device, play a sine, count underruns over 60 s while a CPU load runs;
  measure round-trip latency with a loopback cable; the web worklet build in
  headless Chrome (sample counts, not sound).

The unit cases live in `compiler/tests/cases/` like all others; the scenes are
small Rae programs.

---

## 13. Decisions still needed

- **D1 — vendored C. Decided 2026-11-11: yes.** miniaudio (device layer only)
  and the decoders dr_mp3, dr_flac, stb_vorbis (later libopus) are copied into
  the package's `native/` folder, each with its license file: proven,
  maintained libraries are used rather than rewritten, behind the Rae API that
  keeps each replaceable. They are permissive (public domain / MIT-0 / MIT /
  BSD), so they can live in this repository; LGPL code (FFmpeg) does not.
- **D2 — calling Rae from C** (stage B, §4.2). Decided 2026-11-11: the
  `cExport("cName")` modifier (`realtime-language-design.md` §6.1).
- **D3 — marking real-time functions.** Decided 2026-11-11: the `realtime`
  and `noAlloc` keywords (`realtime-language-design.md` §3). The run-time check
  comes first.
- **D4 — `Atomic(T)`.** Built first, as queue step `[realtime 1/6]`.
- **D5 — a program's own node kinds** (§5.3): generic engine over one program
  type, WASM nodes, or wait for D2.
- **D6 — where it lives. Decided 2026-11-11: a package in this repository,
  `packages/audio`** (and video likewise, `packages/video`), with a `native`
  block like `packages/sql`. A program opts in by declaring it in its
  `.raepack` (`dep audio: { git: ..., subdir: "packages/audio", version: ...,
  hash: ... }`), as AGENTS.md "Packages in this repo" describes; a program that
  does not, never builds or links miniaudio. Consequences: a change reaches the
  users only by a release of the package (its `meta.version`, the tag
  `audio/v<version>`, `rae update audio` in each user); the package sees only
  `lib/` and the packages its own pack declares, so the language-level parts
  (`Atomic(T)`, `Signal`, `SpscRing(T)`, the run-time real-time flag) stay in
  `lib/` and the runtime; the platform shims live in the package's `native/`,
  not in `compiler/runtime`. Check in the first step that a `native` block can
  build Objective-C (`.m`) for the Apple session shims, and extend it if not.
  The game engine is to move to a package the same way later.
- **D7 — SUMU's alarm and Focus** (§9.2): whether SUMU can hold the
  Communication Notifications capability to read Focus, and that the not-running
  case stays a normal notification sound. Settle on the phone during step 1.
- **D8 — ASIO / native PipeWire / Oboe**: only after measurements on real
  hardware show the default backends are not enough.

---

## 14. Style

Everything in `packages/audio` follows AGENTS.md: PascalCase module files in camelCase
folders (`packages/audio/Audio.rae`, `packages/audio/graphSystem/GraphSystem.rae`), no
globals (the engine is a resource on the App), no `pub`, no `@` attributes, typed
bindings, full words (`sampleRate`, `deltaTime`, `gainDecibels`, never `sr`,
`dt`, `db`), US spelling.
