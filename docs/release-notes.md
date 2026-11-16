# Release notes

A release is a git tag (docs/versioning-and-toolchain.md §1). Before 1.0 the
MINOR number is a named milestone; PATCH counts the work in
between. Each milestone gets a section here, newest first.

## 0.4.0 (not released yet): module folders

Named on 2026-11-16. Being built in the 0.3 line from 0.3.173; the task
[release: modules, Rae 0.4.0, then push] completes this section and tags it.
docs/module-model-options.md is the design.

**Modules**
- A module is a PascalCase file or a PascalCase folder (0.3.174): `import
  Gpu2d` finds lib/Gpu2d.rae or the folder lib/Gpu2d/, whose files are one
  module. A module that reaches the 1,000-line cap becomes a folder with no
  change to the code that uses it. Refused: a file and a folder of one name, an
  import of a file inside a module folder, the same function in two files of
  one module, and a PascalCase folder with Rae files in a program's own tree.
- Imports and qualifiers are spelled as on disk, with the same upper and lower
  case, on every system (0.3.173): `import vec3` and `math.sqrt` are refused
  with the right spelling. They used to work on macOS only.
- lib/Gpu2d is a module folder (0.3.175), and its image functions are back in
  it: `Gpu2d.loadImageKey`, `Gpu2d.drawImageKey` and the rest.
  **`Gpu2dImages.` existed only in 0.3.172**; code written against it goes back
  to `Gpu2d.` and drops `import Gpu2dImages`.

## 0.2.0, 2026-10-11: physics and servers

The first milestone after 0.1.0 (2026-09-12). The 0.1 line ended at 0.1.268
(tag `v0.1.268`). A project whose `.raepack` requires `rae: { version: "0.1" }`
(or any `0.1.x`) must change it to `"0.2"`.

**Physics**
- Box3D's C library is Rae's physics engine, through generated bindings
  (`lib/box3d`) and an ECS layer (`lib/box3d/ecs`), with examples for the
  playground and a driven vehicle. The Rae port of Box3D (`lib/physics`) stays
  as a frozen reference and the portable fallback
  (docs/physics-two-implementations.md).
- A physics scaling benchmark for both tracks (`benchmarks/physics_scaling`).

**Networking and servers**
- `lib/net`: TCP sockets, a readiness poller (kqueue on Apple and BSD, epoll
  on Linux) and byte buffers, all Rae over one-call system shims.
- `lib/http`: an HTTP/1.1 parser and response encoder in plain Rae.
- `lib/webSocket`: the upgrade, frame decoder and encoder in plain Rae.
- `lib/net/ecs`: HTTP and WebSocket servers as ECS systems.
- Server benchmarks against tuned Rust, Node and Bun (`benchmarks/servers`,
  and three tabs in the devtools Benchmarks view): Rae is in the same range,
  ahead on single-core HTTP throughput.

**Tasks and concurrency**
- Spawned tasks run M:N on the worker pool, scheduled in Rae; `get()`,
  `sleep` and socket waits suspend a task instead of holding a thread.
- `blocking` externs run a long C call on its own thread.
- A spawn inside a `taskScope` lends a view instead of copying it.
- In the browser, waiting tasks run between `mainLoop` frames.

**Language**
- `const` / `let` / `var`, with a `let` frozen as a whole; `if var`.
- Compile-time `when` and the `Target` prelude for per-platform code;
  `rae build --check-targets`.
- `match let`, `equals`, `enumName` / `enumMembers` / `enumCount`, `toString`
  for lists, optionals and structs, one-UTF-8-character `Char` literals.
- SIMD: `Float4` masked tails and `Float8` (AVX2 on x86-64).
- Many fixes in generics, ownership and drops, and sema checks that reject
  wrong members and qualifiers.

**Toolchain**
- The Live / VM target is gone: the C backend is the one target.
- `rae format` and the build's format preflight own the layout.
- Projects declare the Rae they need; `rae toolchain`; dependencies with
  `rae add` / `fetch` / `update` / `tree`.
- The compiler builds with -O2 and LTO, and emitting is linear.
- Browser builds of WebGPU apps, with WASM SIMD and a frame-callback loop.
- The generated C compiles without warnings; gates for TSan, ASan, banned
  terms and stress cases run before every suite.
- Runtime logic moved from C to Rae (strings, JSON, time, logging); C is kept
  for platform calls, and a faster platform implementation wins where one
  exists (CommonCrypto SHA-1).
