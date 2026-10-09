# Per-platform code: `when` (design)

**Status:** design approved by the maintainer, 2026-10-08: W1-W4 (§6) are
decided as recommended. The implementation is queued in seven steps (§5).

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

At first the platform choice was made in C. `runtime_crypto_platform.c` had
an `#if defined(__APPLE__)` body that called CommonCrypto and a stub
elsewhere that answered -1, and `lib/crypto/Sha1.rae` called the shim on
every platform and fell back to Rae on -1. Since 0.1.229, the
choice is in Rae: the binding is declared only under
`when hasCommonCrypto`, and the stub is gone. The kqueue poller, FileNotify and Float4's
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

**The runtime model: separate facts, never "is it a tablet".** "Tablet",
"phone" and "laptop" each bundle three independent things: which inputs
exist, which one is in use, and how much room the window has. Hardware keeps
pulling them apart:
- a touchscreen MacBook (expected soon) has touch, but the trackpad is still
  its main pointer;
- an iPad gains a trackpad when a keyboard case is attached;
- iPad Split View gives an app a phone-sized window on a tablet;
- a foldable changes size while running.

So `Device` answers the separate questions, the same split CSS media queries
settled on (`any-pointer`, `pointer`, `hover`):

```rae
Device.hasTouch() ret Bool               # any touch input present (the touch MacBook: true)
Device.primaryPointer() ret Pointer      # Pointer { fine, coarse, none }: the touch MacBook: fine; a phone: coarse
Device.canHover() ret Bool               # a mouse/trackpad that hovers exists
Display.scale() ret Float                # pixels per point (2.0 on Retina); layout works in points
```

| device | hasTouch | primaryPointer | canHover |
|---|---|---|---|
| desktop / MacBook today | false | fine | true |
| touch MacBook | true | fine | true |
| iPhone, Android phone | true | coarse | false |
| iPad alone | true | coarse | false |
| iPad with trackpad case | true | coarse, or fine once the trackpad is used | true |
| Chromebook / touch laptop | true | fine | true |
| TV with remote | false | none | false |

Three rules for UI code:

