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

### 3.1 The target: exact values, families, capabilities, and what is not compile-time

The hard part is not the keyword but the vocabulary. "BSD", "unix",
"mobile", "desktop" and "web" overlap. macOS is unix and Apple. Android is a
Linux kernel, a mobile OS, and sometimes a laptop (Chromebooks). A web build
runs in desktop Chrome and mobile Safari alike. A single exclusive `Os` enum
cannot say any of that. The answer is four layers, each with one job.

**Layer 1: exact values, one per axis, exclusive.** What the program is
compiled *against*: the system API it links to. A build has exactly one of
each.

```rae
enum Os {
  macos
  ios        # iPhone and iPad (iPadOS is iOS for the API)
  tvos
  watchos
  visionos
  android
  sailfish   # Jolla: a Linux kernel, but its own app model
  linux
  freebsd
  openbsd
  netbsd
  windows
  web        # the browser platform: in a browser the API IS the web, whatever OS is under it
}

enum Arch { arm64, x64, wasm32 }

# module Target, set by the compiler for the build being made
const os: Os = ...
const arch: Arch = ...
const simd: Bool = ...     # false under --float4-scalar
```

An OS gets its own value when its *API* differs, not because of branding:
- Android is not `linux`, since it has bionic, its own app lifecycle and no
  glibc;
- Sailfish is not `linux` either, since apps follow its mobile app model;
- an Ubuntu phone build would just be `linux`.

Adding a value is a toolchain change. A new OS needs a C toolchain and
runtime anyway, so the list stays closed and every `when` can be checked
against it.

**Layer 2: families, as predicates, not enum values.** An umbrella is a
`Bool` computed from the exact value, written in Rae in the `Target` module
itself, so anyone can read exactly what "unix" means:

```rae
const isApple: Bool = os is Os.macos or os is Os.ios or os is Os.tvos or os is Os.watchos or os is Os.visionos
const isBsd: Bool = os is Os.freebsd or os is Os.openbsd or os is Os.netbsd
const isLinuxKernel: Bool = os is Os.linux or os is Os.android or os is Os.sailfish
const isUnix: Bool = isApple or isBsd or isLinuxKernel          # POSIX: fork, file descriptors, signals
const isMobileOs: Bool = os is Os.ios or os is Os.android or os is Os.sailfish
const isDesktopOs: Bool = os is Os.macos or os is Os.windows or os is Os.linux or isBsd
const isNative: Bool = os is not Os.web
```

Families overlap freely. macOS is `isApple`, `isUnix` and `isDesktopOs` at
once, because they are separate `Bool`s rather than places in a tree, so no
hierarchy has to be agreed on. A "mobile OS" family means the mobile *app
model*: the app is suspended in the background, cannot spawn processes, and
lives in a store sandbox. It does not mean a small screen (layer 4). macOS
is deliberately not `isBsd`: it has kqueue, but not OpenBSD's `pledge` or
FreeBSD's `capsicum`, and its kqueue filters differ.

**Layer 3: capabilities, the preferred test.** Most per-platform code
depends on one facility, not on an OS. Test for the facility, named by what
it is, and define it next to the code that uses it, the way autoconf or
browser feature detection do:

```rae
# lib/net/NetSystem.rae
const hasKqueue: Bool = Target.isApple or Target.isBsd
const hasEpoll: Bool = Target.isLinuxKernel

# lib/crypto/Sha1.rae
const hasCommonCrypto: Bool = Target.isApple
```

When FreeBSD gains a facility, or Sailfish loses one, the fix is one line where
the facility is used, and no `when` elsewhere changes. The subtle
differences inside a family stay exact tests, written where they matter:
OpenBSD's kqueue has no `EVFILT_USER`, so lib/net writes
`when hasKqueue and Target.os is not Os.openbsd`. The guideline for review:
- `when Target.os is ...` for a genuinely OS-specific API;
- a family for a shared API (POSIX, the Apple frameworks);
- a capability whenever the code needs one facility.

**Layer 4: what is NOT compile-time, so not `when`.** The device a build
runs on. One iOS binary runs on an iPhone and on an iPad with a keyboard,
and on an Apple-silicon Mac. One Android build runs on a phone and on a
Chromebook with a mouse. One web build runs in desktop Chrome and mobile
Safari. So "mobile", "desktop", "touch", "small screen" and "which browser"
are runtime questions, answered by ordinary functions and `if`:

```rae
if Device.isTouchPrimary() { ... }          # layout and input, at runtime
if Device.browser() is Browser.safari { ... }
```

`Device` is a separate (future) runtime module. UI layout should follow the
window's size and the input devices actually present, not the OS: an iPad
with a trackpad wants hover states, and a touchscreen Linux laptop wants
large targets.

So the combinations in the question are each one line:

| want | write |
|---|---|
| OpenBSD only | `when Target.os is Os.openbsd` |
| any BSD | `when Target.isBsd` |
| unix, including macOS and Linux | `when Target.isUnix` |
| kqueue wherever it exists | `when hasKqueue` (defined in lib/net) |
| any mobile OS | `when Target.isMobileOs` |
| Android, but not other mobile OSes | `when Target.os is Os.android` |
| mobile OS except iOS | `when Target.isMobileOs and Target.os is not Os.ios` |
| a browser build | `when Target.os is Os.web` |
| a browser on a phone | runtime: `if Device.isTouchPrimary()` in a `web` build |
| a desktop-shaped layout, anywhere | runtime: the window size and the input devices |

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
5. **Families and capabilities are ordinary `const Bool`s.** A program can
   define its own (`const hasMetal: Bool = Target.isApple`) and use it in
   `when` like the built-in ones.
6. **A file-wide `when` is just one block around the file.** No file-name
   convention (Go's `_darwin.go`) is added, because PascalCase module names
   and imports stay platform-neutral.

### 3.4 Keeping unselected branches honest

D's experience is that `version` blocks for other platforms rot, since nobody
compiles them. Rae's answer is
`rae build --check-targets` (and a pre-suite case that runs it on `lib/`): it
type-checks the program once per real (`Os`, `Arch`) pair the toolchain supports (`web` only with `wasm32`, for one), without
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
4. **W4** The vocabulary (§3.1):
   - exact `Os` values (recommended: one value per distinct system API, as
     listed);
   - families as `Bool` predicates in `Target` (recommended), rather than an
     enum hierarchy;
   - capabilities defined next to the code that uses them (recommended),
     rather than a global list in `Target`;
   - device class and browser at runtime only (recommended).
