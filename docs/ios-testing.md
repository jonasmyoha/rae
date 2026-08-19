# Running Rae on iOS — testing harness design

Status: **design** (no implementation yet). Goal: a repeatable way to run Rae's
featured examples (and later self-contained Rae app directories) on a real
iPhone, so we can profile performance on device — no Rae program has ever run on
iOS. Perf is now the motivating concern, so the design centres a **native**
device path, with a lighter WASM launcher for breadth and quick iteration.

---

## 1. Why iOS is close (grounding in the current build)

The current architecture is unusually portable, which makes iOS mostly a
**toolchain + packaging** problem, not a rewrite:

- **Runtime is one amalgamated translation unit.** `compiler/runtime/rae_runtime.c`
  `#include`s every `runtime_*.c`, and every app **recompiles it from source**
  alongside the generated app C. There is no prebuilt runtime lib. → An Xcode/CMake
  target just compiles `app.c` + `rae_runtime.c` with the right `-D` flags. Nothing
  to relink or repackage.
- **One place assembles the native link.** `gcc_link_c_to_binary()`
  (`compiler/src/main.c` ~2666–2738) is the *sole* site that builds the compile/link
  command. The module-graph capability detection (`uses_sdl3`, `uses_webgpu`) and the
  emitted `.deps` file (`build_c_backend_output`, ~2296–2420) is exactly the input an
  iOS generator needs. We hook here.
- **The GPU path is SDL3 + wgpu-native, and it's the same on iOS.** Surface creation
  (`runtime_gpu2d_platform.c` ~264–316) is `SDL_CreateWindow(SDL_WINDOW_METAL)` →
  `SDL_Metal_CreateView` → `SDL_Metal_GetLayer` (a `CAMetalLayer`) →
  `WGPUSurfaceSourceMetalLayer` → `wgpuInstanceCreateSurface`. SDL3 provides the
  identical Metal view on iOS (backed by a `UIView`), and the WGSL shaders are the
  same ("WebGPU-everywhere").