1. **Size from the window, in points, never from the device or raw pixels.**
   - Layout picks a size class from the window's width in points (for
     example compact under 600, medium to 840, expanded above, Material 3's
     breakpoints; Apple's compact/regular is the same idea).
   - Pixels are points × `Display.scale()`, so a Retina screen and a
     1080p screen get the same layout at the same window size.
   - An iPad in Split View gets the compact layout because its window IS
     compact, which `isTablet()` would get wrong.
   - Physical size (millimetres, from the display's DPI) is for the rare
     thing that must be physically sized: a ruler, a print preview. It is
     never for layout.
2. **Hit targets and hover from the pointer, per interaction.**
   - Every pointer event carries its kind (`touch`, `mouse`, `pen`).
   - A control pressed by touch uses the touch-sized target and feedback.
     The same control clicked by a trackpad on the same touch MacBook uses
     the precise one, and hover effects appear only when `canHover()` is
     true.
   - lib/ui keeps the last input kind, so a whole screen can switch density
     the way iPadOS and Windows do when the keyboard comes off.
3. **These values change while the app runs.**
   - A keyboard case is attached, a window is resized, a monitor is
     unplugged, a foldable unfolds.
   - They arrive as events (lib/ui's input and window resources), and
     layout reruns. Nothing reads them once at startup.

`Device` and `Display` are `lib/Device.rae` and `lib/Display.rae` (0.1.240):
- Natively they are filled from SDL3: the touch device count, whether a
  mouse or trackpad is attached (`SDL_HasMouse`), and the window's display
  scale. In the browser they come from the media queries `any-pointer:
  coarse`, `pointer`, `any-hover` and `devicePixelRatio`.
- The C is one call per answer (`compiler/runtime/runtime_device.c`). Which
  pointer is primary is Rae (`factsFromInputDevices`,
  `factsFromMediaQueries`): on a mobile OS touch stays primary with a
  trackpad attached, elsewhere a mouse or trackpad is.
- With no window (a headless run) SDL lists no devices, so the facts are no
  touch, pointer `none`, no hover, scale 1.
- A change is an event: an app keeps a `DeviceWatch` and asks
  `Device.deviceChanged(watch:)` once per frame. It reads the facts again,
  including the display scale, and answers true once when any of them
  changed (a keyboard case, a monitor move). `observeFacts` takes a reading
  directly, which is how fixture 1067 simulates one.

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
| a browser on a phone | runtime, in a `web` build: `Device.primaryPointer() is Pointer.coarse` |
| touch-sized controls | runtime: the event's pointer kind, or the last input kind |
| a tablet-shaped layout | runtime: the window's size class in points (no `isTablet`) |
| hover effects | runtime: `Device.canHover()` |

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
  func commonCryptoSha1(bytes: Buffer(UInt8), offset: Int, count: Int, digest: Buffer(UInt8)) unsafe extern(
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

## 5. Phasing (queued 2026-10-08 as "when 1/7" … "when 7/7")

0. The `Target` module (§3.1 layers 1–2), with a check-only
   `--target-os` / `--target-arch` override so fixtures can fake a target. **Done (0.1.227):**
   - `lib/Target.rae` is imported by every program but opened by none, so a
     bare `os` is an error that says to write `Target.os`.
   - The compiler writes the build's values into its three consts.
   - `const` folding now evaluates Bools, enum cases, `is`/`is not`,
     `and`/`or`/`not` and qualified consts.
   - `rae build --target-os/--target-arch` (check-only) and `--print-target`
     exist.
   - Fixtures 1045–1048.
1. `when` at statement and declaration level. **Done (0.1.228):**
   - `compiler/src/when_resolve.c` keeps the selected branch after the merge
     and before sema: declarations are spliced in, to a fixed point, and a
     statement becomes `if true { … }`.
   - Fixtures 1049–1056.
   - `--report-waits` follows the selection, also with `--target-os`.

   The fixtures cover:
   - each branch selected by a different `Target` value (one test fakes the
     target);
   - an error for a non-constant condition;
   - an error for a reference to an unselected declaration;
   - formatting of every branch.
2. Move SHA-1's platform choice into Rae: the extern is declared only for
   Apple, and the `#else` stub is removed. **Done (0.1.229):**
   - `const hasCommonCrypto: Bool = Target.isApple`, with
     `commonCryptoSha1` declared under it and selected by a statement-level
     `when`;
   - fixture 1057 type-checks the portable path on a faked Linux;
   - 16 MB still takes 7.3 ms on macOS.
3. `rae build --check-targets` and its pre-suite case. **Done (0.1.230):**
   - The real pairs are one table in `compiler/src/main.c`
     (`k_check_targets`): 17 pairs, `web` only with `wasm32`.
   - Each target is a check-only child build, because the compiler keeps
     state between builds in one process. It prints one ok/FAILED line per
     target, under that target's errors.
   - `compiler/tools/check-targets.sh` is the pre-suite case. It checks
     `compiler/tests/checkTargets/Main.rae`, which must import every lib/
     module that uses `when`, and the script fails when one is missing.
   - Fixtures 1058 (a Linux-only type error fails exactly the Linux-kernel
     targets) and 1059 (the same program runs on macOS).
4. Migrate the other C `#if` choices that are really policy (lib/net's
   kqueue/epoll, FileNotify) to capabilities. Float4's lowering choice stays
   in C, because it is per-instruction. **Done (0.1.231):**
   - `hasKqueue` / `hasEpoll` live in `lib/net/NetSystem.rae`, and the
     kqueue poller and FileNotify shims are declared under
     `when hasKqueue`.
   - The C stubs are gone, and `RAE_HAS_KQUEUE` is the C twin of
     `hasKqueue`.
   - Every runtime `#if` is classified in docs/runtime-c-audit.md. Three
     policy ones remain and are queued: sockets on web/Windows, the Apple
     platform calls, and the Spotify bridge.
5. The runtime `Device` / `Display` facts (§3.1 layer 4). **Done (0.1.240):**
   `lib/Device.rae`, `lib/Display.rae`, `runtime_device.c`, fixture 1067.
6. lib/ui: the per-event pointer kind, `lastInputKind`, hover only when
   `canHover`, and the window size class in points.
   **Done (0.1.241):**
   - `lib/ui/InputKind.rae`: `InputKind { mouse, touch, pen }`, from the
     gpu2d event pump (SDL3 marks mouse events made from a touch or a pen
     with `SDL_TOUCH_MOUSEID` / `SDL_PEN_MOUSEID`; in the browser SDL reads
     Pointer Events). Also `pointInTarget` (a rectangle grown to at least a
     target size), `touchTargetPoints` (44) and `showsHover`.
   - `lib/ui/SizeClass.rae`: `SizeClass { compact, medium, expanded }` from
     the width in points (< 600, < 840), and the `WindowSizeClass` resource.
   - lib/ui's `UiInput`: each `UiPointerState` carries its `kind`, there is
     `lastInputKind`, and hit testing tries the precise rectangle first and
     then, for a finger only, touch-sized targets. Hover is marked only when
     `showsHover`.
   - 106_mobile_ui's own input system does the same (44 pt converted to its
     design units), drops hover for a finger or a device without hover, and
     recomputes the size class every frame, logging a change.
   - 106 lays out by it when no phone preset is emulated
     (`RAE_UI_DEVICE=none`): compact fills the window with the page column,
     medium widens the column so the grids gain columns (Home 2 -> 3,
     Library 3 -> 4), expanded adds the Library as a left side panel with the
     dock a bar under both (`Viewport.rae`, `screenSystem/SidePanel.rae`).
     A size class change rebuilds the world. `RAE_UI_WINDOW=WxH` starts at a
     given size in points, to check each class headlessly.
   - Fixture 1068.

## 6. Decisions (maintainer, 2026-10-08: all four as recommended)

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
   - device class and browser at runtime only (recommended), as the separate
     facts `hasTouch`, `primaryPointer`, `canHover`, `Display.scale`, the
     per-event pointer kind and the window's size class. There is no
     `isTablet` / `isPhone` / `isTouchPrimary`.
