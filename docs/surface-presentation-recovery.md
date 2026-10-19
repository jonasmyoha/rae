# Surface presentation recovery

The repeated `unknown(196609)` diagnostic was a misclassified wgpu-native
status: `196609 == 0x00030001 == Occluded`. It is a native extension, absent
from the standard WebGPU enum. This is not evidence of a header/library ABI
mismatch. The installed native header and generated Rae bindings define it.

[wgpu-native's contract](https://github.com/gfx-rs/wgpu-native/blob/trunk/ffi/wgpu.h)
says this status leaves the surface valid; retry when the window is visible.
Reconfiguring every frame was incorrect. Timeout also does not imply a broken
surface. Both now retry without configuration, report once after three skipped
frames, and reset the streak after a successful presentation. Actual surface
errors retain bounded configuration retries (first three, then every 120 skips).
The 3D path now honours the same SDL visibility guard as 2D. Neither path forces
a minimized/background window forward, and normal event polling continues.

This fixes the confirmed recovery-policy bug, not every possible black screen.
In particular, persistent native Occluded while a window is visibly foregrounded
still needs a native visibility/event trace; do not assume the surface is lost
or recreate the device merely because no frame was presented.

## Black window at startup: wgpu's occlusion check (2026-10-19)

The "persistent native Occluded while a window is visibly foregrounded" case
above was about half of all Rae window starts on macOS: the window stayed
black, logging `[present] Occluded(196609)`, until another window covered it
FULLY and moved away again (a partial cover did not help).

Cause: wgpu 29's Metal backend (our wgpu-native v29.0.1.1; wgpu PR #9141, the
fix for issue #8309's one-second `nextDrawable` stall) reads
`NSWindow.occlusionState` before every acquire and returns Occluded, with no
drawable, while its visible bit is clear. That bit is clear for a window that
was just created: measured with a per-frame trace, frame 1 always reads "not
visible" (the app is not yet active) and frame 2 "visible" once it is. When
the bit stays clear (most likely the app not being activated, e.g. started by
`rae watch` while another app is in front), wgpu refuses every frame, while
SDL's own `SDL_WINDOW_OCCLUDED` correctly says visible: SDL follows AppKit's
occlusion NOTIFICATIONS, and none fires for a state that never changed. A full
cover and uncover is a real change, so only that brought the window back.
Upstream: wgpu issue #9430 ("First frame does not render due to being somehow
occluded"), fix PR #10302 (open), and issue #10627 (the check also runs off
the main thread).

Fix (`rae_presentation_layer`, runtime_platform_apple.c): on macOS the surface
presents into its own CAMetalLayer, a sublayer filling SDL's view layer, whose
delegate answers `window` with nil. wgpu finds the window by asking the first
layer delegate above the surface for it, so its check is skipped. The stall it
guarded against stays guarded: the present never acquires while SDL reports
the window hidden, minimized or occluded. Measured: the first frame now
presents on frame 1 instead of waiting for the flag; window captures show the
frame, also after the window is created at one size and resized to the saved
one. `RAE_WGPU_OCCLUSION_CHECK=1` restores the old path for comparison. Drop
the sublayer once a wgpu-native release contains PR #10302 and the black start
no longer reproduces without it.

## Verification

The policy is Rae since render-perf-11b (`lib/gpu/SurfaceRecovery.rae`, used
by `lib/gpu/Surface.rae`; C keeps the one-call `rae_surface_reconfigure`, which
also skips a zero window size). Fixture `1128_surface_recovery` checks repeated
Occluded/Timeout (no reconfigure), the streak reset after a presented frame,
and the bounded retries of a real error; it runs in the normal suite. It does
not simulate the actual window server:

```sh
MAKEFLAGS='TEST_RUNNER=tools/run_examples.sh' RAE_EXAMPLE_FILTER='140_water_lake' perl -e 'alarm shift; exec @ARGV' 300 bash compiler/tools/watch-tests.sh
```

The 118 screenshot gate reads the offscreen texture (a screenshot run never
draws into the drawable), so passing it does not prove that presentation to
the window succeeded.

Manual window-server check: run 118 normally, minimize and restore it, cover and
uncover it, and switch Spaces/back. The image should resume without restarting
the app. A native occlusion report must not trigger repeated reconfiguration.
If a foreground window stays black, capture the new status logs and whether
moving/resizing the window restores it. Rebuild watch applications to pick up
the runtime change; already running binaries retain the old recovery code.
