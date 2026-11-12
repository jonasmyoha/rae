# Real-time code in Rae: what the language needs

Status: **proposal, nothing implemented** (2026-11-11). New syntax needs
approval (AGENTS.md "Scope boundaries"); §9 lists the decisions. Decided
2026-11-11: the keywords `realtime` and `noAlloc` (R1, R2), marks on the entry
function with optional marks on helpers (R3), `cExport("cName")` (R4), and
Homebrew LLVM for RealtimeSanitizer (R7). R5 and R6 wait.

`audio-design.md` §2 and §4.3 list the rules every audio engine follows on its
real-time thread: no allocation, no locks, no system calls that can wait, no
unbounded work; single-producer single-consumer queues both ways; objects the
real-time thread lets go of are freed by another thread; denormal numbers
flushed to zero; the worst case matters, not the average. `video-design.md`
has the same needs in a softer form (a frame every 16 ms, decode threads, the
per-frame render). Games want the softest form: "no allocation per frame".

Today these rules are only conventions. This document asks which of them the
**compiler** can hold, in Rae code and in the C that Rae links, what the
keywords would be, and what threading features are missing.

---

## 1. What Rae already has to build on

More than expected:

- **A complete call graph.** Rae has no function values and no virtual calls,
  so the compiler sees every call. Generics are analyzed per specialization.
  This is what makes a whole-program effect check cheap and exact.
- **The may-wait analysis** (`compiler/src/may_wait.c`,
  `lightweight-spawn-design.md` §4.1, §11). A fixed point over that call graph
  that finds which functions can wait (`Task.get`, sleeps, socket waits, joins,
  `blocking` externs), with a report (`rae build --report-waits`) that names
  the chain. A real-time check is the same machinery with more kinds of effect.
- **The `blocking` modifier on externs** (decided 2026-10-08):
  `func readFile(path: String) blocking unsafe extern("…") ret String`. It is the
  precedent for marking a C function's behavior that the compiler cannot see,
  as a plain keyword in the modifier position.
- **Parameter modes** (`own` / `copy` / `view` / `mod`) and scope-exit
  deallocation: the compiler already knows where every heap value is created,
  deep-copied and freed. That is exactly the "allocates" effect.
- **`Array(T, cap: N)`**: a value aggregate with a fixed capacity, no heap
  (`collections.md`). The natural container for real-time state.
- **`Float4` / `Float8`** for SIMD.
- **It compiles to C, with clang.** Two clang features check real-time rules in
  C, and both were tried on this Mac (2026-11-11), §5:
  - `__attribute__((nonblocking))` and `nonallocating` with
    `-Wfunction-effects`: a compile-time check. Apple clang 21 has it. It warned
    on a function marked `nonblocking` whose callee calls `malloc`, and named the
    chain.
  - **RealtimeSanitizer** (`-fsanitize=realtime`, LLVM 20+): a run-time check
    that stops on `malloc`, `free`, `pthread_mutex_lock`, blocking system calls
    and the like inside a `nonblocking` function, with a stack trace. Homebrew
    clang 22 runs it on macOS; **Apple's clang does not** ("unsupported option").

---

## 2. The effects to track

| effect | what counts in Rae | what counts in C |
|---|---|---|
| **allocates** | creating a heap value: a `List` or `String` literal or interpolation, `List.add` that can grow, a struct literal that owns heap, a **deep copy** (`let x: T = v` and `copy` of anything that owns heap), `opt` of a heap value, a hash map insert, an ECS component add, `log` with interpolation | `malloc`, `calloc`, `realloc`, `new`, `posix_memalign`, … |
| **frees** | an owned heap value reaching the end of its scope, an assignment that drops the old value, `List.remove` of a heap element | `free`, `delete` |
| **locks** | `Channel(T)` (mutex-guarded today), any `Shared(T)` if it is ever added | `pthread_mutex_*`, `os_unfair_lock`, `dispatch_sync`, … |
| **waits** | the may-wait set: `Task.get`, joins, `sleep`, socket waits, `spawn` (creating a thread) | `blocking` externs; `read`, `write`, `poll`, `usleep`, … |
| **unbounded** | a `loop` whose end the compiler cannot bound | — |

The first four are decidable over the call graph. The fifth is not, in general
(§4.4).

