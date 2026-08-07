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
        if [ "$EXAMPLE_NAME" = "109_gpu3d_pbr" ]; then
          SCREENSHOT="$TMP_OUT/gpu3d.bmp"
          if RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" > "$TMP_OUT/screenshot.log" 2>&1 \
             && RAE_SDL_HEADLESS_MS=250 RAE_GPU3D_DRAW_LIMIT=4 \
                perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app" > "$TMP_OUT/overflow.log" 2>&1 \
             && [ "$(grep -c '\[gpu3d\] ERROR: draw limit exceeded: configured=4 hardMaximum=4096' "$TMP_OUT/overflow.log")" -eq 1 ]; then
            echo "PASS: $EXAMPLE_NAME (non-blank 3D screenshot, loud draw-limit guard)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (3D screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" "$TMP_OUT/overflow.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "111_deferred_gbuffer" ]; then
          # The deferred G-buffer (#356). Each channel is checked separately
          # because they fail differently: a broken albedo write still leaves
          # plausible normals, and a broken normal transform still leaves
          # correct albedo. One composite screenshot would hide either.
          #
          # Albedo and material are UNSHADED, so they legitimately contain
          # about as many colours as the scene has materials (25 spheres +
          # ground + clear = 27) — the default 32-colour floor assumes a lit
          # image and would fail them for being correct. --min-colors=20
          # states the real expectation. Normals and linearised depth are
          # continuous and pass the standard check.
          GB_OK=1
          for GB_VIEW in lit albedo normal material depth; do
            GB_SHOT="$TMP_OUT/gbuffer-$GB_VIEW.bmp"
            case "$GB_VIEW" in
              albedo|material) GB_ARGS="--min-colors=20" ;;
              *)               GB_ARGS="" ;;
            esac
            if ! (RAE_GBUFFER_VIEW="$GB_VIEW" RAE_GBUFFER_DEBUG=1 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$GB_SHOT" \
                  perl -e 'alarm shift; exec @ARGV' 25 "$TMP_OUT/app" > "$TMP_OUT/gb-$GB_VIEW.log" 2>&1 \
                  && python3 tools/assert_nonblank_bmp.py "$GB_SHOT" $GB_ARGS >> "$TMP_OUT/gb-$GB_VIEW.log" 2>&1); then
              GB_OK=0
            fi
          done
          # Order is DERIVED from the graph's resource edges, never written,
          # and the pyramid must actually have been built — a silently
          # skipped pass would still leave a correct-looking lit image,
          # since nothing reads the pyramid yet.
          if [ "$GB_OK" = "1" ] \
             && [ "$(grep -c '  0: gbuffer' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  4: present' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c 'depth pyramid: .* mips' "$TMP_OUT/gb-lit.log")" -ge 1 ]; then
            echo "PASS: $EXAMPLE_NAME (lit frame + 4 G-buffer channels, pyramid built, derived pass order)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (deferred G-buffer)"
            cat "$TMP_OUT"/gb-*.log 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "110_gpu3d_ui" ]; then
          SCREENSHOT="$TMP_OUT/gpu3d-ui.bmp"
          FREE_SCREENSHOT="$TMP_OUT/gpu3d-ui-free.bmp"
          PAUSE_SCREENSHOT="$TMP_OUT/gpu3d-ui-pause.bmp"
          SETTINGS_SCREENSHOT="$TMP_OUT/gpu3d-ui-settings.bmp"
          SCENE2_SCREENSHOT="$TMP_OUT/gpu3d-ui-scene2.bmp"
          SCENE3_SCREENSHOT="$TMP_OUT/gpu3d-ui-scene3.bmp"
          if (cd .. && RAE_GPU3D_SDF_TEST_LOG=1 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --gpu3d-ui > "$TMP_OUT/screenshot.log" 2>&1 \
             && [ "$(grep -c '\[gpu3d\] SDF metaballs: count=5' "$TMP_OUT/render.log")" -eq 1 ] \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=free RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$FREE_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/free-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$FREE_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/free-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=pause RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$PAUSE_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/pause-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$PAUSE_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/pause-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=debug RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SETTINGS_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/settings-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SETTINGS_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/settings-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=scene2 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCENE2_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/scene2-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCENE2_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/scene2-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=scene3 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCENE3_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/scene3-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCENE3_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/scene3-screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (3D scenes + camera, settings and pause UI screenshots)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (3D/UI screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" "$TMP_OUT/free-render.log" "$TMP_OUT/free-screenshot.log" "$TMP_OUT/pause-render.log" "$TMP_OUT/pause-screenshot.log" "$TMP_OUT/settings-render.log" "$TMP_OUT/settings-screenshot.log" "$TMP_OUT/scene2-render.log" "$TMP_OUT/scene2-screenshot.log" "$TMP_OUT/scene3-render.log" "$TMP_OUT/scene3-screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        else
          echo "PASS: $EXAMPLE_NAME"
          ((PASSED++))
        fi
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
