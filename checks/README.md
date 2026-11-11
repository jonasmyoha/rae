# Hardware checks

A check is a small program that tests one part of the runtime on real
hardware: a GPU, or a browser. It prints what it tested and exits 0 when every
test in it passed. It is not an example (there is nothing to look at) and not
a unit test case (the unit suite does not need a GPU device or a browser).

| check | tests |
|---|---|
| `backdropBlur` | `BackdropBlur` from a scene alone: three frosted tiles over black and white stripes, through the real pipeline; the picture (`RAE_GPU2D_SCREENSHOT`) is read back: the stripes under a tile are an even gray, outside they stay black and white, and the tiles of one layer took one blurred copy; over 20 frames a copy is kept while nothing under the tiles changes (only frames 0, 1 and the one where the stripes move blur) |
| `canopyCoverage` | `CanopyBake` + the sprite pass: a far canopy cluster of each tree species covers as many pixels as its trees drawn as single impostors (from 150, 700 and 1,100 m, four directions; within 13%, the phone settings 25% with `RAE_IMPOSTOR_PHONE=1`); `RAE_CANOPY_BENCH=1` times a whole far forest of clusters on the GPU, marched and not |
| `leafParticles` | `LeafParticles` (docs/leaf-particles-design.md §4): two crowns release 280 leaves over 20 frames (under the frame cap) into the GPU ring in a wind; read back, they fall and drift downwind in their crowns' colors, most land on the ground (the swirls keep some aloft), lie, and are gone after a leaf's life; a leaf past the reach dies at once; a ground lift's leaves rise off the ground; `RAE_LEAF_BENCH=1` times the compute of a full ring in a gale and a breeze instead |
| `leafWind` | production wind shader: rigid patch distances, fixed pivot, rotated normals, exact calm and actual motion over 64 times |
| `windGust` | the gust field (docs/leaf-particles-design.md §2.3): lib/wind.wgsl `windGustAt` on the GPU equals lib/WindGust.rae's at 256 points in a calm, a breeze, a gale and near the drift's wrap (within 0.001) |
| `surfaceImages` | opaque material image owner: sRGB conversion, white fallback, two-layer mip array, failed replacement preserving the previous image, resource retirement |
| `gpuArgs` | `gpu/GpuArgs`: a packed argument struct reads back the same through WGSL's own layout |
| `gpuLifetime` | `gpu/GpuLifetime`: recordings, submissions, completion polling, retirement behind in-flight work |
| `gpu2dTiming` | the 2D canvas's GPU time per frame (`Gpu2dCanvasTiming`, `RAE_GPU2D_TIMING=1`): the frame pass's own timestamps read back without waiting; 40 light and 40 heavy frames are timed, and the heavy ones take longer |
| `gpuManagerTiming` | `gpu/GpuTiming`, the one GPU timing module: per-pass timestamps read back without blocking, named passes, and the last sample published as profiler counters (gpu.<pass>, gpu.total) |
| `gpuReadback` | `gpu/GpuReadback`: concurrent manager-owned readbacks, reuse, cancellation, shutdown |
| `gpuRender` | `gpu/GpuRender`: a manager render pipeline draws into a texture that is read back and compared |
| `gpuResources` | `gpu/GpuResources`: two managers, known buffer contents, stale ids, exhaustion, cleanup |
| `impostorBake` | `ImpostorBake` and `CanopyBake`: every tree species baked into an octahedral impostor atlas on the GPU, then a far canopy of four 40 m cells baked from those impostors; each covers part of its layer and every mip keeps level 0's coverage within 3%; writes the atlas as PNGs to `RAE_IMPOSTOR_PNG_DIR` (default `/tmp/rae-impostor-atlas`; `RAE_IMPOSTOR_PHONE=1` bakes the phone's 4 x 4) |
| `localNotification` | lib/LocalNotification on a real iPhone: rings with the app closed, the same id replaces, a cancel holds (docs/local-notifications.md). Off iOS it passes with nothing to test |
| `uiAppear` | `Appear` and `Stagger` from a scene alone, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a true middle state. Pictures: docs/images/ui-appear |
| `uiFocusField` | `FocusStyle` from a scene alone: a field gains focus and its edge brightens on its motion, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures (`docs/images/ui-focus/`) |
| `uiLayoutText` | `AnimateLayout` and `AnimateText` from a scene alone, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of rows gliding, a text fading and a clock rolling (`docs/images/ui-layout-text/`) |
| `uiDialogs` | dialog motion from a card with none of its own, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a dialog opening, closing (`RAE_DIALOG_CHECK=close`), one over another (`over`) and a full page (`page`) (`docs/images/ui-dialogs/`) |
| `uiLists` | list motion through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a ghost, a new row and a glide, or with `RAE_LIST_CHECK=sideways` a change of all the data (`docs/images/ui-lists/`) |
| `uiPageBudget` | how a 30-row page transition with a new world holds its frame rate, by part, on a device (`RAE_IOS_HIGH_REFRESH=1 sh tools/ios/gen-ios.sh checks/uiPageBudget`, then `run-ios.sh`; the `[pageBudget]` lines are in the app's `Documents/rae.log`) |
| `uiPageTransitions` | a page transition with a new world per screen, with a `SharedElement` bar that stays and a tile that morphs, through the real pipeline and `pageTransitionDraw`, and `RAE_ANIMATION_HOLD` pictures by `RAE_PAGE_KIND` and `RAE_PAGE_BACK`; `RAE_PAGE_DRAG` runs it from a held finger's share of the way (`docs/images/ui-pages/`) |
| `uiZoom` | a `TransformFx` scale on a whole subtree, painted: a card with a stroke, a shadow, a gradient, text, an icon and a clipped list, at `RAE_UI_ZOOM`. Pictures: docs/images/ui-zoom |
| `webPackageAsset` | a package's shader and declared file, natively and on a page (`compiler/tools/wasm_webgpu_smoke.sh` runs it in headless Chrome) |
| `readRequest` | `webgpu/ReadRequest`: independent buffer reads (docs/webgpu-readback.md) |
| `webRelay` | a player reaches a relay session over WebSocket and gets 100 messages echoed in order by a UDP host: natively (its own relay and host) and in the browser (`compiler/tools/wasm_relay_smoke.sh`, the page's own WebSocket) |
| `webReadback` | a nonblocking readback completes in a `mainLoop`, natively and in the browser |
| `webTask` | a spawned task that sleeps runs between the frames of a `mainLoop`, natively and in the browser |
| `audioSession` | `packages/audio`'s session on an iPhone (`RAE_IOS_TEAM=<team> RAE_IOS_FOCUS_USAGE=<words> sh tools/ios/run-ios.sh checks/audioSession`, then read `Documents/rae.log` from the app's container): an ambient tone, an alert, 20 s of music with the session's events logged (unplug headphones, take a call, ask Siri meanwhile), the device's callbacks at each step, and the Focus status before and after asking (docs/audio-design.md §9.2). Off iOS it plays the same sounds and passes |
| `audioAac` | `packages/audio`'s AAC in M4A through the platform's decoder (AudioToolbox on Apple, AMediaCodec on Android: `sh tools/android/gen-android.sh checks/audioAac`, then `sh tools/android/run-android.sh audioAac`): fixture 1414's two files against afconvert's decodes (same frames, no delay, within -40 dB), seeks, a stream. Where there is no AAC decoder it passes with nothing to test |
| `audioInput` | `packages/audio`'s recording through a duplex device (the default input: an Apogee Duet, the built-in microphone): the permission, the input channels, the output and input latencies and their round trip, 2 s recorded (none lost, the frame count right, the level). `RAE_AUDIO_ECHO_TEST=1` plays four quiet clicks and finds them in the aligned recording; `RAE_AUDIO_ASK_MICROPHONE=1` asks for the permission. Without either an input or the permission it passes with nothing to test |
| `audioMidi` | `packages/audio`'s MIDI (CoreMIDI): the sources and destinations there are, and a loopback through the system's MIDI server (virtual ports): notes with running status, system exclusive, pitch bend, a controller, played on the synth with the music bus muted (`RAE_AUDIO_MIDI_LISTEN=1` unmutes, `RAE_CHECK_SECONDS` leaves time to play a keyboard) |
| `audioNowPlaying` | `packages/audio`'s Now Playing, lock-screen buttons and background play (`RAE_IOS_TEAM=<team> RAE_IOS_BACKGROUND_AUDIO=1 sh tools/ios/run-ios.sh checks/audioNowPlaying`, then read `Documents/rae.log`): 100 s of a tune streamed with the `music` intent and shown in Now Playing; each button press and what it did, the app going to the background and back, and the place in the song every 5 s are logged. Passes when the song moved on in the background and the callbacks kept coming. Natively it runs 3 s (`RAE_CHECK_SECONDS`) and shows in the Mac's Now Playing. On Android (`gen-android.sh`, then `RAE_ANDROID_ENV="RAE_CHECK_SECONDS=40" run-android.sh audioNowPlaying 70`) it holds the audio focus and a MediaSession: send buttons with `adb shell cmd media_session dispatch play-pause` or `input keyevent KEYCODE_MEDIA_PLAY_PAUSE`, and see the focus with `dumpsys audio` |
| `audioSine` | `packages/audio` on the real default output: a sine, then one panned left to right, through the stage-A render thread; passes when the device ran, both voices ended and it played no silence for lack of samples (`RAE_REALTIME_CHECK=1` also fails it on an allocation in the render thread). With no output device it passes with nothing to test |

## Running

```sh
bash checks/run.sh                              # every check, natively
RAE_CHECK_FILTER="gpuArgs webTask" bash checks/run.sh
make -C compiler checks                         # the same
```

`make test-examples` (the visual examples gate) runs them too when it runs
every example. The two `web*` checks are also built for the browser and run in
headless Chrome by `compiler/tools/wasm_webgpu_smoke.sh`.

A new check is a folder here with a `Main.rae` whose `main` returns 0 only
when it passed. The devtools page lists the checks in their own Checks tab.