Allocation in Rae is mostly **visible but quiet**: the line `let state: Mixer =
mixer` reads like an assignment, but it is a deep copy that allocates if `Mixer`
holds a `List`. In real-time code that line must become `let state: view Mixer
= mixer`. The check is what makes this mistake loud, which is the most valuable
thing it does for Rae specifically.

---

## 3. The keywords

### 3.1 Recommendation: `realtime`, and later `noAlloc`

```rae
func renderBlock(engine: mod AudioEngineState, block: mod AudioBlock) realtime {
  ...
}
```

- **`realtime`** on a function: it and everything it can reach neither
  allocate, free, lock nor wait. It is the strict set, for the audio thread, MIDI
  timing, an audio worker, a control loop.
- **`noAlloc`** on a function (built together with `realtime`): it and everything it reaches neither allocate nor
  free. Locks and waits are allowed. For "no garbage per frame" in a game's
  update or a renderer's per-frame record. `realtime` includes it.

Position: the modifier position after the parameters, where `blocking` already
sits (`func f(...) realtime ret T`), so the formatter and parser have one place
for function modifiers.

### 3.2 Names considered and not chosen

- **`audio` / `video`**: these name a domain, not a property. The same rules hold
  for a MIDI clock, a physics step on a fixed deadline, a robot's control loop
  or a network tick. A keyword should say what the compiler checks.
