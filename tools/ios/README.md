# tools/ios — running Rae on iOS

Design: `docs/ios-testing.md`. Tasks: `#516` (umbrella), `#518`–`#524`.

Rae's native target (Compiled: emit C → binary) ports to iOS by compiling the
generated `app.c` + the amalgamated `rae_runtime.c` for `arm64-apple-ios`, linking
static SDL3 + wgpu-native (Metal), and bundling assets. WASM is the other track
(a WKWebView launcher, `#523`).

## One-time: build the dependency xcframeworks

Both deps ship only as macOS builds; build the iOS versions once:

```sh
sh tools/ios/build-wgpu-ios.sh    # downloads matching v29.0.1.1 iOS libs → wgpu_native.xcframework
sh tools/ios/build-sdl3-ios.sh    # builds SDL release-3.4.10 from source → SDL3.xcframework
```

Both land in `tools/ios/frameworks/` (git-ignored — regenerate with the scripts).

## Generate + run one example on device

```sh
sh tools/ios/gen-ios.sh examples/102_gpu2d_animated
open build/ios/102_gpu2d_animated/proj/RaeApp.xcodeproj
```

In Xcode: **Signing & Capabilities → Team** (Automatic), pick your plugged
device, **Run**. Signing needs your Apple ID/team and can't be scripted.

The generator: emits the app C, prepends `<SDL3/SDL_main.h>` (so SDL owns the iOS
entry), bundles the example's `assets/` preserving relative paths, and writes a
CMake `-G Xcode` project. `ios_bootstrap.c` `chdir`s to the bundle at launch so
relative asset reads resolve.

## Status / next

- `build-wgpu-ios.sh`, `build-sdl3-ios.sh`, `gen-ios.sh`, `ios_bootstrap.c`: done.
- `#520`: first device run of `102` then `114` (validate SDL_main entry, the busy
  render loop vs iOS lifecycle, and touch input).
- `#521`: multi-example generator (one target per `.raepack featured` example,
  scheme dropdown = launcher).
- `#523`: WKWebView WASM launcher.
