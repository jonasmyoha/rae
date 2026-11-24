# Hardware checks

A check is a small program that tests one part of the runtime on real
hardware: a GPU, or a browser. It prints what it tested and exits 0 when every
test in it passed. It is not an example (there is nothing to look at) and not
a unit test case (the unit suite does not need a GPU device or a browser).

| check | tests |
|---|---|
| `backdropBlur` | `BackdropBlur` from a scene alone: three frosted tiles over black and white stripes, through the real pipeline; the picture (`RAE_GPU2D_SCREENSHOT`, a temp file when unset; 20 frames unless `RAE_HEADLESS_FRAMES` says otherwise) is read back: the stripes under a tile are an even gray, outside they stay black and white, and the tiles of one layer took one blurred copy; over 20 frames a copy is kept while nothing under the tiles changes (only frames 0, 1 and the one where the stripes move blur) |
| `canopyCoverage` | `CanopyBake` + the sprite pass: a far canopy cluster of each tree species covers as many pixels as its trees drawn as single impostors (from 150, 700 and 1,100 m, four directions; within 13%, the phone settings 25% with `RAE_IMPOSTOR_PHONE=1`); `RAE_CANOPY_BENCH=1` times a whole far forest of clusters on the GPU, marched and not |
| `leafParticles` | `LeafParticles` (docs/leaf-particles-design.md §4): two crowns release 280 leaves over 20 frames (under the frame cap) into the GPU ring in a wind; read back, they fall and drift downwind in their crowns' colors, most land on the ground (the swirls keep some aloft), lie, and are gone after a leaf's life; a leaf past the reach dies at once; a ground lift's leaves rise off the ground; `RAE_LEAF_BENCH=1` times the compute of a full ring in a gale and a breeze instead |
| `leafWind` | production wind shader: rigid patch distances, fixed pivot, rotated normals, exact calm and actual motion over 64 times |
| `windGust` | the gust field (docs/leaf-particles-design.md §2.3): lib/wind.wgsl `windGustAt` on the GPU equals lib/WindGust.rae's at 256 points in a calm, a breeze, a gale and near the drift's wrap (within 0.001) |
| `surfaceImages` | opaque material image owner: sRGB conversion, white fallback, two-layer mip array, failed replacement preserving the previous image, resource retirement |
| `gpuArgs` | `gpu/GpuArgs`: a packed argument struct reads back the same through WGSL's own layout |
| `gpuLifetime` | `gpu/GpuLifetime`: recordings, submissions, completion polling, retirement behind in-flight work |
| `gpu2dTiming` | the 2D canvas's GPU time per frame (`Gpu2dCanvasTiming`, `RAE_GPU2D_TIMING=1`): the frame pass's own timestamps read back without waiting; 40 light and 40 heavy frames are timed, and the heavy ones take longer |
| `spriteHueRotate` | a Sprite's `hueRotate` and `brightness` on the GPU: one blue image drawn by four sprites of a scene (0, 120, -65 degrees, and 120 at brightness 1.3) through the real pipeline; the frame read back at each sprite reads blue, red, green and a brighter red, each within 3 steps of `imageColorOf` (case 1569 has the numbers without a GPU) |
| `gpu2dSizedCapture` | `Gpu2dCanvasCache` sized slots: a box and a word drawn into a 3840x2160 slot from a 1280x800 window, read back against their pixels and the font's glyph boxes; the frame keeps its own transform after the capture |
| `gpuManagerTiming` | `gpu/GpuTiming`, the one GPU timing module: per-pass timestamps read back without blocking, named passes, and the last sample published as profiler counters (gpu.<pass>, gpu.total) |
| `gpuReadback` | `gpu/GpuReadback`: concurrent manager-owned readbacks, reuse, cancellation, shutdown |
| `gpuRender` | `gpu/GpuRender`: a manager render pipeline draws into a texture that is read back and compared |
| `gpuTextureRegion` | `gpu/GpuRenderTransfer` writeTextureRegion: four 32x32 regions of a 64x64 texture written and read back; out-of-bounds, short and non-CopyDst regions refused |
| `gpuResources` | `gpu/GpuResources`: two managers, known buffer contents, stale ids, exhaustion, cleanup |
| `impostorBake` | `ImpostorBake` and `CanopyBake`: every tree species baked into an octahedral impostor atlas on the GPU, then a far canopy of four 40 m cells baked from those impostors; each covers part of its layer and every mip keeps level 0's coverage within 3%; writes the atlas as PNGs to `RAE_IMPOSTOR_PNG_DIR` (default `/tmp/rae-impostor-atlas`; `RAE_IMPOSTOR_PHONE=1` bakes the phone's 4 x 4) |
| `localNotification` | lib/LocalNotification on a real iPhone: rings with the app closed, the same id replaces, a cancel holds (docs/local-notifications.md). Off iOS it passes with nothing to test |
| `uiAppear` | `Appear` and `Stagger` from a scene alone, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a true middle state. Pictures: docs/images/ui-appear |
| `uiFocusField` | `FocusStyle` from a scene alone: a field gains focus and its edge brightens on its motion, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures (`docs/images/ui-focus/`) |
| `uiLayoutText` | `AnimateLayout` and `AnimateText` from a scene alone, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of rows gliding, a text fading and a clock rolling (`docs/images/ui-layout-text/`) |
| `uiLayerEffects` | layer effects on the GPU (docs/ui-layers-design.md §8, §9): a toast that slid in (animateLayer) under an open dialog's blurred, tinted scrim reads darkened with its light spread past its edge; an isolated layer at opacity 0.5 reads the same gray where two of its white boxes overlap as where they do not (drawn box by box the overlap reads 191, not 128) |
| `uiLayerOrder` | the layer paint order on the GPU (docs/ui-layers-design.md): one frame from a program walking its own layer plan, read back: its hudOver draw over a hud box and under a dock box, a QR code (qr 1.2.0, a CustomPaint node) darkened by a dialog's scrim over it, a white draw at layer opacity 0.5 a mid gray, a dock draw cut at a 100-point bottom safe area |
| `uiTextOutline` | text outlines from the theme: two styles with their own `outline` tokens (a palette brown 1.5 pt, a gold 3 pt) in one frame, each drawn in its color at its width through the real pipeline; the parsing is in case 1524 |
| `uiDialogs` | dialog motion from a card with none of its own, through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a dialog opening, closing (`RAE_DIALOG_CHECK=close`), one over another (`over`) and a full page (`page`) (`docs/images/ui-dialogs/`) |
| `uiMenu` | a menu of `ui/MenuSystem` with icons, shortcuts, a separator, a disabled item and its submenu open; a picture |
| `uiLists` | list motion through the real pipeline, and `RAE_ANIMATION_HOLD` pictures of a ghost, a new row and a glide, or with `RAE_LIST_CHECK=sideways` a change of all the data (`docs/images/ui-lists/`) |
| `uiPageBudget` | how a 30-row page transition with a new world holds its frame rate, by part, on a device (`RAE_IOS_HIGH_REFRESH=1 sh tools/ios/gen-ios.sh checks/uiPageBudget`, then `run-ios.sh`; the `[pageBudget]` lines are in the app's `Documents/rae.log`) |
| `uiPageTransitions` | a page transition with a new world per screen, with a `SharedElement` bar that stays and a tile that morphs, through the real pipeline and `pageTransitionDraw`, and `RAE_ANIMATION_HOLD` pictures by `RAE_PAGE_KIND` and `RAE_PAGE_BACK`; `RAE_PAGE_DRAG` runs it from a held finger's share of the way (`docs/images/ui-pages/`) |
| `perfHud` | the perf HUD (`ui/PerfHud`) over a frame loop that keeps the pool's workers busy: frames in its graph, a bar for the main thread and each worker, the save button's `.raeperf` reads back, and closing it ends its capture |
| `uiPanes` | dividers and scrollbars: a pane sized by its divider, a list scrolled by its thumb, a strip with a track; a picture |
| `uiTextField` | a focused text field wider than its place, scrolled to its caret with a part selected; a picture |
| `uiTimeline` | the `Timeline` chart from a scene (three lanes, spans, marks, one selected), painted and compared with its `reference.png`; the rules are in case 1472 |
| `uiFlameHistogram` | the `FlameGraph` and `Histogram` charts, painted and compared with its `reference.png`; the rules are in case 1473 |
| `uiLineTable` | the `LineGraph` and `StatTable` charts, painted and compared with its `reference.png`; the rules are in case 1474 |
| `uiCanvasCull` | drawing only what is on screen (`lib/ui/RenderSystem/RenderCull`): 1,000 mini timelines (4-node sub-scenes) on a Camera2D canvas; the paint walk zoomed in on about 30 of them must take under 2 ms (it paints about 210 nodes, against all 3,930 zoomed out). Run headless (`RAE_HEADLESS_FRAMES=100000`): a window waits for the display's refresh. `RAE_CANVAS_COUNT` sets the size |
| `uiPlaneView` | `PlaneView` (lib/ui/planeViewSystem): 50,000 mini timelines on a Camera2D canvas, only the ones in view live; for each camera (zoomed in to everything on screen) under 2,000 entities and 2 ms of systems a frame, the mount under 100 ms, and an edit kept after scrolling out and back. Run headless (`RAE_HEADLESS_FRAMES=100000`); `RAE_CANVAS_COUNT` sets the size |
| `uiHitIndex` | the hit index (ui/HitIndex): 50,000 clickable nodes; a hit test under 5 us and the same as the walk's, and what keeping the index costs (built, idle, a panned camera, one node moved). Needs no window |
| `uiWires` | `Wires` (ui/WireComponents): 10,000 wires between 2,000 boxes, each a curve in the frame's one box draw; drawing them takes under 2 ms a frame (the CPU's part) with all on screen, and the whole frame is printed. Run headless; `RAE_GPU2D_SCREENSHOT` keeps a picture, `RAE_WIRES_PER_BOX=0` times the frame without wires |
| `uiPaintCache` | `PaintCache` (ui/RenderSystem/RenderPaintCache): 50,000 mini timelines on a Camera2D canvas; zoomed out, the frames drawn from the cached picture under 8 ms, the capture frame and the zoomed-in nodes timed, and the pop where the picture gives way to the nodes compared pixel by pixel (picture drawn at that zoom, and at 0.7 of it scaled up). Run headless (`RAE_HEADLESS_FRAMES=100000`); `RAE_CANVAS_COUNT` sets the size |
| `uiZoom` | a `TransformFx` scale on a whole subtree, painted: a card with a stroke, a shadow, a gradient, text, an icon and a clipped list, at `RAE_UI_ZOOM`. Pictures: docs/images/ui-zoom |
| `webPackageAsset` | a package's shader and declared file, natively and on a page (`compiler/tools/wasm_webgpu_smoke.sh` runs it in headless Chrome) |
| `readRequest` | `webgpu/ReadRequest`: independent buffer reads (docs/webgpu-readback.md) |
| `storeKit` | `packages/storeKit` (docs/store-kit.md): on a Mac, products, entitlements and finish requests start, finish and are read back, and the updates listener starts (no purchase: that needs an app-hosted test); `bash checks/storeKit/simulator.sh` runs the same program as an iOS app in the iOS Simulator |
| `storeKitPurchase` | not run by `run.sh` (its entry is `Purchase.rae`): `bash checks/storeKitPurchase/run.sh` on a Mac with Xcode runs the storeKit purchase test, an app-hosted XCTest over `packages/storeKit/testing/Products.storekit` that calls the Rae program (docs/store-kit.md) |
| `webRelay` | a player reaches a relay session over WebSocket and gets 100 messages echoed in order by a UDP host: natively (its own relay and host) and in the browser (`compiler/tools/wasm_relay_smoke.sh`, the page's own WebSocket) |
| `webReadback` | a nonblocking readback completes in a `mainLoop`, natively and in the browser |
| `webTask` | a spawned task that sleeps runs between the frames of a `mainLoop`, natively and in the browser |
| `audioSession` | `packages/audio`'s session on an iPhone (`RAE_IOS_TEAM=<team> RAE_IOS_FOCUS_USAGE=<words> sh tools/ios/run-ios.sh checks/audioSession`, then read `Documents/rae.log` from the app's container): an ambient tone, an alert, 20 s of music with the session's events logged (unplug headphones, take a call, ask Siri meanwhile), the device's callbacks at each step, and the Focus status before and after asking (docs/audio-design.md §9.2). Off iOS it plays the same sounds and passes |
| `audioAac` | `packages/audio`'s AAC in M4A through the platform's decoder (AudioToolbox on Apple, AMediaCodec on Android: `sh tools/android/gen-android.sh checks/audioAac`, then `sh tools/android/run-android.sh audioAac`): fixture 1414's two files against afconvert's decodes (same frames, no delay, within -40 dB), seeks, a stream. Where there is no AAC decoder it passes with nothing to test |
| `audioInput` | `packages/audio`'s recording through a duplex device (the default input: an Apogee Duet, the built-in microphone): the permission, the input channels, the output and input latencies and their round trip, 2 s recorded (none lost, the frame count right, the level). `RAE_AUDIO_ECHO_TEST=1` plays four quiet clicks and finds them in the aligned recording; `RAE_AUDIO_ASK_MICROPHONE=1` asks for the permission. Without either an input or the permission it passes with nothing to test. On Android: `RAE_ANDROID_PERMISSIONS="RECORD_AUDIO" gen-android.sh checks/audioInput`, install, `adb shell pm grant com.rae.audioInput android.permission.RECORD_AUDIO`, then `RAE_ANDROID_INSTALL=0 run-android.sh audioInput` |
| `audioMidi` | `packages/audio`'s MIDI (CoreMIDI): the sources and destinations there are, and a loopback through the system's MIDI server (virtual ports): notes with running status, system exclusive, pitch bend, a controller, played on the synth with the music bus muted (`RAE_AUDIO_MIDI_LISTEN=1` unmutes, `RAE_CHECK_SECONDS` leaves time to play a keyboard). On Android (MidiManager, AMidi) there are no virtual ports: built with `RAE_ANDROID_PARTS="midiLoopback" gen-android.sh checks/audioMidi` the app has packages/audio's loopback device and the same five messages go round through it; without it the check lists the devices and listens to them |
| `audioNowPlaying` | `packages/audio`'s Now Playing, lock-screen buttons and background play (`RAE_IOS_TEAM=<team> RAE_IOS_BACKGROUND_AUDIO=1 sh tools/ios/run-ios.sh checks/audioNowPlaying`, then read `Documents/rae.log`): 100 s of a tune streamed with the `music` intent and shown in Now Playing; each button press and what it did, the app going to the background and back, and the place in the song every 5 s are logged. Passes when the song moved on in the background and the callbacks kept coming. Natively it runs 3 s (`RAE_CHECK_SECONDS`) and shows in the Mac's Now Playing. On Android (`gen-android.sh`, then `RAE_ANDROID_ENV="RAE_CHECK_SECONDS=40" run-android.sh audioNowPlaying 70`) it holds the audio focus and a MediaSession: send buttons with `adb shell cmd media_session dispatch play-pause` or `input keyevent KEYCODE_MEDIA_PLAY_PAUSE`, and see the focus with `dumpsys audio` |
| `audioSine` | `packages/audio` on the real default output: a sine, then one panned left to right, through the stage-A render thread; passes when the device ran, both voices ended and it played no silence for lack of samples (`RAE_REALTIME_CHECK=1` also fails it on an allocation in the render thread). With no output device it passes with nothing to test |
| `audioTap` | the output tap (audio/AudioTap, AudioSettings.outputTap) on the real default output: a tone for 2 s, the main thread taking the tapped output every 2 ms; passes when 2 s of output arrive over 2 s with no gap and nothing dropped (stage B, and A with RAE_AUDIO_STAGE=A); RAE_REALTIME_CHECK=1 fails it if the tap allocates or locks |

## Running

```sh
bash checks/run.sh                              # every check, natively
RAE_CHECK_FILTER="gpuArgs webTask" bash checks/run.sh
make -C compiler checks                         # the same
```

`make test-examples` (the visual examples gate) runs them too when it runs
every example. The two `web*` checks are also built for the browser and run in
headless Chrome by `compiler/tools/wasm_webgpu_smoke.sh`.

## Reference pictures

A check whose folder has a `reference.png` is also a picture test. `checks/run.sh`
runs it for 10 frames (`RAE_HEADLESS_FRAMES=10`) with a screenshot
(`RAE_GPU2D_SCREENSHOT`), and the check fails when more than 0.1% of the
screenshot's pixels differ from the reference by more than 8 levels
(`compiler/tools/assert_bmp_diff.py`). A screenshot of another size than the
reference (another display scale) is not compared. The references are in Git
LFS. After a change to how a chart is drawn, write them again and look at
each one before committing it:

```sh
bash checks/references.sh uiTimeline uiFlameHistogram uiLineTable
bash checks/references.sh                       # every check with a reference
```

A new check is a folder here with a `Main.rae` whose `main` returns 0 only
when it passed. The devtools page lists the checks in their own Checks tab.
