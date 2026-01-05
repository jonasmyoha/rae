# Hybrid Hot Reload Demo

This sample mirrors the workflow described in the hybrid packaging plan:

- `host.rae` simulates a compiled host application that embeds the Live VM.
- `scripts/hot_loader.rae` coordinates dev/release bundles and represents the glue between the C backend and VM runtime.
- `scripts/downloaded/*.rae` act as stand-ins for “downloaded” code that would arrive from a remote service.

Use `rae build --target hybrid --out build/hybrid_demo examples/hybrid_hot_reload/host.rae` (or the DevTools “Build artifacts” button) to observe both the compiled host stubs and VM chunks materialize under `compiled/` and `vm/`.

The generated `manifest.json` + `.vmchunk` files can be copied into the running host during development to simulate a download-driven hot-reload.
