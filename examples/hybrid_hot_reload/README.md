# Hybrid Hot Reload Demo

This sample mirrors the workflow described in the hybrid packaging plan:

- `host.rae` simulates a compiled host application that embeds the Live VM and runs a short main loop.
- `scripts/hot_loader.rae` coordinates dev/release bundles and represents the glue between the C backend and VM runtime.
- `scripts/downloaded/*.rae` act as stand-ins for “downloaded” code that would arrive from a remote service.

Use `rae build --target hybrid --out build/hybrid_demo examples/hybrid_hot_reload/host.rae` (or the DevTools “Build artifacts” button) to observe both the compiled host stubs and VM chunks materialize under `compiled/` and `vm/`.

The generated `manifest.json` + `.vmchunk` files can be copied into the running host during development to simulate a download-driven hot-reload.

## Simulate Downloaded Bundles

Run `scripts/simulate_download.sh host.rae dev version1` (or `release version2`, etc.) from the Rae repo to build a fresh hybrid package, copy the VM artifacts into `.simulated_downloads/<profile>/<version>/`, and print the hashes that DevTools shows in the dashboard. The Example actions in DevTools invoke the same helper so reviewers can trigger “downloaded code” events without leaving the browser.

The `scripts/downloaded/version1|version2|version3` folders hold the staged patch scripts used by the helper. Each run copies the chosen version into `scripts/downloaded/dev_patch.rae` or `scripts/downloaded/release_patch.rae` before emitting the VM chunk, so you can cycle between versions and watch the hot-reload artifacts update over time.
