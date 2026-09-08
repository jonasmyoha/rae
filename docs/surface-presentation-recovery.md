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

## Verification

Run separately, through the shared test log:

```sh
MAKEFLAGS='TEST_RUNNER=tools/test_present_recovery.sh' perl -e 'alarm shift; exec @ARGV' 120 bash compiler/tools/watch-tests.sh
MAKEFLAGS='TEST_RUNNER=tools/run_examples.sh' RAE_EXAMPLE_FILTER='118_water_lake' perl -e 'alarm shift; exec @ARGV' 300 bash compiler/tools/watch-tests.sh
```

The C test compiles the production recovery section with mocked SDL/configuration
calls. It checks repeated Occluded/Timeout, successful recovery, drawable-size
configuration, bounded retries, and zero-size protection. It does not simulate
the actual window server. The 118 screenshot gate reads the offscreen texture,
so passing it does not prove that presentation to the window succeeded.

Manual window-server check: run 118 normally, minimize and restore it, cover and
uncover it, and switch Spaces/back. The image should resume without restarting
the app. A native occlusion report must not trigger repeated reconfiguration.
If a foreground window stays black, capture the new status logs and whether
moving/resizing the window restores it. Rebuild watch applications to pick up
the runtime change; already running binaries retain the old recovery code.
