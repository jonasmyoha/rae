# lib/wasm: run WASM modules inside a native program

`lib/wasm` (`wasm/Wasm`, `wasm/WasiMinimal`) lets a native Rae program (macOS,
iOS, Linux) run a downloaded WASM module without a JIT: SUMU's hosted apps
(its docs/HOSTED_APPS_PLAN.md §2). The engine is WAMR's fast interpreter
(Apache-2.0 with the LLVM exception); the API is small enough that a WASM
interpreter written in Rae (docs/raepack-v2-and-packages.md §11) can replace it
behind the same functions.

```rae
var module: WasmModule = wasmLoad(bytes: bytes)                 # nothing runs
var instance: WasmInstance = wasmInstantiate(module: module, exposed: wasiMinimalCapabilities(), granted: { "log", "clock" }, limits: limits)
let result: WasmCallResult = wasmCall(MyHost, host: host, instance: instance, name: "add", args: { wasmI64(value: 40), wasmI64(value: 2) }, timeoutMs: 1000)
wasmDestroy(instance: instance)
wasmUnload(module: module)
```

- **Capabilities, deny by default** (docs/wasm-interfaces.md §4). A
  `WasmCapability { name, imports }` is a named set of imports
  (`"module.name"`) that the host defines. `wasmLoad` lists a module's imports
  (`module.imports`: kind, module and field name, parameter and result kinds)
  and its requests (the `rae.interface` section's `requests`; a module
  without the section, not built by Rae, requests every exposed capability).
  `wasmInstantiate(module, exposed, granted, limits)` checks, before anything
  runs:
  - every request is exposed, else `requestNotExposed`;
  - every grant was requested, else `grantNotRequested` (a request never
    grants: an edited section gains nothing the host did not grant);
  - every import is a function in a requested capability, else
    `importRefused`, naming them all.

  `runtime` is granted whenever it is exposed. The imports of a capability
  that was requested but not granted still link, so the module loads, and
  `wasmCall` answers a call to one with the trap
  `capability <name> is not granted`. The host's `wasmHostCall` never sees
  that call.
- **Host functions are Rae.** `wasmCall(H, host, ...)` answers each import the
  module calls by calling `wasmHostCall(host: mod H, instance: mod WasmInstance,
  call: view WasmHostCall) ret WasmHostAnswer`, which the program writes for its
  own host type `H` (the way lib/net/ecs's frame calls a router's `route`).
  The answer is the results (`wasmAnswer`), nothing (`wasmAnswerNothing`) or a
  trap (`wasmTrap(message)`). A host function may read and write the
  instance's memory.
- **Values**: `WasmValue { kind, integer, real }` (`wasmI32`, `wasmI64`,
  `wasmF32`, `wasmF64`); an argument of another kind is converted to the one
  the export takes.
- **Limits** (`WasmLimits`): the WASM stack, a host-managed heap (0 for a
  module that brings its own allocator) and the linear memory's cap in 64 KiB
  pages: a `memory.grow` past it fails (-1) instead of taking memory.
- **Time**: a call running past `timeoutMs` is stopped and answers
  `timedOut`; `wasmStop(instanceHandle)` stops a running call from any thread
  (a watchdog), answering `trap` ("terminated by user").
- **Errors are values** (`WasmError { kind, message }`): `load`,
  `importRefused`, `instantiate`, `noSuchFunction`, `wrongArguments`, `busy`,
  `trap` (with WAMR's message, e.g. "integer divide by zero"), `timedOut`,
  `memory`, `unsupported` (no engine on this platform: the web, Windows). A
  trap leaves the instance usable.
- **Memory**: `wasmMemorySize`, `wasmMemoryRead` (none outside it),
  `wasmMemoryWrite` (false outside it), `wasmMemoryReadInt` /
  `wasmMemoryWriteInt` (little-endian 4 or 8 bytes).
- **WASI**: `wasm/WasiMinimal` answers the WASI functions a module built by
  `rae build --target wasm` imports (docs/wasm-modules.md) with nothing behind
  them: stdout / stderr text collected in a `WasiOutput` for the host's log,
  the real clock and random bytes, no environment, no arguments, EBADF for
  every file, `proc_exit` a trap. `wasiMinimalCapabilities()` is the
  standard set over it: `runtime` (the calls with no authority), `log`
  (`fd_write`), `clock` (`clock_time_get`, `clock_res_get`) and `random`
  (`random_get`). Expose it, grant the names a module may use, and call
  `wasiMinimalAnswer` from `wasmHostCall`. (`wasm/WasmMemory` reads and
  writes the little-endian integers WASI's pointer arguments need.)

## Typed calls: `wasm/WasmInterface`

A module built by `rae build --target wasm` describes its exports in its
`rae.interface` section (docs/wasm-modules.md). A host built from the same Rae
types (a package both import, as `tests/wasmFrames` is for the check below)
checks an export once and then calls it with encoded values
(docs/wasm-interfaces.md §3, §5):

```rae
if let mismatch: WasmError = wasmCheckExport(
  module: module
  name: "summarize"
  params: { binarySchemaText(AppFrameIn) }
  result: binarySchemaText(AppFrameOut)
) {
  log(mismatch.message)      # interfaceMismatch: both descriptions, which part differs
}
let reply: WasmBytesResult = wasmCallEncoded(Host, host: host, instance: instance, name: "summarize", input: frame.toBinary(), timeoutMs: 200)
if let out: AppFrameOut = AppFrameOut.fromBinary(bytes: reply.bytes) { ... }
```

- `wasmLoad` reads the section from the module's bytes (in Rae: the sections
  are an id byte, a LEB128 size and the contents; this WAMR build answers no
  custom sections) into `module.interfaceText`. `wasmModuleInterface(module)`
  parses it into a `WasmInterface` (version, each export's params and result
  as name / description / hash, the requests), none when there is none.
- `wasmCheckExport` answers none when the export takes exactly those types
  (`""` as `result` for none). Otherwise it answers an `interfaceMismatch`
  that names the export, the parameter and both descriptions. A module
  without the section, or without that export, is a mismatch too.
- `wasmCallEncoded(H, host, instance, name, input, timeoutMs)` allocates
  memory in the module (`raeModuleAlloc`), writes the bytes, calls the export
  with their address and length, reads the result and frees both
  (`raeModuleFree`). A failed step answers its error and empty bytes. A value
  the module cannot decode traps (docs/wasm-modules.md) and comes back as a
  `trap` error.

### Typed host imports

A module's typed host import (`imports: "game.describe"` in its pack,
docs/wasm-modules.md) calls the host as `rae.describe`. The host exposes it as
a capability (`WasmCapability { name: "game", imports: { "rae.describe" } }`)
and answers it in `wasmHostCall` with typed helpers:

