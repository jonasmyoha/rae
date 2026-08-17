.PHONY: all dev stop wasm

DEV_CMD=bun run scripts/dev.ts

all: dev

dev:
	@echo "Starting Rae devtools server..."
	$(DEV_CMD)

stop:
	@bun run scripts/stop.ts

# Build the Rae raytracer to WebAssembly for the live demo in the Raytracer
# view (served at /wasm/raytracer.wasm). Needs wasi-sdk (set WASI_SDK).
wasm:
	@echo "Building Rae raytracer -> WASM..."
	@../rae/compiler/tools/wasm_build.sh examples/46_raytracer_wasm_web