- **`nonblocking`** (clang's word): Rae already has `blocking` for "C that may
  wait a long time", so `nonblocking` would read as "does not wait", while the
  check also bans allocation and locks. Confusing next to `blocking`.
- **`noalloc`**: a short keyword is fine (Rae has `ret`, `mod`, `func`, `opt`;
  the no-abbreviations rule is for names in code, not keywords), but a keyword
  of two words is camelCase (`taskScope`, `parallelLoop`, `mainLoop`), so the
  short form is `noAlloc`. `noAllocation` is the spelled-out alternative; the
  short one is chosen because a modifier keyword is read on many lines, like
  `mod` and `ret`, and "alloc" is the word every programmer knows from `malloc`.
- **`@realtime`, `#[no_alloc]`**: no `@` attributes, by rule.
- **Separate `noLock`, `noWait`**: no use found that needs one without the
  other; `realtime` covers the set. Add one only when a real program needs it.

### 3.3 Declared at the root, inferred below

Only the **entry point** of real-time code needs the keyword: the function the
device callback calls, a node's `process`. Every function it reaches is checked
without being marked, as the may-wait analysis does. Marking is **also
required** on a `lib/` function that promises to stay real-time-safe for its
callers (`AudioBlock.mix`, `SpscRing.push`): otherwise a later change to its
body breaks callers in other projects without an error in `lib/` itself. Such a
promise is checked like a root.

### 3.4 Externs (C called from Rae)

An extern is a leaf the compiler cannot see into. Rule: **an extern is not
real-time-safe unless it says so**:

```rae
func sinFloat32(x: Float32) realtime unsafe extern("sinf") ret Float32
func ringPublish(ring: Ptr, index: Int) realtime unsafe extern("rae_ring_publish")
```

On an extern `realtime` is a promise, like `blocking`; it is checked at run time
(§5.2), not trusted blindly. `bindgen` can carry the mark over from a C header
that already declares `[[clang::nonblocking]]`. The runtime's own math, atomic
and ring shims get the mark in `lib/`.

### 3.5 What an error looks like

```
Mixer.rae:42:5: error: `renderBlock` is realtime, but this line allocates:
  a `String` interpolation.
    renderBlock (Engine.rae:18) -> mixVoice (Mixer.rae:30) -> here
  Real-time code may not allocate. Format the text outside the audio thread,
  or send a number back on the feedback ring.
```

```
Engine.rae:77:3: error: `renderBlock` is realtime, but `plan` is freed here
  when it goes out of scope (an own `RenderPlan` holds a List).
  Push it to the reclaim ring instead: `reclaim.push(value: own plan)`.
```

```
Voice.rae:12:3: error: `renderBlock` is realtime, but this is a deep copy of
  `Sound` (it holds a Buffer). Use `let sound: view Sound = voice.sound`.
```

Every error names the chain from the root, as `--report-waits` does, and the
fix. `rae build --report-realtime` prints the whole set: each root, what it
reaches, and the externs it trusts.

### 3.5a Landed (2026-11-11, compiler 0.3.85)

`realtime` and `noAlloc` parse as modifier words where `blocking` does. The
may-wait walk (`compiler/src/may_wait.c`) records each function's own effects
with the first site of each; `compiler/src/realtime_check.c` carries them up
the call graph to a fixed point and checks every marked function as a root
(a marked helper is a root too), in the backend right after the graph is
built. What counts:

- allocates: a String interpolation (it also frees its temporary), a list or
  collection literal, a deep copy into a `let` from a place, a spawn, and C
  not marked `realtime` / `noAlloc`;
- frees: a `let` / `var` whose type owns heap (freed at the end of its scope;
  a value moved out is still counted, see below), an interpolation, unmarked C;
- locks and waits: `Task.get`, a task joined at scope end, a spawn, and
  unmarked C (a Channel's send is C, so it counts).

Method calls that sema leaves to the backend (a generic container's
`list.add(...)`) are resolved by name as effect-only edges, which the
may-wait answers do not follow. Errors with the same cause are one message
(`this allocates, frees, locks and waits: it calls rae_ext_rae_buf_alloc`).
`lib/core`'s `runtimeWarning` / `indexOutOfRangeWarning` are the crash path
(§3.6). Marked in `lib/`: the math, clock, `buf_zero` / `buf_prefetch`,
`rae_str_len`, atomic, signal-post and real-time-mark externs (`realtime`),
`rae_ext_rae_signal_wait` and `Signal.wait` (`noAlloc`), and the Rae
functions that promise it (`Atomic`'s operations, `Signal.post` / `tryWait`,
`SpscRing.push` / `pop` / `count`, the `Realtime` mark and recorder calls).
Fixtures 1405 (accepted), 1406 (each rule broken), 1407 (the report).

Known gaps, conservative or open: a heap value moved out of a `let` is still
counted as freed; generic C intrinsics (`rae_ext_rae_buf_get/set/drop_at`)
are not nodes, so a `drop_at` of a heap element is not seen; assignments of
heap values (`x = y`) are not counted yet; the error points at the site
(sometimes in `lib/`), with the chain from the marked function in the
message.

### 3.6 Cases that are allowed on purpose

- **The crash path.** A failed bounds check or `unreachable` ends the program;
  the text it builds on the way out is not a real-time violation.
- **`Ptr` / `Buffer` writes** into memory reserved earlier: no allocation.
- **`Array(T, cap: N)`** values, `Float4`/`Float8`, numbers, enums, plain
  structs: stack values.
- **A `List` read** (`viewAt`, iteration) and a write to an existing element:
  no growth, no allocation. `add` is not allowed unless the list is a bounded one
  that cannot grow (§6.3).

---

## 4. Rules beyond the effects

### 4.1 Values that cross between threads

A value that goes into a real-time ring must not own heap, or the real-time side
would end up freeing it. The compiler already sorts types this way for `spawn`
(plain values: numbers, enums, POD structs, `Array(T, cap: N)` of them). The
proposal: the ring type `SpscRing(T)` accepts only such plain `T`, checked when
the generic is specialized, with an error that says which field owns heap.
Larger things (a new render plan, a decoded sound) cross as a **handle**: an
index into a table the main thread owns, plus the reclaim ring for giving it
back.

**Landed (2026-11-11):** `lib/collections/SpscRing.rae`. `createSpscRing` calls
the intrinsic `rae_ext_rae_require_plain(T)`, which the C backend checks where
the specialization binds `T` (`c_call.c`, with `type_heap_part` in
`ownership.c`) and emits nothing for. The error names the path to the heap:
"a SpscRing of `Envelope` is not allowed: `Envelope` owns heap (its field
`Envelope.message`: its field `Message.text`: it is a `String`)". It points at
the line in `SpscRing.rae`, not at the caller's `createSpscRing`, because the
check runs per specialization; the type named in it says which call it was.
Fixture 1397.

### 4.2 Ownership out of real-time code

"Objects the audio thread drops go back to another thread" becomes a compile
rule: in `realtime` code an `own` heap value can only leave by being moved (into
a ring, into a field, returned). The free at scope end is the "frees" effect,
so the error of §3.5 already enforces it.

### 4.3 Threads may only call what their role allows

Two roles beyond `realtime` already exist in practice: platform UI and window
APIs are main-thread only (`concurrency-model.md` §3), and some iOS calls
(`AVAudioSession` changes) are best kept there too. A later `mainThread` mark on
externs would let the same analysis reject a `spawn`ed or `realtime` function
that reaches one. Not needed for the first audio step; listed as §9 R5.

### 4.4 Unbounded work

The compiler cannot prove a `loop` ends in time. What it can do:

- `loop i in 0 ..< count` is bounded by `count` at entry; that is ordinary.
- A `loop` on a condition in `realtime` code gets a **report line** (not an
  error) in `--report-realtime`, so a reviewer sees each one.
- The real check is **measurement**: the runtime records the time of every
  callback (CPU cycles at entry and exit), keeps the maximum and a histogram,
  and the test suite fails a scene whose worst block takes more than a set share
  of its period (§7). "The worst case matters" is a measured number, not a
  proof.

### 4.5 Floating point on real-time threads

- Denormals: the shim sets flush-to-zero on each real-time thread; no language
  change.
- Determinism of offline renders: the C compiler may fuse `a * b + c` into one
  instruction on ARM64 and not on x86-64, so offline renders differ slightly by
  machine. The audio and video tests already compare with a tolerance. A build
  option that turns contraction off (`-ffp-contract=off`) for bit-exact
  reference renders is cheap to add; it is a flag, not a language feature.

### 4.6 No aliasing, faster loops

A DSP loop over two buffers vectorizes well only when the C compiler knows they
do not overlap. Rae's modes already say it: a `mod` parameter and a `view`
parameter of the same call cannot alias (the borrow rules forbid it). Emitting
C's `restrict` on such `Buffer` pointers would let clang vectorize the loop
without a run-time overlap check. A code generation change, no syntax; it needs
the aliasing rule audited first.

**The audit (2026-11-12): the rule does not exist yet, so no `restrict`.**
Probed against compiler 0.3.102 (`stress/13_modAndViewOfOnePlace`):

- One place may be passed to a call as `mod` and as `view` (the callee grows
  the list through the `mod`; the `view` reads 1 item, then 101), as two
  `mod`s, and a field as `mod` with its whole struct as `view`. The borrow
  rules refuse overlap with a *held* borrow (a `viewAt` binding, a collection
  loop: stress cases 11 and 12), not between two arguments of one call.
- `Buffer(T)` is a plain handle: copies of it point at the same memory and
  nothing tracks them, so two Buffer parameters can never be proved apart.
- Even for Lists, `restrict` on the parameter (a pointer to the List's header)
  would not reach the loop that matters: the elements are another object,
  reached through `data`. The useful form is a `restrict` copy of each data
  pointer in a counted loop (`c_loop_versioning.c` already makes the
  check-free copy), sound only once two arguments cannot overlap.

So the order is: first a rule that a `mod` argument may not overlap any other
argument of the same call (Swift's exclusive access, Rust's borrow rule;
refused at compile time where the places are known, as a local, a field or a
whole versus its part), which is a language change (§9 R8); then `restrict` on
the List data pointers of a counted loop over arguments the rule proves
apart. Buffers stay out (they are the `unsafe` handle layer), unless a
program states it with a future, explicit, checked-at-run-time promise.

---

## 5. Checking the C that Rae links

Rae code is checked by the compiler (§3). The C side has three layers, from
cheapest to strongest.

### 5.1 Compile-time: clang function effects

Every `realtime` Rae function is emitted with `__attribute__((nonblocking))`
(and `noAlloc` with `nonallocating`); every extern marked `realtime` gets
the attribute on its C declaration. The C build of a test runs with
`-Wfunction-effects -Werror=function-effects` for those files. Then clang checks
the generated C **and** any C shim or library compiled in the same build that a
real-time function calls: a shim that calls `malloc` is an error at build time.
Clang infers the property for functions whose body it sees in the same file, and
reports a call to a C function it cannot see. This is a second, independent
check of the Rae analysis too.

Tried here: Apple clang 21.0 with `-Wfunction-effects` printed "function with
'nonblocking' attribute must not call non-'nonblocking' function 'helper'" and
the note "cannot be inferred 'nonblocking' because it calls … 'malloc'".

**Landed (2026-11-11):** the backend writes `RAE_NONBLOCKING` after the
parameters of every `realtime` function's prototype and definition, and
`RAE_NONALLOCATING` for `noAlloc` (`c_effect_attribute`, c_backend.c); a
user extern's generated prototype gets it too. `rae_runtime.h` defines them as
clang's `nonblocking` / `nonallocating` where `__has_attribute` says so (empty
elsewhere, gcc included) and puts them on the runtime C that `lib/` marks: the
math functions that are not inline, `rae_ext_nowNs`, the signal post /
try-wait and the real-time mark (`nonblocking`), the signal wait
(`nonallocating`). The crash-path helpers (`rae_int_trap`,
`rae_list_set_out_of_range`) are marked too, with the check turned off in
their own bodies (`RAE_COLD_PATH_BEGIN/END`, §3.6). The real-time fixtures'
C compiles with `-Werror=function-effects` with no finding; a `noAlloc`
function that interpolates is found by clang as well as by Rae (1407).

A C function's own header must carry the attribute for clang to trust it:
clang reports a redeclaration that adds it ("does not match previous
declaration"), so the backend cannot add it to a prototype it does not write.

### 5.2 Run-time: RealtimeSanitizer

`rae build --sanitize realtime` (a test mode, like the TSan gate) builds with
`-fsanitize=realtime`. Every `nonblocking` function (every `realtime` Rae
function, through §5.1's attribute) is a real-time context; RTSan intercepts
`malloc`, `free`, locks and blocking system calls made from it, **including
inside prebuilt C libraries** that have no source in the build, and stops with
the stack. This is what checks miniaudio's callback path, the decoders if they
are ever called on the audio thread, and the `realtime` promises on externs.

Tried here: Homebrew clang 22.1 with `-fsanitize=realtime` reported
"Intercepted call to real-time unsafe function `malloc` in real-time context"
with the chain `malloc ← helper ← render`. Apple's clang refuses the option, so
the gate needs the Homebrew LLVM (like `naga`, a test dependency that `make
setup` installs), or runs on Linux.

**Landed (2026-11-11):** `rae run --sanitize realtime` (and `rae build`
accepts it) compiles with LLVM's clang (`RAE_RTSAN_CC`, else Homebrew's
`/opt/homebrew/opt/llvm/bin/clang`; an error naming `brew install llvm` when
neither is there), adds `-fsanitize=realtime`, and checks the program's own C
with `-Werror=function-effects` instead of `-w`. The gate
`compiler/tools/realtime-sanitize-check.sh` (`make -C compiler
realtime-check`, a pre-suite case of every full run) does both for the
real-time fixtures, and plants a liar (C promised `realtime` whose body
calls malloc in another file, invisible to clang) that RealtimeSanitizer must
catch; it SKIPs the sanitizer part with a message without LLVM. `make setup`
installs LLVM with Homebrew on a Mac. Its first run caught a real bug: the
first `markRealtimeThread()` registered the check's exit report with
`atexit`, which locks; the runtime now registers it at startup.

### 5.3 Rae's own runtime check

The runtime check of `audio-design.md` §4.3 (`RAE_REALTIME_CHECK=1`: a
thread-local flag set on the audio thread, checked in Rae's allocation, free,
lock and `log` entry points). It works with any compiler and in release builds,
and covers the paths RTSan does not see (Rae's own string pool, buffer
allocator). It is the first step because it needs no language change.

**Landed (2026-11-11):** `lib/Realtime.rae` (`markRealtimeThread`,
`unmarkRealtimeThread`, `realtimeThreadIsMarked`) over
`compiler/runtime/runtime_realtime.c`. The mark is a `_Thread_local` flag. The
guard `RAE_REALTIME_GUARD` sits in `rae_ext_rae_buf_alloc` / `free` /
`resize`, `rae_mem_str_tag` / `untag` (every String body, named by its site:
`interp`, `int_to_str`, ...), the `RAE_LOCK` macro, a Channel's lock,
`rae_ext_rae_log_write`, and the task entry points (`rae_task_new`,
`rae_task_await`, `rae_task_drop`, `rae_parallel_for`). A hit claims a slot of
a fixed table with a compare-and-swap, so recording never allocates or locks.
The first mark arms an exit handler that prints one line per entry point
(`rae_ext_rae_buf_alloc (a Buffer allocation): 3`) and ends the process with
code 70; `rae run` reports that as a failed run. The bookkeeping locks of
`RAE_MEM_STATS` and the buffer records are not counted: the allocation they
belong to is. Fixtures 1398 (caught), 1399 (a clean real-time loop with the
ring, atomics, signal and recorder).

| check | sees | when | needs |
|---|---|---|---|
| Rae `realtime` analysis | all Rae code | build | the keyword |
| clang `-Wfunction-effects` | generated C + C compiled with it | build | Apple or LLVM clang |
| RealtimeSanitizer | everything that runs, prebuilt C included | test run | LLVM clang (not Apple's) |
| Rae runtime flag | Rae's runtime allocator and locks | any run | nothing |

---

## 6. Threading features that are missing

### 6.1 Calling Rae from C: `cExport` (decided)

The device calls the audio callback; VideoToolbox calls its decode-output
callback; CoreMIDI calls its read callback; Box3D wants a scheduler callback.
Rae cannot hand a function to C today (`audio-design.md` §4.2, decision D2). The
proposal keeps the call graph complete: not function values, but **a named Rae
function exported under a C name**, which C may call from any thread:

```rae
func audioRender(engine: Ptr, output: Ptr, frameCount: Int) realtime cExport("rae_audio_render") {
  ...
}
```

- The compiler emits a C function with that exact name and a C-ABI signature
  (numbers, `Ptr`, `Bool`; no `own` heap parameters, no `String` result).
- An exported function is a **root** of every analysis: it is treated as callable
  from an unknown thread, so the `spawn` capture rules apply to what it touches,
  and `realtime cExport` is checked like any `realtime` root.
- Why `cExport` and not `export`: Rae already uses `export` for modules (a
  re-export, `export "path"`, and a bare `export` that joins a file to its
  folder's main module). The C boundary already has its own words, `extern`
  (call C) and `cheader`; `cExport` reads as their pair and says which way the
  call goes. camelCase, as a keyword of two words is.

**Landed (2026-11-11):** `cExport("cName")` parses in the modifier position
(any order with `realtime`, `noAlloc`); the parser refuses it on an extern, on
a generic function, with a name that is not a C identifier, and with a
parameter or result that is not a C value (a number, `Bool`, `Ptr`,
`Buffer(T)`; never `mod`, `opt`, `String` or a struct). The backend writes,
after the prototypes, `RAE_EXPORTED <ret> cName(<params>)` (with
`RAE_NONBLOCKING` when the function is `realtime`) that calls the static Rae
function, and reports two functions with one C name. `rae format` keeps the
name. The function is an ordinary node of the may-wait graph, so the
real-time check covers a `realtime cExport` as a root like any marked
function. Fixtures 1408 (C calls three exports, one from a thread it starts),
1409, 1410 (the refusals); 1408 is in the TSan and real-time gates. Not yet:
the may-wait analysis does not mark an export as "called from an unknown
thread" (it is never made a resumable twin, which is right for C callers),
and no package uses it yet.

### 6.2 Foreign threads

A thread made by C (the Core Audio IO thread) enters Rae code with none of
Rae's per-thread runtime state. Its first use must not allocate, so a shim call
`attachThread()` runs on that thread when the device starts (before real-time
work) and sets up what Rae needs. The rule for the compiler: a `cExport`
function used before `attachThread` is a run-time error in debug builds.

**Landed (2026-11-11):** the runtime call is `rae_thread_attach()`
(`rae_runtime.h`): it installs the crash handler's alternate stack and
registers the thread's memory-statistics counters, the two things a Rae
thread gets when it starts and a C thread would otherwise get lazily, the
second by allocating on the first Rae call. It is safe to call twice. The
run-time error for a missing attach is not built: an unattached thread
still works (it allocates on its first counted allocation), which
RAE_REALTIME_CHECK reports if that happens in real-time code.

### 6.3 Containers that cannot grow

- `Array(T, cap: N)` exists for small fixed state (filter coefficients, a voice
  table).
- A **bounded list** whose capacity is set once on the main thread and whose
  `tryAdd` answers `false` when full instead of growing: a `lib/collections`
  type, not a language feature (`BoundedList(T)`). The analysis treats its
  `tryAdd` as non-allocating because it is marked `realtime` in `lib/`.

### 6.4 Atomics: `Atomic(T)` (needed)

The rings, the "plan swapped" flag and the meters need acquire / release
atomics. `parallel-stages-design.md` §5 already proposes `Atomic(Int)` and
`Atomic(Int32)` as library types over C11 `stdatomic`. Needed here:
`load`, `store`, `exchange`, `compareExchange`, `fetchAdd`, with acquire on
load and release on store by default and a `relaxed` form for counters only.
The orders stay few on purpose; sequential consistency is not offered as a
default because it hides cost. All atomic operations are `realtime`.

**Landed (2026-11-11):** `lib/Atomic.rae`: `Atomic(T)` for `Int`, `Int32` and
`Bool` with `load`, `loadRelaxed`, `store`, `storeRelaxed`, `exchange`,
`compareExchange` (answers whether it wrote), `fetchAdd`, `fetchAddRelaxed`,
`freeAtomic`. The value is one cell of a `Buffer(Int)` that every copy shares
(so `spawn` shares it and the leak check sees a forgotten free); the shims are
`static inline` one-instruction functions in `compiler/runtime/runtime_atomics.h`
over the `__atomic` builtins. `SpscRing` uses the same shims on its two
counters. Fixture 1395.

### 6.5 Signaling a waiting thread

The audio thread wakes the streaming thread and the render thread of stage A
(`audio-design.md` §4.2) by posting a semaphore, which is real-time-safe (it
does not wait). A `Signal` type in `lib/` (post: `realtime`; wait: `blocking`)
over one-call shims (`dispatch_semaphore` / mach semaphore on Apple, futex on
Linux, `WaitOnAddress` on Windows).

**Landed (2026-11-11):** `lib/Signal.rae` (`createSignal`, `post`, `wait`,
`tryWait`, `freeSignal`) over `compiler/runtime/runtime_signal.c`:
`dispatch_semaphore` on Apple, a POSIX `sem_t` on Linux and Android (glibc and
bionic build it on a futex, so it is the futex without our own wait loop in
C), and a plain counter in a browser build without threads. There is no
Windows branch: the runtime has no Windows target yet, and `WaitOnAddress`
comes with it. `wait` is a `blocking` extern, so a lightweight task that waits
gives its worker back meanwhile; a dedicated real-time thread (§6.6) must not
be such a task. Only the macOS branch has run so far (fixtures 1395, 1396, the
TSan gate); the POSIX and browser branches are compiled the first time a Linux
or browser build includes them.

### 6.6 Real-time threads and teams

- **A dedicated thread with real-time priority.** `spawn` of a function that
  waits on a `Signal` already runs on its own OS thread (the may-wait rule keeps
  waiting tasks off the pool). A shim then raises its priority
  (`thread_policy_set` time-constraint on Apple, MMCSS on Windows, rtkit on
  Linux) and joins the device's audio workgroup on Apple. No language change,
  but it relies on a rule of the scheduler that is not visible in the code. An
  explicit form (§9 R6) would be clearer later.
- **Parallel work inside one block** (the DAW's graph on several cores): the
  worker pool behind `parallelLoop` is not real-time, and its threads are shared
  with everything else. Needed: a separate **real-time team** of threads, joined
  to the workgroup, that a `realtime` function can fan work out to and join
  within the same block, spinning briefly, then waiting on a `Signal`.
  `parallel-stages-design.md`'s team construct (Option A) is the natural base;
  this is the last feature needed and the largest.

### 6.7 `Channel(T)` stays off the real-time thread

`Channel` is a mutex-guarded MPSC queue; that is right for ordinary workers. The
real-time crossings use `SpscRing(T)` (§4.1). No change to `Channel`.

---

## 7. Worst-case time, measured

Real-time rules forbid the slow operations; they cannot make a function fast
enough. The runtime records, per real-time root:

- the CPU time of every call, as cycles at entry and exit (one register read
  each), into a fixed histogram (no allocation);
- the maximum, the 99.9th percentile, and the share of the period used.

`RAE_REALTIME_REPORT=1` prints it at exit; the headless audio tests fail when a
scene's worst block uses more than a set share (for example 50 %) of its period
on the test machine. The devtools page can show the live histogram. Video's
frame path uses the same recorder against a 16.7 ms period.

**Landed (2026-11-11), differing in two points:** the recorder is a Rae value,
`RealtimeRecorder` in `lib/Realtime.rae`, owned by the loop that runs the
real-time code (no runtime table of recorders, no global), and it reads
`nowNs()` (the monotonic clock; on Apple and Linux a user-space read) rather
than CPU cycles, so a preempted call counts its full wall time, which is what a
deadline sees. `callStarted` / `callEnded` time a call, `recordCall` adds a
duration measured elsewhere, and none of them allocates. The histogram has
four steps per doubling (248 steps up to 2^63 ns), so `percentileNanoseconds`
answers the upper edge of a step, within 25 %. `report()` prints the calls, the
mean, p99.9, the maximum, the share of the period the slowest call used and the
non-empty steps when `RAE_REALTIME_REPORT=1`; the program calls it when the loop
ends. Failing a test on the period share is left to the audio tests that set
the period. Fixture 1400.

---

## 8. Order of work

The queue holds these as `[realtime 1/6]` to `[realtime 6/6]`, between the audio
steps that need them.

1. **`Atomic(T)`** (§6.4) and **`Signal`** (§6.5), with `SpscRing(T)` in `lib/`.
2. **Runtime flag** (§5.3) and the **time recorder** (§7): no language change;
   before `audio 1/7`.
3. **`realtime` and `noAlloc` keywords and analysis** (§3): reuse `may_wait.c`'s graph with an
   effect set instead of one flag; `--report-realtime`; the errors of §3.5;
   the plain-value rule for `SpscRing(T)` (§4.1).
4. **clang attributes** in the generated C and `-Wfunction-effects` in the test
   build (§5.1); **the RTSan gate** (§5.2) as a pre-suite case like TSan, when the
   Homebrew LLVM is present.
5. **`cExport`** and foreign threads (§6.1, §6.2): unlocks rendering inside the
   device callback (audio stage B), the VideoToolbox and CoreMIDI callbacks, and
   Box3D's scheduler.
6. `restrict` emission (§4.6); `mainThread` (§4.3); the real-time team (§6.6).

---

## 9. Decisions needed

- **R1 — the keyword `realtime`** on functions and externs, in the modifier
  position (§3.1). **Decided 2026-11-11: yes.**
- **R2 — `noAlloc`** (or the spelled-out `noAllocation`) as the second, weaker
  keyword, or not yet (§3.1). **Decided 2026-11-11: `noAlloc`, built together
  with `realtime`.**
- **R3 — where the marks go. Decided 2026-11-11:** the entry function carries
  `realtime`; everything it reaches is checked without a mark. A helper MAY
  carry the mark too, to tell a reader (and a later caller) that it is meant to
  be real-time; a marked helper is checked as its own root, so the mark can
  never be wrong. `lib/` and package functions that promise to stay real-time
  for their callers carry it always (§3.3).
- **R4 — calling Rae from C. Decided 2026-11-11:** the modifier
  `cExport("cName")` (§6.1).
- **R5 — `mainThread`** for externs, later (§4.3).
- **R6 — an explicit way to start a dedicated thread** instead of relying on
  the may-wait rule (§6.6).
- **R8 — exclusive access for `mod` arguments** (§4.6): refuse a call that
  passes one place, or a place and a part of it, as `mod` together with
  another `mod` or `view` argument. It fixes a foot gun on its own
  (`stress/13_modAndViewOfOnePlace`) and is what makes `restrict` sound. A
  language change: it may refuse existing code (lib/ and examples to be
  measured first).
- **R9 — the real-time team's base** (§6.6): `parallel-stages-design.md`
  chose Option C for `parallelLoop` and did not recommend Option A (unchecked
  atomics and barriers for every program). A team for one `realtime` function
  may still want A's shape (a leader publishing stages, helpers spinning,
  then waiting on a `Signal`) inside a library type rather than a language
  construct. To decide before building it.
- **R7 — the RTSan gate's compiler. Decided 2026-11-11:** Homebrew LLVM
  (`brew install llvm`), a test dependency on macOS installed by `make setup`,
  like `naga`; the gate SKIPs with a message when it is missing.

---

## Sources

- Clang function effect analysis (`nonblocking`, `nonallocating`):
  https://llvm.googlesource.com/llvm-project/+/5a6cc509215b62e94de3b798ea26944a375ce6cb/clang/docs/FunctionEffectAnalysis.rst
- RealtimeSanitizer and the `nonblocking` attribute:
  https://lists.llvm.org/pipermail/llvm-commits/Week-of-Mon-20240729/1416996.html
- Both tried on this Mac on 2026-11-11 (Apple clang 21.0, Homebrew clang 22.1).