```rae
if call.module is "rae" and call.name is "describe" {
  if let command: Command = wasmCallValue(Command, instance: instance, call: call, at: 0) {
    ret wasmAnswerValue(Command, instance: instance, call: call, value: describeCommand(command: command))
  }
  ret wasmTrap(message: "describe: the command does not decode")
}
```

An argument's position `at` counts the raw arguments: one per scalar, two
(the address and length) per String or value. `wasmAnswerValue` writes the
encoded answer into the module's buffer. When it does not fit, it keeps the
answer on the instance (`pendingAnswer`), and lib/wasm answers the module's
`rae.raeTakeAnswer` with it, never the host.

## How it runs

Rae cannot hand C a function to call, so a host function cannot be a C
callback. Instead each instance has a worker thread that runs its calls, and
every granted import is one C trampoline: it posts the host call (which
import, its arguments) and waits; the Rae caller, waiting in `wasmCall`,
answers it, and the call goes on. The same wait is the watchdog. A module is
loaded once to list its imports; after the grant check the trampoline is
registered for the granted ones and the module is loaded again, so they link.

The glue is `tools/wamr/rae_wasm_glue.c` (prototypes in
`compiler/runtime/runtime_wasm_embed.h`, the module's `cheader`); platform
reason: WAMR is a C library used as a library. `tools/wamr/pin` pins
WAMR-2.4.5; `tools/wamr/build.sh` clones it into `~/.cache/rae/wamr` and builds
`librae_wamr.a` (fast interpreter, no AOT, no JIT, no libc of its own, the
thread manager for terminate) for this machine, iPhone and the iOS simulator.
`make setup` builds it. The compiler links it only into a program whose C
includes the glue header; `tools/ios/gen-ios.sh` links the iOS build into an
app that does (the device's or the simulator's library by SDK).

## Checks

- Case 1302 runs an 823-byte module that imports one function per standard
  capability and requests only `log` (`ModuleSource.txt`, its bytes inline):
  the imports no request covers are refused by name. With its section edited
  to request all three and only `log` granted, `say` writes, while the clock
  and random calls trap in lib/wasm unseen by the host. Granting `clock`
  makes the clock answer. A grant that was not requested and a request this
  host does not expose are refused. It is on the TSan gate.
- Case 1292 runs a 391-byte module (`ModuleSource.txt`, compiled with wasi-sdk,
  its bytes inline as hex): an import no exposed capability covers refused by name, `i64` /
  `i32` / `f64` / `f32` values, a Rae host function, text through memory, a trap
  and the instance after it, the memory cap, a time limit and a watchdog
  thread. It is on the TSan gate.
- `compiler/tools/wasm-module-check.sh` also runs the module
  `rae build --target wasm` makes in a Rae host (`compiler/tests/wasmEmbedHost`):
  refused on a host that exposes nothing, then `_initialize`, `add` and a Unicode `greet`
  through the module ABI with the minimal WASI. Built from the same frame
  types (`compiler/tests/wasmFrames`), the host checks `summarize` and runs
  1000 `AppFrameIn -> AppFrameOut` frames in bounded memory. It answers the
  module's typed import `rae.describe` (a Command in and out, and once with a
  3000-byte answer that takes the fetch path). With `game` not granted, the
  call traps and the host's function never runs. A module built
  from an `AppFrameIn` with one field more is refused by `wasmCheckExport`
  before any call.
- An iPhone build of case 1292's program (gen-ios, xcodebuild for iphoneos
  without signing) links WAMR's iOS library.
