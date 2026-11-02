# Typed native-to-WASM interfaces and capabilities

The design for QUEUE_BACKLOG #129 (typed interfaces with generated
marshalling) and #130 (capabilities, deny by default), and the interface
part of #123 (docs/raepack-v2-and-packages.md §9, §15 Q1). Decided
2026-11-02. The first user is SUMU's hosted apps: per frame the host passes
one `AppFrameIn` value to the module and gets one `AppFrameOut` value back, and
the module imports only log, clock and random (SUMU's
docs/HOSTED_APPS_PLAN.md §4, §9).

What exists already: `rae build --target wasm` builds a module whose exports
take and return `Int`, `Float`, `Float64`, `Bool` and `String`
(docs/wasm-modules.md), and `lib/wasm` runs such a module in a native program
and refuses every import that is not granted, by name
(docs/wasm-embedding.md). This design adds structs, lists and the other value
types across the boundary, a version check, and capabilities in place of
import names.

## 1. Decision: the interface is ordinary Rae, not an IDL

There is no `interface { ... }` block and no new syntax. **An interface is an
ordinary Rae module that the host and the module both import**: its types are
the values that cross, and the module's exported functions (the pack's
`exports:` list, as today) are the entry points. SUMU's SDK already is such a
module: `AppFrameIn` and `AppFrameOut` are Rae types that the built-in apps
and the Shell share.

Why this and not a dedicated IDL:

- **One language on both sides.** An IDL would describe Rae types in a second
  notation and need its own parser, formatter and docs. A shared module is the
  types themselves; the compiler already knows everything about them.
- **No new syntax** (AGENTS.md: no new syntax without need). The only new
  surface is two pack keys (`exports:` already exists, `requests:` is new,
  §4) and functions in `lib/wasm`.
- **The marshalling is derived the way `toJson` is.** The compiler already
  derives `value.toJson()` / `T.fromJson(json:)` for every type
  (docs/compile-time-reflection.md). The binary form is the same derivation
  with a different output (§2). The compiler generates the code on both
  sides: the module's export wrapper decodes and encodes, the host calls the
  derived functions of the same types.
- **What an IDL would add is not needed yet.** Several language bindings, or
  a module written in another language, would need a neutral description. The
  `rae.interface` section (§3) carries a readable description of each type for
  diagnostics. If a non-Rae module ever has to implement a Rae interface, that
  section is where its description comes from.

Rejected: function signatures with annotations (`@export`, `@capability`):
AGENTS.md bans `@` attributes, and the pack already says which functions are
exported.

## 2. The binary encoding (derived per type)

Every type that can cross gets two derived functions, the shape of the JSON
pair:

    let bytes: List(UInt8) = frame.toBinary()
    let decoded: opt AppFrameIn = AppFrameIn.fromBinary(bytes: bytes)

and a compile-time value `binarySchemaHash(AppFrameIn)` (an `Int`, §3).

The format is little-endian, without padding, without a header, and fields are
written in declaration order:

| Rae | bytes |
|---|---|
| `Int`, `Int64`, `UInt64` | 8 |
| `Int32`, `UInt32` | 4 |
| `UInt8` | 1 |
| `Float` / `Float64` | 4 (f32) / 8 (f64) |
| `Bool` | 1: 0 or 1; any other value is a decode error |
| enum | 4: the member's index (the hash covers the member names, so the index is safe) |
| `String` | UInt32 byte count, then the UTF-8 bytes (invalid UTF-8 is a decode error) |
| `List(T)` | UInt32 element count, then each element |
| `opt T` | 1: 0 = none, 1 = a value, which follows |
| a struct | each field, in declaration order |

