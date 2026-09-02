#!/bin/bash
# Rae example smoke tester - verifies all examples compile

set -e

BIN="bin/rae"
EXAMPLES_DIR="../examples"
PASSED=0
FAILED=0

echo "Running Rae example smoke tests..."
echo

# Legacy examples are retained as source archaeology, not supported build
# targets. Guard the active tree before compiling so a new Raylib dependency
# cannot slip in while the explicitly allowlisted ports are being migrated.
./tools/check_active_example_raylib.sh

if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found. Run 'make build' first."
  exit 1
fi

# Find supported examples only. Devtools already hides examples/legacy; the
# compiler gate must use the same definition of the active example surface.
EXAMPLE_FILES=$(find "$EXAMPLES_DIR" -path "$EXAMPLES_DIR/legacy" -prune -o -name "main.rae" -print | sort)

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
      # Attempt full compilation. Define + link the active desktop backends so
      # every supported example links regardless of which it imports: SDL3
      # (lib/sdl3.rae) and native WebGPU (lib/webgpu.rae, via wgpu-native).
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
         -I"$TMP_OUT" -I/opt/homebrew/include -L/opt/homebrew/lib -DRAE_HAS_SDL3 $WGPU_FLAGS \
         -lSDL3 -framework Foundation -framework ImageIO -framework CoreGraphics > "$TMP_OUT/link.log" 2>&1; then
        if [ "$EXAMPLE_NAME" = "91_pong_implicit" ]; then
          SCREENSHOT="$TMP_OUT/pong.bmp"
          if RAE_PONG_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=800 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && grep -q '\[pong\] self-test passed' "$TMP_OUT/render.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (physics self-test + GPU2D/.raescene screenshot)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (Pong logic/render gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "94_tetris2d" ]; then
          SCREENSHOT="$TMP_OUT/tetris2d.bmp"
          if RAE_TETRIS2D_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=900 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && grep -q '\[tetris2d\] deterministic frame ready' "$TMP_OUT/render.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (GPU2D board + .raescene HUD screenshot)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (Tetris 2D render gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "95_easing_2d" ]; then
          SCREENSHOT="$TMP_OUT/easing2d.bmp"
          if RAE_EASING_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=800 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (fixed easing frame + .raescene overlay)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (easing screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "96_easing_3d" ]; then
          SCREENSHOT="$TMP_OUT/easing3d.bmp"
          if RAE_EASING3D_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 25 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && grep -q '\[easing3d\] deterministic deferred frame rendered' "$TMP_OUT/render.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (deferred easing + .raescene overlay)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (deferred easing render gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "97_tetris3d" ]; then
          SCREENSHOT="$TMP_OUT/tetris3d.bmp"
          if RAE_TETRIS3D_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=1400 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 25 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && grep -q '\[tetris3d\] deterministic deferred frame rendered' "$TMP_OUT/render.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (deferred ECS board + .raescene HUD)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (Tetris 3D render gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "109_gpu3d_pbr" ]; then
          # 109 was migrated from the forward renderer to the DEFERRED path
          # (#514). The forward draw-limit guard (RAE_GPU3D_DRAW_LIMIT, via
          # rae_g3d_push_draw_record) therefore no longer fires here — deferred
          # draws never take that path — so this gate verifies the deferred PBR
          # frame renders (non-blank screenshot). The runtime guard still exists
          # for the forward path but has lost its example test host; re-homing it
          # onto a forward multi-mesh example is tracked in QUEUE #759.
          SCREENSHOT="$TMP_OUT/gpu3d.bmp"
          if RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app" > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (non-blank deferred PBR screenshot)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (3D screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "114_walker_character" ]; then
          # The multi-primitive gate, plus the skinning and colour chain.
          # 6717 verts / 3465 triangles is every primitive of all 10 meshes
          # merged; loading only the first would report a fraction of that
          # and still render something plausible, which is exactly why the
          # counts are asserted rather than just "did it draw". The
          # skeleton, clip and atlas lines each cover a stage that fails
          # silently on its own: 65 joints (skin parsed), 195 channels
          # retargeted onto it (animation), 512x512 (the pure-Rae PNG
          # decode that feeds per-vertex colour).
          SHOT="$TMP_OUT/walker.bmp"
          if (cd .. && RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SHOT" \
                perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SHOT" --min-colors=20 > "$TMP_OUT/shot.log" 2>&1 \
             && [ "$(grep -c "walker: 6717 verts, 3465 triangles" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "skeleton: 65 joints, 76 nodes" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "walk 195 ch" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "palette atlas 512x512" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "skinned parts: 12" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "panel buttons: 4 clip, 0 transport" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "\[shadow\] casters 130, 3 cascades" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "\[seamtest\] .* netOk=true physicsOk=true" "$TMP_OUT/render.log")" -eq 1 ]; then
            echo "PASS: $EXAMPLE_NAME (12 skinned parts + ground casting shadows, clip retargeted, atlas decoded)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (walker character)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/shot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "113_gltf_character" ]; then
          # The glTF loader gate. Asserts the COUNTS it reports, not just
          # that something rendered: a loader reading half a buffer still
          # draws a plausible blob, and counts are the cheapest thing that
          # cannot be faked by accident. 1728 verts / 576 triangles is the
          # Khronos Fox — non-indexed, hence exactly verts/3 triangles.
          SHOT="$TMP_OUT/gltf-character.bmp"
          if (cd .. && RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SHOT" \
                perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SHOT" --min-colors=20 > "$TMP_OUT/shot.log" 2>&1 \
             && [ "$(grep -c "character: 1728 verts, 576 triangles" "$TMP_OUT/render.log")" -eq 1 ]; then
            echo "PASS: $EXAMPLE_NAME (glb loaded: 1728 verts / 576 tris, non-blank render)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (glTF character)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/shot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "110_deferred_gbuffer" ]; then
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
          # since nothing reads the pyramid yet. Every declared pass now
          # executes (renderer_deferred.rae), so the pyramid's presence in
          # the derived pass order (`3: depthPyramid`) is the built signal —
          # the old per-build `depth pyramid: N mips` debug log was removed.
          if [ "$GB_OK" = "1" ] \
             && [ "$(grep -c '  0: shadow' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  1: gbuffer' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  2: ssao' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  3: depthPyramid' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  5: taa' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  7: present' "$TMP_OUT/gb-lit.log")" -ge 1 ]; then
            echo "PASS: $EXAMPLE_NAME (lit frame + 4 G-buffer channels, pyramid built, derived pass order)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (deferred G-buffer)"
            cat "$TMP_OUT"/gb-*.log 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "111_metaballs_forward" ]; then
          # The forward twin: one screenshot is enough. 112 owns the deep
          # coverage (scene switching, pause, settings, free camera); repeating
          # all of it here would double the slowest example in the suite to
          # re-test the UI rather than the renderer, which is the only thing
          # that differs.
          SCREENSHOT="$TMP_OUT/gpu3d-ui-forward.bmp"
          if (cd .. && RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --gpu3d-ui > "$TMP_OUT/screenshot.log" 2>&1 \
             && [ "$(grep -c '\[shadow\] casters 17, 3 cascades' "$TMP_OUT/render.log")" -eq 1 ]; then
            echo "PASS: $EXAMPLE_NAME (forward path, lit frame + shadow cascades)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (forward 3D/UI screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "112_metaballs_deferred" ]; then
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
             && [ "$(grep -c '\[shadow\] casters 17, 3 cascades' "$TMP_OUT/render.log")" -eq 1 ] \
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
