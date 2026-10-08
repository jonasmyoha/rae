# Per-platform code: `when` (design)

**Status:** design, 2026-10-08. Proposed; it is new syntax, so the keyword and
the rules below need the maintainer's approval before implementation.

## 1. Why

Rae's rule (AGENTS.md, "the fastest implementation wins") is: write the Rae
version as the reference and the portable fallback, and where a platform has
a clearly faster native version, use that on that platform. The first case
was SHA-1:

| SHA-1 of 16 MB, M1 Max | time |
|---|---|
| Rae (portable) | 100 ms |
| plain C, same algorithm | 77 ms |
| macOS CommonCrypto (the CPU's SHA instructions) | 7 ms |

Today the platform choice is made in C. `runtime_crypto_platform.c` has an
`#if defined(__APPLE__)` body that calls CommonCrypto and a stub elsewhere that
answers -1, and `lib/crypto/Sha1.rae` calls the shim on every platform and
falls back to Rae on -1. The kqueue poller, FileNotify and Float4's
NEON/SSE2/wasm/scalar split (`runtime_float4.h`, `--float4-scalar`) do the
same. That works, but:

- the choice is invisible in Rae: a reader of `Sha1.rae` cannot see which
  platforms take which path;
- every platform needs a C stub, even where the Rae code is the whole story;
- the Rae side always pays a call that may answer "unsupported";
- Rae code itself cannot differ per platform (a Linux-only `epoll` path in
  Rae, a wasm-only frame loop) without the same C detour.

Rae needs one construct for "this code exists only for this target", in Rae.

## 2. What other languages do

| language | construct | notes |
|---|---|---|
| C / C++ | `#if defined(__APPLE__)` | the preprocessor: textual, untyped, the dead branch is never parsed |
| D | `version (OSX) { } else version (linux) { }`, plus `static if` | a keyword, statement and declaration level; the dead branch is parsed but not checked, so it rots unless CI builds every target |
| Zig | `if (builtin.os.tag == .macos)` on a comptime value | an ordinary `if` on a compile-time constant; the dead branch is not analysed |
| Nim | `when defined(macosx):` | a keyword `when`, statement and declaration level |
| Swift | `#if os(macOS)` | a sigil |
| Rust | `#[cfg(target_os = "macos")]` | an attribute: the `@`/`#[...]` sigil Rae refuses (AGENTS.md) |
| Go | `sha1_darwin.go`, `//go:build darwin` | per-FILE selection by name suffix or a build-tag comment |

What Rae takes:
- from D and Nim, a bare keyword at statement and declaration level;
- from Zig, a condition that is an ordinary compile-time expression over
  ordinary constants;
- from nobody, a check that the branches not taken still compile (§4.4),
  because D's untested `version` blocks are the known failure.

## 3. The proposal

### 3.1 The target, as ordinary constants

The compiler provides a prelude module `Target` whose values are fixed per
build:

```rae
enum Os { macos, ios, linux, windows, android, web }
enum Arch { arm64, x64, wasm32 }

# in module Target, set by the compiler for the build being made
const os: Os = ...
const arch: Arch = ...
const simd: Bool = ...     # false under --float4-scalar
```

They are plain `const`s, so they can also be used in ordinary code
(`log("built for {Target.os}")`).

### 3.2 `when`

`when` takes a compile-time `Bool` (a `const` expression: the `Target` values,
other `const`s, `is`, `and`, `or`, `not`). Its blocks look like `if`'s:

```rae
when Target.os is Os.macos or Target.os is Os.ios {
  ...
} else when Target.os is Os.linux {
  ...
} else {
  ...
}
```

It works at two levels.

**Declaration level.** The declarations inside exist only for the targets
that select them:

```rae
when Target.os is Os.macos or Target.os is Os.ios {
  # CommonCrypto: one call, the CPU's SHA instructions
  func platformSha1Digest(bytes: Buffer(UInt8), offset: Int, count: Int, digest: Buffer(UInt8)) unsafe extern(
    "rae_ext_Sha1_commonCrypto"
  ) ret Int
}
```

**Statement level.** Only the selected block is compiled:

```rae
func sha1Range(bytes: view List(UInt8), start: view Int, count: view Int) ret List(UInt8) {
  when Target.os is Os.macos or Target.os is Os.ios {
    ret commonCryptoSha1(bytes: bytes, start: start, count: count)
  } else {
    ret portableSha1Range(bytes: bytes, start: start, count: count)
  }
}
```

With this, the C shim loses its `#else` stub: the extern is declared only for
Apple, and the other platforms never call it.

### 3.3 Rules

1. **The condition is compile-time.** A `when` whose condition is not a
   `const` expression is an error that names the non-constant part. A runtime
   choice is an `if`.
2. **Every block is parsed and formatted.** `rae format`, the LSP and
   `rae build --report-waits` see all of them. Only the selected one is
   type-checked and emitted.
3. **A declaration in an unselected block does not exist.** A reference to it
   from selected code is an error, the same "unknown function" as any other.
4. **No `when` inside an expression.** It is a statement or a declaration
   group, never a value, so a reader always sees whole blocks.
5. **A file-wide `when` is just one block around the file.** No file-name
   convention (Go's `_darwin.go`) is added, because PascalCase module names
   and imports stay platform-neutral.

### 3.4 Keeping unselected branches honest

D's experience is that `version` blocks for other platforms rot, since nobody
compiles them. Rae's answer is
`rae build --check-targets` (and a pre-suite case that runs it on `lib/`): it
type-checks the program once per target in the `Os` × `Arch` set, without
emitting C. Sema already runs per build, so that is a loop over targets, not
a new analysis. An extern declared for one platform must then also link on
it; the C side keeps its `#if` only around the body that really is
per-platform.

## 4. Alternatives considered

- **`if` on a constant (Zig).** No new keyword, but `if` reads as a runtime
  choice, and both branches would have to type-check on every target (the
  CommonCrypto extern would have to be declared on Linux). Rejected.
- **`version` (D).** It reads as "which version": Rae already has toolchain
  versions (`rae: { version: "0.1" }`), and the clash is confusing. `when` is
  free in Rae and reads as a condition.
- **`static if` (D).** It is two words, and `static` means nothing else in
  Rae.
- **`#if` / attributes.** These are sigils, which AGENTS.md refuses.
- **Per-file selection only (Go).** It is coarse: a two-line platform
  difference would need two whole modules. It can be built on top of `when`
  later if wanted (a file-wide `when`).
- **Leave it in C.** That is what we do today. It hides the choice and
  forces a C stub for every platform without a native path.

## 5. Phasing (once approved)

1. The `Target` module and `when` at statement and declaration level, with
   these fixtures:
   - each branch selected by a different `Target` value (one test fakes the
     target);
   - an error for a non-constant condition;
   - an error for a reference to an unselected declaration;
   - formatting of every branch.
2. Move SHA-1's platform choice into Rae: the extern is declared only for
   Apple, and the `#else` stub is removed.
3. `rae build --check-targets` and its pre-suite case.
4. Migrate the other C `#if` choices that are really policy (Float4's
   lowering choice stays in C, because it is per-instruction).

## 6. Decisions for the maintainer

1. **W1** The keyword: `when` (recommended), `version`, or another bare
   keyword.
2. **W2** Statement level and declaration level both (recommended), or
   declaration level only.
3. **W3** Unselected branches: parsed and formatted but not type-checked,
   with `--check-targets` in the suite (recommended); or type-checked against
   every target on every build (slower, catches rot at once).