- **What cannot cross is a compile error naming the field:** `Ptr`, `Buffer`,
  `Task`, `Channel`, a `view`/`mod` field, a GPU or other runtime handle type.
  Nothing crosses by reference; a handle is an `Int` the host gives meaning to
  (the way gpu2d's handles are), and large data is a `List(UInt8)`.
- **Decoding never trusts the bytes.** A downloaded module is untrusted, and so
  is what it returns. `fromBinary` is bounds-checked everywhere and returns
  `none` for a short, long or malformed input; a count is checked against the
  bytes that are left before anything is allocated, so a bad count cannot ask
  for gigabytes. It never crashes and never reads outside the list.
- The encoding is also Rae's general binary serialization: it is the
  "Binary Gen" item of QUEUE_NETWORKING.md Step 1 (compact packets), so it is
  not a WASM-only feature.

## 3. Versioning: a structural hash, checked once at load

A host and a downloaded module can be built from different versions of the
shared types. So each type has a **canonical description** and a 64-bit hash
of it:

    AppFrameIn{commands:List(AppCommand{name:String,payload:String}),resetScene:Bool,...}

The description lists the type name, each field's name and type in order,
enum members in order, and nested types inline, recursively.
`binarySchemaHash(T)` is the FNV-1a hash of that text. Any change to a field
(added, removed, renamed, reordered, or a different type) changes the hash.

The module build writes a custom section **`rae.interface`** (JSON text,
version 1) into the module, and the host can read it without running anything:

    { "version": 1,
      "exports": [ { "name": "sumuUpdate",
                     "params": [ { "name": "input", "type": "AppFrameIn{...}", "hash": "9f3c..." } ],
                     "result": { "type": "AppFrameOut{...}", "hash": "41aa..." } } ],
      "requests": ["log", "clock", "random"] }

- **The host checks each export it will call, once, before the first call:**
  `wasmCheckExport(module, name, params, result)` compares the hashes of the
  host's own types with the section's. A mismatch is the error
  `interfaceMismatch`, and its message contains both descriptions, so it is
  clear which field differs. A frame never pays for the check.
- **Version 1 is strict: equal or refused.** Tolerant changes (a field added
  at the end, read as its default) are a later extension. SUMU already
  versions its SDK (`sdkVersion`), and a strict check can never misread a
  value.
- A scalar or `String` export keeps today's ABI (docs/wasm-modules.md) and
  its section entry names the scalar type.

## 4. Capabilities: deny by default (#130)

These are the three parts of docs/raepack-v2-and-packages.md §8, made concrete
for `lib/wasm`.

- **A capability is a named set of imports, defined by the host, in Rae:**
  `WasmCapability { name: "clock", imports: { "wasi_snapshot_preview1.clock_time_get", ... } }`.
  `lib/wasm/WasiMinimal` defines the standard ones over the minimal WASI it
  already answers:
  - `runtime`: the WASI calls the Rae runtime makes that carry no authority.
    The environment and the arguments are empty, `sched_yield` does nothing,
    every file call answers EBADF, and `proc_exit` traps. It is always
    granted.
  - `log`: `fd_write` to stdout and stderr, collected for the host's log.
  - `clock`: `clock_time_get`, `clock_res_get`.
  - `random`: `random_get`.
- **The module requests:** the pack's wasm target says
  `requests: "log, clock, random"`. The build writes the list into
  `rae.interface`. **The build fails** if the module imports anything that its
  requests (plus `runtime`) do not cover, and names the import. So a module
  cannot quietly depend on more than it asks for.
- **The host grants:** `wasmInstantiate` takes the host's exposed capabilities
  and the names it grants to this module, and validates
  `granted ⊆ requested ⊆ exposed`:
  - a grant the module did not request is the error `grantNotRequested`;
  - a request the host does not expose is the error `requestNotExposed`
    (the module cannot run on this host);
  - an import that is in no requested capability is refused at load by name,
    as today (`importRefused`).
- **A request that is not granted is denied when it is called:** the import
  links, so the module loads. `lib/wasm` itself answers a call to it with a
  trap (`capability clock is not granted`), before the host's `wasmHostCall`
  is reached. The check is in `lib/wasm`, so the host code is not the only
  thing that keeps it closed.
- **A request never grants.** The request comes from the module's bytes and
  the grant comes only from host code. A module that edits its own
  `rae.interface` to request more gains nothing that the host did not grant.

## 5. One frame, end to end

The module, built with `exports: "sumuUpdate"` and
`requests: "log, clock, random"`:

    func sumuUpdate(input: view AppFrameIn) ret AppFrameOut { ... }

The generated export wrapper takes `(address: i32, length: i32)`. It decodes
`AppFrameIn` from the module's memory (a decode failure traps with a message),
calls the function, encodes `AppFrameOut` into newly allocated memory, and
returns the packed `(address << 32) | length` i64, as a `String` result does
today. A `mod` parameter is still refused.

The host, once per module:

    let check: opt WasmError = wasmCheckExport(
      module: module
      name: "sumuUpdate"
      params: { binarySchemaHash(AppFrameIn) }
      result: binarySchemaHash(AppFrameOut)
    )

and per frame:

    let reply: WasmBytesResult = wasmCallEncoded(Shell, host: shell, instance: instance, name: "sumuUpdate", input: frameIn.toBinary(), timeoutMs: 200)
    let frameOut: opt AppFrameOut = AppFrameOut.fromBinary(bytes: reply.bytes)

`wasmCallEncoded` writes the bytes with `raeModuleAlloc`, calls the export,
reads the result and frees both. It uses only one type parameter (the host
type), because functions with two type parameters do not compile today.

## 6. Later: typed host imports (module to host)

SUMU needs no host function of its own: log, clock and random are WASI. When
a host does need one (a game's `spawnEntity`), the module declares it with
the existing `extern` spelling. In a module build, an `extern` function that
the pack lists under `imports:` becomes a WASM import of the module `rae`. It
is marshalled the same way as an export (scalars directly, everything else as
encoded bytes), and its capability is the one the pack names. The host answers
it in `wasmHostCall` with typed helpers. This is the last of the queued
steps, and nothing waits for it.

## 7. The steps (QUEUE.md)

1. The derived binary encoding: `toBinary`, `fromBinary`, `binarySchemaHash`
   and the crossability errors (§2, §3).
2. Module exports with struct, list and `opt` values, and the `rae.interface`
   section (§3, §5).
3. Host side: `wasmCheckExport`, `wasmCallEncoded`, and a frame-shaped round
   trip (§5).
4. Capabilities: `WasmCapability`, the standard set, `requests:`, the
   validated grant and the denial trap (§4).
5. Later: typed host imports (§6).
