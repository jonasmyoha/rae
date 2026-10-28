# forwardMsaa: forward + 4x MSAA vs deferred, one scene

The measured prototype of `docs/forward-msaa-study.md` (render-perf-14). It is a
study, not a renderer: one synthetic scene (instanced spheres over a ground, seen
from a low camera) drawn by six main-pass designs over the raw WebGPU bindings.
Every variant runs the SAME lighting function (`scene.wgsl`: one sun with GGX
specular and a hemisphere ambient) and ends in the same 8-bit image, so the
variants differ only in where the lighting runs and how the samples are stored.

| variant | passes | attachments |
|---|---|---|
| deferred | G-buffer (3 targets + depth, stored), fullscreen lighting | 20 B/pixel |
| forward 1x, depth pre-pass | one pass: depth draws, then shading draws (depth Equal) | 8 B/pixel |
| forward 1x | one pass, shading draws (depth Less) | 8 B/pixel |
| forward 4x MSAA, depth pre-pass, memoryless | as above at 4x, resolved on store | 4 B/pixel |
| forward 4x MSAA, memoryless | one 4x pass, resolved on store | 4 B/pixel |
| forward 4x MSAA, stored samples | the same with ordinary (not memoryless) 4x targets | 36 B/pixel |

"Memoryless" is wgpu's `TRANSIENT` texture usage: the 4x color and depth exist
only in tile memory. wgpu-native 29 passes usage bits to wgpu-core unchanged, so
the benchmark uses wgpu-core's bit (`1 << 17`); the header's
`WGPUTextureUsage_TransientAttachment` (0x20) panics in `wgpuDeviceCreateTexture`.

## Running

```sh
sh benchmarks/forwardMsaa/run.sh              # the Mac table of the study
FM_IMAGES=/tmp/fm sh benchmarks/forwardMsaa/run.sh   # also one PNG per variant
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  sh tools/ios/run-ios.sh benchmarks/forwardMsaa   # the iPhone (bundle com.rae.forwardMsaa)
```

Settings (environment): `RAE_FM_SIZE=WxH` (1920x1080), `RAE_FM_SIDE` (spheres
per grid side, 30), `RAE_FM_FRAMES` (timed frames per variant, 200),
`RAE_FM_BATCH` (frames per submit, 8), `RAE_FM_TIMESTAMPS=0` (clock only),
`RAE_FM_IMAGES=<dir>` (`documents` = the app's Documents folder, for the
iPhone: copy the PNGs back with `xcrun devicectl device copy from`).

The benchmark draws only into its own textures and never to the window, so
the app's screen stays black while it runs; save the PNGs to see what it drew.

The variants take turns over 5 rounds after a warm-up, so a GPU clock change
affects all of them. The reported "clock" is the time of one submit of
`RAE_FM_BATCH` frames, from recording to the GPU finishing it, divided by the
frames: the throughput, with consecutive frames overlapping on the GPU as they do
in a running app. Use a batch large enough that the fixed cost of a submit and
wait (about 2.8 ms on the Mac) does not set the result: 64 on the Mac, 32 on the
iPhone. The GPU timestamps time only the last frame of a batch and are not used
in the study: on the Mac they include the overlap with the frame before, and on
the iPhone they come back with the end before the beginning.