- **No `.m`/`.mm` files.** All Objective-C is `objc_msgSend` from plain C. The mac-only
  bits (`runtime_spotify_apple.c` via `osascript`, raylib's `Cocoa/OpenGL/IOKit/
  CoreVideo`) are legacy/off and excluded on iOS.
- **Codecs are vendored** (`lodepng.c`, `stb_image.h`), filesystem is POSIX
  (`runtime_filesystem.c`) — both iOS-portable.

**What is macOS/homebrew-specific and must be replaced for iOS:**

| Today (macOS) | iOS replacement |
|---|---|
| literal `gcc` + `/opt/homebrew/{include,lib}` | `xcrun clang -arch arm64 -isysroot <iPhoneOS.sdk>` (driven by Xcode/CMake) |
| `-lSDL3` (homebrew **dylib**) | **static SDL3** or `SDL3.xcframework` (device + simulator slices) |
| wgpu-native from `~/.local/wgpu-native` (**dylib + rpath**) | `wgpu_native.xcframework` built for `aarch64-apple-ios` (+ `-sim`) — App Store rules disallow arbitrary dylibs/rpaths |
| frameworks: keep `Metal QuartzCore CoreFoundation Foundation CoreGraphics ImageIO`; drop `Cocoa OpenGL IOKit CoreVideo` | all kept frameworks exist on iOS |
| CWD = repo root; assets by repo-root-relative or `{dir}` paths | bundle assets into the `.app`, `chdir(SDL_GetBasePath())` at launch (see §6) |

---

## 2. Two tracks (both wanted)

### Track A — Native compiled iOS app (the performance path) ★ primary

A real device build: generated C + `rae_runtime.c` compiled by clang for
`arm64-apple-ios`, linking static SDL3 + `wgpu_native.xcframework`, rendering
through Metal. **This is the only track that measures real performance** (native
codegen, Metal, no browser/WASM overhead) — which is the whole reason we're doing
this now. Profiled with Xcode Instruments (Metal System Trace, Time Profiler);
the examples already show an fps/mem overlay.

Cost: needs the two xcframeworks built once, an asset-bundling step, and the
render loop reconciled with iOS app lifecycle (§7).

### Track B — WASM launcher in a WKWebView (breadth + fast iteration)

A tiny native shell app hosting a `WKWebView` that loads the **existing emcc
WASM builds** (`emcc … -sUSE_SDL=3 --use-port=emdawnwebgpu`, real WebGPU via
Safari's `navigator.gpu`). An in-app list shows the featured examples; tapping
one loads its WASM. Reuses the `tools/devtools-web` gallery + `.raepack featured`
metadata. **Iterates without a native rebuild per example** (rebuild WASM, reload
the web view), so it's ideal for functional coverage across many examples.

Caveat: WASM-in-Safari-WebGPU perf is **not** representative of native — Track B
is for "does it run / does it look right on the phone", Track A is for "how fast".
Also depends on iOS Safari WebGPU maturity (improving but a moving target).

**Recommendation:** build Track A first (it answers the perf question and proves
the toolchain end-to-end), then Track B for breadth.

---

## 3. How many Xcode projects / targets

**Two Xcode projects, both generated (not hand-maintained):**

### `RaeExamples.xcodeproj` (Track A, native)
- **1 shared static-library target `RaeRuntime`** — compiles `rae_runtime.c` once
  with `RAE_HAS_SDL3` + `RAE_HAS_WEBGPU` on. Because the amalgamation stubs unused
  capabilities, a single runtime lib with GPU on serves every example (non-GPU
  examples just don't call into it).
- **1 app target per featured example** — compiles that example's generated
  `app.c`, links `RaeRuntime` + `SDL3` + `wgpu_native.xcframework` + the kept
  frameworks, and bundles the example's `assets/` (+ shared lib runtime data).
- **The "launcher" is Xcode's scheme dropdown** — the developer picks
  `114_walker_character`, `102_gpu2d_animated`, … and hits Run. No custom launcher
  code; N schemes = the picker. This is the pragmatic dev-harness "launcher".

Why not one app target that bundles many examples? Each generated `app.c` has its
own `main` + module-level globals, so several can't link into one binary without
collisions. A true in-app native picker would require each example as a separately
`dlopen`-ed embedded framework (allowed on iOS only for your own bundled, signed
frameworks) exposing a `rae_example_main()`. That's a real option for a polished
"Rae Examples" app later, but for a **test harness**, per-example targets +
scheme selection is far simpler and is what the generator produces.

### `RaeWebLauncher.xcodeproj` (Track B, WASM)
- **1 app target** — a `WKWebView` shell. Serves the featured examples' WASM
  builds (bundled, or from a dev machine over the LAN reusing the devtools-web
  server) and an HTML gallery to pick one. No per-example targets; adding an
  example is just building its WASM and adding it to the manifest.

### Dependencies (built once, vendored or fetched)
- `SDL3.xcframework` (device `arm64-apple-ios` + simulator `arm64/x86_64-apple-ios-simulator`).
- `wgpu_native.xcframework` — `cargo build --release --target aarch64-apple-ios`
  (+ `aarch64-apple-ios-sim`), packaged with `xcodebuild -create-xcframework`.
- Live under `tools/ios/frameworks/` (or a `tools/ios/fetch-deps.sh` that downloads
  pinned prebuilts). This is the main **new** infrastructure — today both deps are
  homebrew/`~/.local` dylibs (§1 table).

**So: 2 projects, 2 xcframeworks.** The native project has 1 lib target + 1 target
per featured example (generated); the web project has 1 target.

---

## 4. The generator — how the `.xcodeproj` is produced

Generating raw `.xcodeproj` XML by hand is brittle. Two robust options; **CMake is
recommended** because it re-expresses exactly what `gcc_link_c_to_binary` already
knows (source list, `-D` macros, frameworks) and can emit an Xcode project:

### Recommended: CMake + `-G Xcode`
`tools/ios/CMakeLists.txt` that, per example, defines an app target:
```
add_executable(ex_114 MACOSX_BUNDLE
    ${GEN}/114/app.c ${RUNTIME}/rae_runtime.c)
target_compile_definitions(ex_114 PRIVATE RAE_HAS_SDL3 RAE_HAS_WEBGPU)
target_include_directories(ex_114 PRIVATE ${RUNTIME} ${SDL3_INC} ${WGPU_INC})
target_link_libraries(ex_114 SDL3 wgpu_native
    "-framework Metal" "-framework QuartzCore" "-framework CoreFoundation"
    "-framework Foundation" "-framework CoreGraphics" "-framework ImageIO")
set_target_properties(ex_114 PROPERTIES
    RESOURCE "${EX114_ASSETS}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.rae.ex114
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
```
Configure with `cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS
-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<TEAM>`.
CMake handles the SDK, code signing, bundle, and Instruments-ready builds; it
naturally recompiles `rae_runtime.c` per target (matching today's model).

### Alternative: XcodeGen (YAML → `.xcodeproj`)
A declarative `project.yml` scripted from the featured list. Lighter than CMake for
pure Xcode output, but we'd re-encode SDK/signing/framework knowledge ourselves.
CMake wins because it already models "compile these C sources for this platform".

### The driver script: `tools/ios/gen.sh`
```
1. Discover featured examples: scan examples/*/*.raepack for `featured: "true"`
   (same field tools/devtools-web/src/server/examples.ts reads).
2. For each: rae build --entry <main.rae> --emit-c --out build/ios/gen/<name>/app.c
   (reusing the compiler's existing emit-c path — no new codegen).
   Capture its .deps (uses_sdl3/uses_webgpu) to set the -D macros per target.
3. Collect assets: the example's assets/ + the shared lib runtime data the emcc
   path already enumerates (lib/app3d/scenes, lib/data, lib/noise.wgsl — main.c
   ~2781–2807), bundled preserving identical relative paths (§6).
4. Emit tools/ios/CMakeLists.txt (or project.yml) listing one target per example.
5. cmake -G Xcode … → build/ios/RaeExamples.xcodeproj (gitignored; regenerable).
```
Everything under `build/ios/` is generated and git-ignored. `tools/ios/`
(the generator, framework build scripts, the WebLauncher sources) is committed.

### Supporting new Rae projects that need a dedicated Xcode project
The generator is **parameterised by a project directory + its `.raepack`**, so it
works for any example *or* a future self-contained app repo:
- `gen.sh --project <dir>` reads `<dir>/*.raepack` for entry, sources, assets, and
  capability hints, runs `emit-c`, and emits a single-target `.xcodeproj` for that
  app. A standalone Rae app ships its own `.raepack`; pointing the generator at it
  produces its Xcode project — no per-project bespoke setup.
- **Long-term**, fold this into the compiler as a first-class target, mirroring
  `--target wasm`: `rae build --target ios --out App.xcodeproj` (and later
  `rae ios run` → `xcodebuild` → device/simulator, `rae ios archive` → `.ipa`).
  Because the native-link knowledge is centralised in `gcc_link_c_to_binary`, this
  is "emit an xcodeproj/argv instead of calling `gcc … system()`" using the same
  source list + macros — the cleanest place to add it.

---

## 5. Milestones

1. **Deps as xcframeworks.** Build `SDL3.xcframework` + `wgpu_native.xcframework`
   (device + simulator). `tools/ios/build-deps.sh` + `fetch-deps.sh`. *This is the
   gating unknown — everything else is straightforward once these exist.*
2. **Hand "hello iPhone".** One example (start simple: `102_gpu2d_animated`;
   then `114_walker_character` for the GPU3D/grass perf test) compiled into a single
   hand-written iOS app target, run on device. Proves clang-for-iOS + static SDL3 +
   wgpu Metal surface + one WGSL shader on real hardware, and shakes out §6/§7.
3. **The generator.** `tools/ios/gen.sh` + CMakeLists producing `RaeExamples.xcodeproj`
   with a target per featured example. Scheme dropdown = the launcher.
4. **Asset bundling shim** (§6) generalised across all examples.
5. **Track B WebLauncher** — WKWebView + featured WASM gallery.
6. **Profiling pass** — Instruments on `114` (grass/deferred) at various densities;
   feed results back into the perf work that motivated this.
7. **(later) `rae build --target ios`** as a first-class compiler target.

---

## 6. Assets on iOS (the bundle-path shim)

iOS apps have no writable CWD at the repo root; they read from the `.app` bundle.
Today assets resolve two ways (both CWD/entry-relative, no bundling layer):
repo-root-relative paths (e.g. `examples/113_.../assets/character.glb`) and the
`{dir}` placeholder (entry directory).

Plan — **preserve identical relative paths in the bundle** (exactly what the emcc
path already does with `--preload-file X@/X`):
- Bundle each example's `assets/` **and** the shared lib runtime data
  (`lib/app3d/scenes`, `lib/data`, `lib/noise.wgsl`) into the `.app`, keeping the
  same relative layout the compiler enumerates for WASM.
- At startup, `chdir(SDL_GetBasePath())` — on iOS SDL3's base path **is** the bundle
  resource dir, so all existing repo-root-relative + `{dir}` reads resolve with
  **zero Objective-C**. (If a case needs absolute bundle lookup, `objc_msgSend` to
  `NSBundle` is available, matching the existing pattern — but SDL_GetBasePath
  should suffice.)
- The generator computes the per-example resource set from the same dependency
  enumeration the WASM build uses, so no per-example asset config by hand.

---

## 7. Open questions / risks

- **App lifecycle vs the busy render loop.** Examples own their loop
  (`loop not windowShouldClose()`; hybrid wait/render per `docs/ui-render-loop-
  performance.md`). iOS wants the app to yield to the system run loop and handle
  suspend/resume/background. SDL3 on iOS pumps via `CADisplayLink` and supports
  `SDL_PollEvent`, but we should decide: keep the busy loop (simplest, works, but
  must pause on background) or adopt SDL3's main-callback API
  (`SDL_AppInit/Iterate/Event/Quit`). A thin per-target shim can bridge either way;
  needs a spike on device.
- **wgpu-native on iOS.** Confirm the Rust build for `aarch64-apple-ios` is clean and
  that the Metal backend + our feature usage (compute, `DrawIndirect`, storage
  buffers used by the grass system) work on iPhone GPUs (they should — all Metal).
  Simulator uses a different Metal path; test on device.
- **Static SDL3 + `SDL_main`.** iOS SDL apps rename `main`→`SDL_main` and SDL owns
  `UIApplicationMain`. The generated app's `main` must be reconciled (SDL's
  `SDL_MAIN_HANDLED`/`SDL_main` macro, or a small entry shim per target).
- **Code signing / provisioning** for device installs (Automatic signing + a
  development team in CMake attrs). CI device runs need a provisioning profile.
- **Binary size** — each native example bundles the runtime + wgpu-native; fine for
  dev, worth noting if we ship a combined app.
- **iOS Safari WebGPU maturity** gates Track B fidelity; it's a moving target.
- **HiDPI / safe areas / touch input** — SDL3 handles Retina + touch, but the
  examples assume mouse; touch→pointer mapping and safe-area insets (notch) need a
  pass (ties into the existing `lib/ui` coordinate/responsive work, #207/#209).
- **Perf overlay** already exists (fps/mem), so on-device numbers are visible
  immediately; pair with Instruments for the real profile.

---

## 8. Summary

- **2 Xcode projects, both generated:** `RaeExamples.xcodeproj` (native — 1 shared
  `RaeRuntime` static lib + 1 app target per featured example; the scheme dropdown
  is the launcher) and `RaeWebLauncher.xcodeproj` (a WKWebView hosting the existing
  WASM builds for breadth/iteration).
- **2 xcframeworks** built once: `SDL3` + `wgpu_native` (device + simulator). This
  is the only genuinely new infra — the rest reuses today's emit-C + amalgamated
  runtime unchanged.
- **Generator = CMake (`-G Xcode`) driven by `tools/ios/gen.sh`**, which reuses the
  compiler's `--emit-c` and the `.raepack featured` list; parameterised by project
  dir so it also serves future standalone Rae app repos, and is the natural seed for
  a first-class `rae build --target ios` later.
- **Native track is the perf answer**; WASM launcher is for coverage. Ship the deps
  xcframeworks + a hand "hello iPhone" first (milestone 1–2) to de-risk, then the
  generator.
