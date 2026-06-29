#!/bin/bash
# Rae example smoke tester - verifies all examples compile

set -e

BIN="bin/rae"
EXAMPLES_DIR="../examples"
PASSED=0
FAILED=0

echo "Running Rae example smoke tests..."
echo

if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found. Run 'make build' first."
  exit 1
fi

# Find all main.rae files in examples subdirectories
EXAMPLE_FILES=$(find "$EXAMPLES_DIR" -name "main.rae" | sort)

for EXAMPLE_FILE in $EXAMPLE_FILES; do
  EXAMPLE_NAME=$(basename "$(dirname "$EXAMPLE_FILE")")
  PROJECT_DIR=$(dirname "$EXAMPLE_FILE")

  # Compiled-target smoke only. The Live (bytecode VM) target is frozen /
  # unsupported (docs/live-vm-status.md, QUEUE #133/#134) and lib/ui now imports
  # gpu2d for its WebGPU backend — gpu2d externs have no VM binding, so a VM
  # compile of any lib/ui example would fail by design. Compiled is the
  # authoritative gate.
  if true; then
    # C Backend Smoke Test (generate + compile + link)
    TMP_OUT=$(mktemp -d)
    if "$BIN" build --target compiled --emit-c --project "$PROJECT_DIR" --out "$TMP_OUT/out.c" "$EXAMPLE_FILE" > "$TMP_OUT/emit.log" 2>&1; then
      # Attempt full compilation
      # Note: we include Raylib flags since many examples use it.
      # Link raylib statically so GLFW symbols bundled in libraylib.a
      # (glfwWaitEventsTimeout, glfwPostEmptyEvent, ...) resolve. The
      # shared libraylib.dylib does not export those.
      # Define + link the desktop backends so every example links regardless of
      # which it imports: raylib (statically, for the bundled GLFW symbols),
      # SDL3 (lib/sdl3.rae), and native WebGPU (lib/webgpu.rae, via wgpu-native).
      # All use distinct symbol names, so the runtime blocks compile together.
      # WebGPU is only added when wgpu-native is present (WGPU_NATIVE, default
      # ~/.local/wgpu-native), so the suite still runs without it (example 50
      # would then fail to link, others pass).
      WGPU="${WGPU_NATIVE:-$HOME/.local/wgpu-native}"
      WGPU_FLAGS=""
      [ -f "$WGPU/lib/libwgpu_native.dylib" ] && WGPU_FLAGS="-DRAE_HAS_WEBGPU -I$WGPU/include -L$WGPU/lib -lwgpu_native -Wl,-rpath,$WGPU/lib -framework Metal -framework QuartzCore -framework Foundation -framework ImageIO -framework CoreGraphics"
      if gcc -O2 -o "$TMP_OUT/app" "$TMP_OUT/out.c" "$TMP_OUT/rae_runtime.c" \
         $([ -f "$TMP_OUT/monocypher.c" ] && echo "$TMP_OUT/monocypher.c") \
         $(ls "$PROJECT_DIR"/*.c 2>/dev/null | grep -v "rae_runtime.c" | grep -v "main_compiled.c" || true) \
         -I"$TMP_OUT" -I/opt/homebrew/include -L/opt/homebrew/lib -DRAE_HAS_RAYLIB -DRAE_HAS_SDL3 $WGPU_FLAGS \
         /opt/homebrew/lib/libraylib.a -lSDL3 -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL -framework ImageIO -framework CoreGraphics > "$TMP_OUT/link.log" 2>&1; then
        echo "PASS: $EXAMPLE_NAME"
        ((PASSED++))
      else
        echo "FAIL: $EXAMPLE_NAME (C linking)"
        cat "$TMP_OUT/link.log" | sed 's/^/  /'
        ((FAILED++))
      fi
    else
      echo "FAIL: $EXAMPLE_NAME (C backend emit)"
      cat "$TMP_OUT/emit.log" | sed 's/^/  /'
      ((FAILED++))
    fi
    rm -rf "$TMP_OUT"
  else
    echo "FAIL: $EXAMPLE_NAME (VM compiler)"
    ((FAILED++))
  fi
done

echo
echo "=========================================="
echo "Results: $PASSED passed, $FAILED failed"
echo "=========================================="

if [ $FAILED -gt 0 ]; then
  exit 1
fi
