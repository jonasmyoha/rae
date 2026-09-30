#!/bin/bash
# Rae example smoke tester - verifies all examples compile

set -e

BIN="bin/rae"
# #919: the example gate runs the build's format preflight in CHECK mode — an
# unformatted example fails its gate instead of being rewritten by the gate.
export RAE_FORMAT=check
# Declared shaders are validated with naga at build time; the gate never skips
# the check silently (docs/shaders-and-the-compiler.md).
export RAE_SHADER_VALIDATE="${RAE_SHADER_VALIDATE:-require}"
# #984: the gate below runs each example's STANDALONE built binary (not
# `bin/rae`), so nothing sets $RAE_STDLIB or cwd for it automatically — a
# gate that execs "$TMP_OUT/app" without both stalls on every stdlib asset
# read (`lib/gpu2d_box.wgsl`, a `.raescene`, ...): "could not read stdlib
# asset ..., and $RAE_STDLIB is unset". `rae run` gives its child both (cwd =
# repo root, RAE_STDLIB = the toolchain lib/); every gate below that execs
# the app must do the same, either via `(cd .. && RAE_STDLIB=... ...)` or by
# relying on this exported default.
export RAE_STDLIB="$(cd .. && pwd)/lib"
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
EXAMPLE_FILES=$(find "$EXAMPLES_DIR" -path "$EXAMPLES_DIR/legacy" -prune -o \( -name "Main.rae" -o -name "main.rae" \) -print | sort)

for EXAMPLE_FILE in $EXAMPLE_FILES; do
  EXAMPLE_NAME=$(basename "$(dirname "$EXAMPLE_FILE")")
  # Exact directory names, space-separated: narrow GPU gates without running
  # every example. An unset filter preserves the full-suite behaviour.
  if [ -n "${RAE_EXAMPLE_FILTER:-}" ]; then
    case " $RAE_EXAMPLE_FILTER " in
      *" $EXAMPLE_NAME "*) ;;
      *) continue ;;
    esac
  fi
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
          if (cd .. && RAE_PONG_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=800 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
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
          if (cd .. && RAE_TETRIS2D_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=900 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
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
          if (cd .. && RAE_EASING_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=800 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
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
          if (cd .. && RAE_EASING3D_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
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
          if (cd .. && RAE_TETRIS3D_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=1400 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
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
          if (cd .. && RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (non-blank deferred PBR screenshot)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (3D screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "117_transparent_pass" ]; then
          # The transparent forward pass in isolation (#845): the log line
          # proves the pass ran with instances queued on every rendered frame;
          # the non-blank shot proves a frame came out. Whether the cubes
          # actually BLEND and are depth-occluded is a human check on hardware
          # (see the example header) — a non-blank BMP cannot tell.
          # Run from the repo root (cd ..) like 112/114: the pass reads
          # lib/transparentForward.wgsl relative to the working directory.
          SCREENSHOT="$TMP_OUT/transparent.bmp"
          if (cd .. && RAE_TRANSPARENT_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && grep -qE '\[transparent\] deterministic deferred frame rendered: 10 alpha instances over 4 opaque, [1-9][0-9]* transparent frames' "$TMP_OUT/render.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (transparentForward: alpha row over a lit deferred scene)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (transparent pass render gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "121_ui_editor" ]; then
          # The .raescene viewer (#997): each shipped sample opens headlessly
          # under the RAE_UI_EDITOR_SCENE fallback, mounts with ZERO
          # diagnostics (every component, token, sub-scene and texture the
          # samples use resolved) and renders a non-blank frame.
          UI_EDITOR_OK=1
          # Coverage (#1000) authors EVERY registered component with every
          # field; its 0-diagnostics line is the authoring-coverage gate. Each
          # sample is also compared with its checked-in reference PNG
          # (examples/121_ui_editor/references, `make -C examples/121_ui_editor
          # references` to regenerate) — tolerant, and skipped when the
          # machine renders at another DPR than the reference.
          # SelfContained and split/Page (#1008) are the two `import`/`assets`
          # shapes: one file with everything inline, and a page importing
          # `split/Theme` + `split/Assets` by package path from the samples
          # root (the scene root is passed explicitly, as `--scene-root` would).
          for SAMPLE in MainMenu Settings CardStrip Coverage SelfContained split/Page; do
            NAME="$(basename "$SAMPLE")"
            SCREENSHOT="$TMP_OUT/ui-editor-$NAME.bmp"
            # Coverage animates (#1005): an exact frame at a fixed step keeps
            # its reference deterministic; the static samples use wall-clock.
            SAMPLE_BUDGET="RAE_SDL_HEADLESS_MS=1500"
            if [ "$SAMPLE" = "Coverage" ]; then SAMPLE_BUDGET="RAE_HEADLESS_FRAMES=12 RAE_FIXED_DT=0.05"; fi
            if (cd .. && env RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/$SAMPLE.raescene" \
               RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
               RAE_UI_HEADLESS=1 $SAMPLE_BUDGET RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
               perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-$NAME.log" 2>&1 \
               && grep -qE "\[ui-editor\] mounted $NAME: [1-9][0-9]* nodes, 0 diagnostics" "$TMP_OUT/render-$NAME.log" \
               && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                  > "$TMP_OUT/screenshot-$NAME.log" 2>&1 \
               && python3 tools/assert_bmp_diff.py "$SCREENSHOT" "../examples/121_ui_editor/references/$NAME.png" \
                  --skip-size-mismatch >> "$TMP_OUT/screenshot-$NAME.log" 2>&1; then
              :
            else
              UI_EDITOR_OK=0
              echo "  sample $SAMPLE failed:"
              cat "$TMP_OUT/render-$NAME.log" "$TMP_OUT/screenshot-$NAME.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
            fi
          done
          # NestedPage (#1011) is page -> sub-scene -> sub-sub-scene. The
          # registry must contain both descendants before the first resolve;
          # otherwise report policy consumes the child marker as missing.
          SCREENSHOT="$TMP_OUT/ui-editor-NestedPage.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/NestedPage.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1500 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="ParentMount" RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-NestedPage.log" 2>&1 \
             && grep -qE '\[ui-editor\] mounted NestedPage: [1-9][0-9]* nodes, 0 diagnostics' "$TMP_OUT/render-NestedPage.log" \
             && grep -qF 'ParentMount - Scene  nested/Parent' "$TMP_OUT/render-NestedPage.log" \
             && grep -qF 'ChildRoot - Scene  nested/Child' "$TMP_OUT/render-NestedPage.log" \
             && grep -qF 'gap=24 · from spaceM' "$TMP_OUT/render-NestedPage.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot-NestedPage.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  nested sub-scene sample failed:"
            cat "$TMP_OUT/render-NestedPage.log" "$TMP_OUT/screenshot-NestedPage.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # Hover leak (#32786425, #13186639): 1500 painted frames, each a
          # canvas hover change — RAE_GPU2D_TEST_POINTER moves the pointer
          # between Title and OrbitPlanet every poll, so the REAL frame loop
          # runs hit-test, hover outline, hierarchy refresh and paint. At exit
          # (RAE_MEM_STATS=1) the outstanding allocations must be the boot
          # baseline (8 Strings, 1 buffer today), not a per-hover count:
          # any leak of even one String per hover adds 1500. A MB threshold
          # was too coarse — the hierarchy-refresh String leak (~0.7 KB per
          # hover) passed the old 4 MB check. The move line proves the
          # pointer really moved 1500 times.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
             RAE_UI_HEADLESS=1 RAE_HEADLESS_FRAMES=1500 RAE_FIXED_DT=0.05 RAE_MEM_STATS=1 \
             RAE_GPU2D_TEST_POINTER="623,196,787,297,1" \
             perl -e 'alarm shift; exec @ARGV' 90 "$TMP_OUT/app") > "$TMP_OUT/render-hover-leak.log" 2>&1 \
             && grep -aqF '[gpu2d-test-pointer] 1500 moves' "$TMP_OUT/render-hover-leak.log" \
             && awk '
                  function out(line) { sub(/.*outstanding=/, "", line); sub(/ .*/, "", line); return line + 0 }
                  /\[mem:string:TOTAL/ { total = out($0); seen++ }
                  /\[mem:string:sub /  { substrings = out($0) }
                  /\[mem:string:interp/ { interps = out($0) }
                  /\[mem:buf /         { buffers = out($0); seen++ }
                  END { exit !(seen == 2 && total < 100 && substrings < 100 && interps < 100 && buffers < 50) }
                ' "$TMP_OUT/render-hover-leak.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hover leak check failed (1500 painted canvas hovers; outstanding must stay at the boot baseline):"
            grep -a 'test-pointer\|mem:string:TOTAL\|mem:string:sub \|mem:string:interp\|mem:buf ' "$TMP_OUT/render-hover-leak.log" 2>/dev/null | tail -5 | sed 's/^/    /'
          fi
          # Hover glow (Shadow.hoverOpacity + the ButtonSystem fade): hovering
          # the Open pill once must brighten its glow — the frame differs from
          # the un-hovered MainMenu shot above, and only by the glow.
          HOVERED="$TMP_OUT/ui-editor-MainMenu-hover.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_UI_EDITOR_TEST_HOVER=OpenPill \
             RAE_GPU2D_SCREENSHOT="$HOVERED" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-hover.log" 2>&1 \
             && grep -qF '[ui-editor] hover OpenPill' "$TMP_OUT/render-hover.log" \
             && ! python3 tools/assert_bmp_diff.py "$TMP_OUT/ui-editor-MainMenu.bmp" "$HOVERED" --max-diff-pct 0.02 \
                > "$TMP_OUT/hover-diff.log" 2>&1 \
             && python3 tools/assert_bmp_diff.py "$TMP_OUT/ui-editor-MainMenu.bmp" "$HOVERED" --max-diff-pct 1.0 \
                >> "$TMP_OUT/hover-diff.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hover glow check failed (Open pill hovered vs rest):"
            cat "$TMP_OUT/render-hover.log" "$TMP_OUT/hover-diff.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # Wheel routing: one notch reaches ONE consumer (lib routeUiWheel +
          # WheelTarget on the EditArea). Over the hierarchy it scrolls the tree
          # and must NOT zoom the canvas; over the canvas it zooms and scrolls
          # nothing; over the component list it scrolls only that list.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=800 RAE_UI_EDITOR_TEST_SELECT=Title \
             RAE_UI_EDITOR_TEST_WHEEL="TreePanel:-3 EditArea:2 InsWidgets:-1" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-Wheel.log" 2>&1 \
             && grep -qF 'wheel TreePanel -3: routed TreeList zoom 38->38 tree 0->-144 components 0->0' "$TMP_OUT/render-Wheel.log" \
             && grep -qF 'wheel EditArea 2: routed EditArea zoom 38->46 tree -144->-144 components 0->0' "$TMP_OUT/render-Wheel.log" \
             && grep -qF 'wheel InsWidgets -1: routed ComponentList zoom 46->46 tree -144->-144 components 0->-48' "$TMP_OUT/render-Wheel.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  wheel routing failed:"
            grep -a '\] wheel' "$TMP_OUT/render-Wheel.log" 2>/dev/null | sed 's/^/    /'
          fi
          # The effect systems (#1005): Coverage's AnimFrames / WobbleFx /
          # BackgroundPan+SmokeFx / Carousel nodes must have MOVED between frame
          # 2 and frame 12 at the same fixed step (assert_bmp_diff MISMATCH is
          # the pass here: more than 0.1% of pixels differ — the document is a
          # sixth of the desktop window now).
          EARLY="$TMP_OUT/ui-editor-Coverage-f2.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/Coverage.raescene" \
             RAE_UI_HEADLESS=1 RAE_HEADLESS_FRAMES=2 RAE_FIXED_DT=0.05 RAE_GPU2D_SCREENSHOT="$EARLY" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-Coverage-f2.log" 2>&1 \
             && ! python3 tools/assert_bmp_diff.py "$TMP_OUT/ui-editor-Coverage.bmp" "$EARLY" --max-diff-pct 0.1 \
                > "$TMP_OUT/motion-Coverage.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  effect motion check failed (Coverage frame 2 vs 12 did not move):"
            cat "$TMP_OUT/render-Coverage-f2.log" "$TMP_OUT/motion-Coverage.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The watcher (#998): the app opens a COPY of the samples (the repo
          # files stay untouched), rewrites it itself after 600 ms with the
          # .rewrite twin (one changed Text, one component no lib table has —
          # an app's own, KEPT as authored, not an error), and must log
          # the reload with 0 diagnostics + 1 kept and still render a frame.
          WATCH_DIR="$TMP_OUT/watch-samples"
          cp -r ../examples/121_ui_editor/assets/samples "$WATCH_DIR"
          SCREENSHOT="$TMP_OUT/ui-editor-reload.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="$WATCH_DIR/MainMenu.raescene" RAE_UI_EDITOR_TEST_REWRITE=600 \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=2200 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-reload.log" 2>&1 \
             && grep -qE "\[ui-editor\] mounted MainMenu: [1-9][0-9]* nodes, 0 diagnostics" "$TMP_OUT/render-reload.log" \
             && grep -qE "\[ui-editor\] reloaded .*MainMenu.raescene \(\+[1-9][0-9]* -[1-9][0-9]* nodes, 0 diagnostics, 1 app components kept\)" "$TMP_OUT/render-reload.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot-reload.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  watch/reload failed:"
            cat "$TMP_OUT/render-reload.log" "$TMP_OUT/screenshot-reload.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The inspector (#999): RAE_UI_EDITOR_TEST_SELECT selects a document
          # node at boot as a click would; the log names its components and
          # the frame carries the outline + panels (tree open too).
          SCREENSHOT="$TMP_OUT/ui-editor-select.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_TEST_SELECT=PlayButton RAE_UI_EDITOR_TREE=1 \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-select.log" 2>&1 \
             && grep -qE "\[ui-editor\] selected PlayButton: Rect,.*OnClick" "$TMP_OUT/render-select.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot-select.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  inspector select failed:"
            cat "$TMP_OUT/render-select.log" "$TMP_OUT/screenshot-select.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The Rect group's text inputs (#44133406), driven through the SAME
          # functions the keys call (RectEdit.rae), never a back door. Four
          # runs, each pinning one rule of the editor:
          #   Tab commits the field it leaves and moves on (x=5, then y=7)
          #   Esc drops the draft: typed 99, nothing written, h still mixed
          #   an untyped MIXED field commits nothing — tabbing through it must
          #     not flatten a selection that disagrees to one value
          #   the W/H lock keeps EACH entity's own ratio: Card 300x460 locked
          #     to w=150 gets h=230, CardCost 60x40 to w=120 gets h=80
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 RAE_UI_EDITOR_TEST_SELECT=PlayButton \
             RAE_UI_EDITOR_TEST_SET_RECT="focus:x type:5 tab type:7 enter" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-recttab.log" 2>&1 \
             && grep -qaF "rect set focus:x type:5 tab type:7 enter -> 2: x 5 layout  y 7 layout" "$TMP_OUT/render-recttab.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 RAE_UI_EDITOR_TEST_SELECT=PlayButton,ContinueButton \
                RAE_UI_EDITOR_TEST_SET_RECT="focus:h type:99 esc focus:h enter" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-rectesc.log" 2>&1 \
             && grep -qaF "rect set focus:h type:99 esc focus:h enter -> 0: x 0 layout  y 0 layout  w 0 fill  h —" "$TMP_OUT/render-rectesc.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/Card.raescene" \
                RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 RAE_UI_EDITOR_TEST_SELECT=Card \
                RAE_UI_EDITOR_TEST_SET_RECT="lock w=150" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-rectlock.log" 2>&1 \
             && grep -qaF "rect set lock w=150 -> 1: x 0  y 0  w 150  h 230" "$TMP_OUT/render-rectlock.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/Card.raescene" \
                RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 RAE_UI_EDITOR_TEST_SELECT=CardCost \
                RAE_UI_EDITOR_TEST_SET_RECT="lock w=120" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-rectlock2.log" 2>&1 \
             && grep -qaF "rect set lock w=120 -> 1: x 0 layout  y 0 layout  w 120  h 80" "$TMP_OUT/render-rectlock2.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  rect text inputs failed:"
            cat "$TMP_OUT/render-recttab.log" "$TMP_OUT/render-rectesc.log" "$TMP_OUT/render-rectlock.log" "$TMP_OUT/render-rectlock2.log" 2>/dev/null | grep -a "rect " | cut -c1-200 | sed 's/^/    /'
          fi
          # Canvas selection gestures (#20778145). One run replays a script of
          # real canvas clicks through the live gesture path, so what is gated
          # is the gesture, not a back door into the selection set:
          #   PlayButton          plain click -> exactly the deepest hit
          #   shift:ContinueButton  Shift adds
          #   cmd:PlayButton        Cmd toggles that one back out
          #   shift:at:4,4          a MODIFIED click on empty canvas does
          #                         nothing (near-misses must not destroy a set)
          #   at:4,4                a PLAIN click on empty canvas clears it —
          #                         the deselect that did not exist before
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_CLICKS="PlayButton shift:ContinueButton cmd:PlayButton shift:at:4,4" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-clicks.log" 2>&1 \
             && grep -qF "clicks [PlayButton shift:ContinueButton cmd:PlayButton shift:at:4,4] -> ContinueLabel" "$TMP_OUT/render-clicks.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_CLICKS="PlayButton shift:ContinueButton at:4,4" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-deselect.log" 2>&1 \
             && grep -qF "clicks [PlayButton shift:ContinueButton at:4,4] -> none" "$TMP_OUT/render-deselect.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  canvas selection failed:"
            cat "$TMP_OUT/render-clicks.log" "$TMP_OUT/render-deselect.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # Empty-canvas drag marquee (#82302705). The scripted band uses the
          # same begin/move/finish functions as live input and every result
          # still passes through applySelect. It pins all four modifier modes
          # plus the four-pixel slop: `small` must preserve the Play group.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_MARQUEES="PlayButton small shift:ContinueButton cmd:PlayButton alt:ContinueButton" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-marquee.log" 2>&1 \
             && grep -qF "marquee PlayButton -> PlayButton,PlayIcon,PlayLabel" "$TMP_OUT/render-marquee.log" \
             && grep -qF "marquee small -> PlayButton,PlayIcon,PlayLabel" "$TMP_OUT/render-marquee.log" \
             && grep -qF "marquee shift:ContinueButton -> PlayButton,PlayIcon,PlayLabel,ContinueButton,ContinueLabel" "$TMP_OUT/render-marquee.log" \
             && grep -qF "marquee cmd:PlayButton -> ContinueButton,ContinueLabel" "$TMP_OUT/render-marquee.log" \
             && grep -qF "marquee alt:ContinueButton -> none" "$TMP_OUT/render-marquee.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  canvas marquee selection failed:"
            cat "$TMP_OUT/render-marquee.log" 2>/dev/null | grep -av '^\[present\]' | sed 's/^/    /'
          fi
          # The rubber band itself is authored chrome, pinned as pixels while
          # the preview hook holds it over PlayButton for one frame.
          SCREENSHOT="$TMP_OUT/ui-editor-marquee.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_MARQUEES="preview:PlayButton" RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-marquee-preview.log" 2>&1 \
             && python3 tools/assert_bmp_diff.py "$SCREENSHOT" \
                  "../examples/121_ui_editor/references/Marquee.png" \
                  --skip-size-mismatch > "$TMP_OUT/screenshot-marquee.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  canvas marquee chrome failed:"
            cat "$TMP_OUT/render-marquee-preview.log" "$TMP_OUT/screenshot-marquee.log" 2>/dev/null | grep -av '^\[present\]' | sed 's/^/    /'
          fi
          # The inspector is MULTI-SELECTION only (#82314313): it renders the
          # COMPOSITE of the whole selection, and n=1 is that same code path
          # with one member rather than a separate single-selection mode.
          # Two runs, and the pair is the point:
          #   n=1 — the component line carries NO (have/total) suffix, because
          #     every count equals the total. Identical to what a single
          #     selection always showed, which is what proves there is no
          #     second rendering path to drift.
          #   n=2 — a Button plus its own Label: the header becomes
          #     "Selection (2): <ids>", the line is the UNION of both nodes'
          #     components, and everything only ONE of them carries is marked
          #     partial. Rect/Size/Name/NodeId stay bare because both have them.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-inspect1.log" 2>&1 \
             && grep -qaF "inspector: PlayButton | PlayButton | Rect, Size, Layout, Shape, OnClick, Name, NodeId, Children" "$TMP_OUT/render-inspect1.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton cmd:PlayLabel" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-inspect2.log" 2>&1 \
             && grep -qaF "Selection (2): PlayButton,PlayLabel" "$TMP_OUT/render-inspect2.log" \
             && grep -qaF "Rect, Size, Layout (1/2), Shape (1/2), OnClick (1/2), Name, NodeId, Children (1/2), Text (1/2)" "$TMP_OUT/render-inspect2.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  composite inspector failed:"
            cat "$TMP_OUT/render-inspect1.log" "$TMP_OUT/render-inspect2.log" 2>/dev/null | grep -av '^\[present\]' | sed 's/^/    /'
          fi
          # Generated component widgets (#24323144). No widget is written per
          # component: every ComponentTable of the world is walked by
          # reflection and its fields rendered over the selection. Three runs:
          #   n=1 PlayButton — Layout's enum field shows the current member AND
          #     the picker from enumMembers; OnClick, Shape, Name, NodeId all
          #     get rows with no code naming them; Rect is bespoke (no rows);
          #     Children has only a hidden List(EntityId) so it gets no header.
          #   n=2 Button+Label — a component only one of them carries heads its
          #     rows "Layout (1/2)"; a shared field folds to its value.
          #   Coverage FadedBox / HugText — the hand-written override table adds
          #     Opacity's range+step; HugText (parent Layout AND own Align) is
          #     the node that used to crash RectWidget's driver fold.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-widgets1.log" 2>&1 \
             && grep -qaF "select parent:; filter: / to search components:; Rect: lock w/h=off x=0 layout y=0 layout w=0 fill h=128; Size: w=SizeAxis { mode: fill, min: -1, max: -1 } h=SizeAxis { mode: fixed, min: -1, max: -1 }; Layout: kind=horizontal [none|horizontal|vertical|grid|stack] gap=16 · from spaceS" "$TMP_OUT/render-widgets1.log" \
             && grep -qaF "; OnClick: actionId=menu.play maxDelayMs=250; Name: label=PlayButton · computed; NodeId: id=PlayButton · computed" "$TMP_OUT/render-widgets1.log" \
             && ! grep -qaF "Children:" "$TMP_OUT/render-widgets1.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton cmd:PlayLabel" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-widgets2.log" 2>&1 \
             && grep -qaF "widgets select parent:; filter: / to search components:; Rect: lock w/h=off x=0 layout y=0 layout w=— — h=—; Size: w=— h=SizeAxis { mode: fixed, min: -1, max: -1 }; Layout (1/2): kind=horizontal" "$TMP_OUT/render-widgets2.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/Coverage.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_HEADLESS_FRAMES=3 RAE_FIXED_DT=0.05 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="FadedBox" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-widgets3.log" 2>&1 \
             && grep -qaF "Opacity: value=0.35 [0..1] ±0.05" "$TMP_OUT/render-widgets3.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/Coverage.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_HEADLESS_FRAMES=3 RAE_FIXED_DT=0.05 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="HugText" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-widgets4.log" 2>&1 \
             && grep -qaF "inspector: HugText | HugText | Rect, Size, Layout, Padding, Align" "$TMP_OUT/render-widgets4.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  generated component widgets failed:"
            cat "$TMP_OUT/render-widgets1.log" "$TMP_OUT/render-widgets2.log" "$TMP_OUT/render-widgets3.log" "$TMP_OUT/render-widgets4.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-400 | sed 's/^/    /'
          fi
          # Rows carry type + preview, and the header a node count and the
          # filter (#89663819). Two runs:
          #   unfiltered — the header reads the row count
          #   filtered — `stat` keeps the Stats subtree AND its ancestor
          #     Screen, because a filter that hid non-matching ANCESTORS would
          #     cut the matches out of the tree entirely; the header shows
          #     both the narrowed count and the filter text
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="Screen" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-treehdr.log" 2>&1 \
             && grep -qaF "tree header: Hierarchy (40)" "$TMP_OUT/render-treehdr.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="treeFilter:stat Screen" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-treefilter.log" 2>&1 \
             && grep -qaF "tree rows (11): Screen,Stats,StatWins,StatWinsValue," "$TMP_OUT/render-treefilter.log" \
             && grep -qaF "tree header: Hierarchy (11)  filter: stat" "$TMP_OUT/render-treefilter.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hierarchy row labels / filter failed:"
            cat "$TMP_OUT/render-treehdr.log" "$TMP_OUT/render-treefilter.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-260 | sed 's/^/    /'
          fi
          # The selected-row PLATE (#38090472). A screenshot diff, not a log
          # line: this is a purely visual change, and the whole requirement is
          # that it "look right with MANY rows selected". Three rows selected
          # must match the checked-in TreeSelection.png, in which the ACTIVE
          # row (LogoRing) is a brighter token than the two other selected
          # rows — one flat colour could not say which row the inspector
          # describes or which one the arrows walk from.
          SCREENSHOT="$TMP_OUT/ui-editor-treeplate.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1500 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="Title cmd:Tagline cmd:LogoRing" \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-treeplate.log" 2>&1 \
             && grep -qaF "Title,Tagline,LogoRing active LogoRing" "$TMP_OUT/render-treeplate.log" \
             && python3 tools/assert_bmp_diff.py "$SCREENSHOT" \
                  "../examples/121_ui_editor/references/TreeSelection.png" \
                  --skip-size-mismatch > "$TMP_OUT/screenshot-treeplate.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hierarchy selected-row plate failed:"
            cat "$TMP_OUT/render-treeplate.log" "$TMP_OUT/screenshot-treeplate.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-260 | sed 's/^/    /'
          fi
          # Deep reveal has a fixed clip, and refreshes preserve manual scrolling.
          SCREENSHOT="$TMP_OUT/ui-editor-treereveal.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1500 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="Footer" RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-treereveal.log" 2>&1 \
             && grep -qaF "[ui-editor] hierarchy scroll: -943" "$TMP_OUT/render-treereveal.log" \
             && python3 tools/assert_bmp_diff.py "$SCREENSHOT" \
                  "../examples/121_ui_editor/references/TreeReveal.png" \
                  > "$TMP_OUT/screenshot-treereveal.log" 2>&1 \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="Footer treeScroll:-82 Footer" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-treemanual.log" 2>&1 \
             && grep -qaF "[ui-editor] hierarchy scroll: -82" "$TMP_OUT/render-treemanual.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="Footer Screen" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-treeup.log" 2>&1 \
             && grep -qaE "hierarchy scroll: -?0$" "$TMP_OUT/render-treeup.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hierarchy deep reveal / manual scroll preservation failed:"
            cat "$TMP_OUT/render-treereveal.log" "$TMP_OUT/screenshot-treereveal.log" \
                "$TMP_OUT/render-treemanual.log" "$TMP_OUT/render-treeup.log" 2>/dev/null | tail -25
          fi
          # Collapse / expand the hierarchy (#72481430). Three runs:
          #   collapse hides a subtree — LogoRing's four descendants leave the
          #     visible rows, 40 -> 32, and expanding restores them
          #   a Shift-RANGE spans only VISIBLE rows: Title..Panel is twelve
          #     rows expanded and four with LogoRing collapsed, which is the
          #     whole point of keying the range on the visible list
          #   collapsing KEEPS hidden descendants selected (dropping them would
          #     silently shrink a set the user built)
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="collapse:LogoRing Screen" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-collapse.log" 2>&1 \
             && grep -qaF "tree rows (32): Screen,Eyebrow,Title,Tagline,LogoRing,Panel," "$TMP_OUT/render-collapse.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="collapse:LogoRing expand:LogoRing Screen" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-expand.log" 2>&1 \
             && grep -qaF "tree rows (40): Screen,Eyebrow,Title,Tagline,LogoRing,OrbitOuter," "$TMP_OUT/render-expand.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="collapse:LogoRing Title shift:Panel" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-range.log" 2>&1 \
             && grep -qaF "tree clicks [collapse:LogoRing Title shift:Panel] -> Title,Tagline,LogoRing,Panel active Panel" "$TMP_OUT/render-range.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="OrbitOuter cmd:OrbitInner collapse:LogoRing" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-keep.log" 2>&1 \
             && grep -qaF "OrbitOuter,OrbitInner active OrbitInner" "$TMP_OUT/render-keep.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hierarchy collapse/expand failed:"
            cat "$TMP_OUT/render-collapse.log" "$TMP_OUT/render-expand.log" "$TMP_OUT/render-range.log" "$TMP_OUT/render-keep.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-260 | sed 's/^/    /'
          fi
          # Select parent over the SET, and the identity block (#23728694).
          # The click-through gesture walks ONE node up the stack under the
          # cursor; this walks the whole selection up the tree, which a gesture
          # cannot express. Two selected SIBLINGS must collapse to their one
          # shared parent — the dedup is the whole point, or selecting two
          # children of a Button would "select parent" into the same node
          # twice. The identity block is n=1 only and carries the entity id,
          # node id, name, primary type, parent and children, so the ids in
          # diagnostics can be matched by eye.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayLabel cmd:PlayIcon parent" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-parent.log" 2>&1 \
             && grep -qaF "select parent (1): PlayButton" "$TMP_OUT/render-parent.log" \
             && grep -qaF "identity: entity=#10 gen 0 node=PlayButton name=PlayButton type=(none) parent=Panel #9 children=PlayIcon #11, PlayLabel #12" "$TMP_OUT/render-parent.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayLabel cmd:ContinueLabel parent" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-parent2.log" 2>&1 \
             && grep -qaF "select parent (2): PlayButton,ContinueButton" "$TMP_OUT/render-parent2.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  select parent / identity failed:"
            cat "$TMP_OUT/render-parent.log" "$TMP_OUT/render-parent2.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-300 | sed 's/^/    /'
          fi
          # Add / remove a component, and the filter (#57156455). Three runs,
          # each going through the same functions the panel's own rows call:
          #   remove over a MULTI-selection strips the component only from the
          #     nodes that have it — PlayButton has Shape and PlayLabel does
          #     not, so "from 1" of a 2-node selection is the whole asymmetry
          #   add applies to EVERY selected node that lacks it ("to 2")
          #   the filter narrows BOTH lists at once: with `tran` no present
          #     component matches and the add list is just TransformFx, which
          #     is what makes a ~79-entry add list usable at all
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton cmd:PlayLabel" \
             RAE_UI_EDITOR_TEST_COMPONENT="remove:Shape" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-comp1.log" 2>&1 \
             && grep -qaF "confirm removing Shape" "$TMP_OUT/render-comp1.log" \
             && grep -qaF "removed Shape from 1" "$TMP_OUT/render-comp1.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton cmd:PlayLabel" \
                RAE_UI_EDITOR_TEST_COMPONENT="add:Constraints" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-comp2.log" 2>&1 \
             && grep -qaF "added Constraints to 2" "$TMP_OUT/render-comp2.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton" \
                RAE_UI_EDITOR_TEST_COMPONENT="filter:tran" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-comp3.log" 2>&1 \
             && grep -qaF "filter: tran:; add component: TransformFx=" "$TMP_OUT/render-comp3.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  component add/remove/filter failed:"
            cat "$TMP_OUT/render-comp1.log" "$TMP_OUT/render-comp2.log" "$TMP_OUT/render-comp3.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-300 | sed 's/^/    /'
          fi
          # The component rows scroll inside a FIXED clip. A deep virtual-list
          # offset must still paint rows and leave the parent chain in place.
          SCREENSHOT="$TMP_OUT/ui-editor-component-scroll.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1500 \
             RAE_UI_EDITOR_TEST_SELECT="PlayButton" \
             RAE_UI_EDITOR_TEST_COMPONENT="scroll:-280" \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-component-scroll.log" 2>&1 \
             && grep -qaF "component scroll: -280" "$TMP_OUT/render-component-scroll.log" \
             && python3 tools/assert_bmp_diff.py "$SCREENSHOT" \
                  "../examples/121_ui_editor/references/ComponentScroll.png" \
                  --skip-size-mismatch > "$TMP_OUT/screenshot-component-scroll.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  component list scroll failed:"
            cat "$TMP_OUT/render-component-scroll.log" "$TMP_OUT/screenshot-component-scroll.log" 2>/dev/null | grep -av '^\[present\]' | cut -c1-300 | sed 's/^/    /'
          fi
          # What DRIVES a value, inline with it (#21438052). The generated
          # widgets tag each field with where its value came from, read back
          # from the parsed scene:
          #   `· computed`  — nothing in the .raescene authored this component
          #     on this node (Name and NodeId are made by the loader), so the
          #     value cannot be traced to the author at all
          #   `· from <x>`  — the author wrote a spelling such as a theme
          #     token and a fresh decode produces the live typed value
          #   `· changed from <x>` — the live value differs from that fresh
          #     decode, so a runtime system or edit replaced it
          # An authored value still equal to what the author wrote stays quiet,
          # and an enum's picker must NOT be compared (that would call every
          # enum overridden), which `kind=roundedRect` with no tag pins.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-driver.log" 2>&1 \
             && grep -qaF "Name: label=PlayButton · computed" "$TMP_OUT/render-driver.log" \
             && grep -qaF "gap=16 · from spaceS" "$TMP_OUT/render-driver.log" \
             && grep -qaF "fill=RgbaColor { r: 72, g: 180, b: 152, a: 255 } · from accent" "$TMP_OUT/render-driver.log" \
             && grep -qaF "stroke=RgbaColor { r: 0, g: 0, b: 0, a: 0 } · from #00000000" "$TMP_OUT/render-driver.log" \
             && grep -qaF "kind=roundedRect [rect|roundedRect|circle] fill=" "$TMP_OUT/render-driver.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  value provenance failed:"
            grep -av '^\[present\]' "$TMP_OUT/render-driver.log" 2>/dev/null | cut -c1-400 | sed 's/^/    /'
          fi
          # Click-through cycling (#73816378). A canvas click lands on the
          # DEEPEST hit, which is almost never the node you meant; clicking the
          # same spot again steps one level UP the hit stack. Two runs:
          #   the cycle — five clicks on one spot walk PlayLabel -> PlayButton
          #     -> Panel -> Screen -> NOTHING (the deselect step, #66601047),
          #     and a sixth WRAPS back to PlayLabel; the two runs together
          #     prove the stepping, the empty step and the wrap
          #   the reset — a modifier click in between is "another route", so
          #     the next plain click starts again at the deepest node instead
          #     of inheriting a stale depth
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_CLICKS="PlayButton PlayButton PlayButton PlayButton PlayButton" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-cycle.log" 2>&1 \
             && grep -qF "cycle 2/5: PlayButton" "$TMP_OUT/render-cycle.log" \
             && grep -qF "cycle 4/5: Screen" "$TMP_OUT/render-cycle.log" \
             && grep -qF "cycle 5/5: nothing" "$TMP_OUT/render-cycle.log" \
             && grep -qF "clicks [PlayButton PlayButton PlayButton PlayButton PlayButton] -> none" "$TMP_OUT/render-cycle.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_CLICKS="PlayButton PlayButton PlayButton PlayButton PlayButton PlayButton" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-cycle-wrap.log" 2>&1 \
             && grep -qF "clicks [PlayButton PlayButton PlayButton PlayButton PlayButton PlayButton] -> PlayLabel" "$TMP_OUT/render-cycle-wrap.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_CLICKS="PlayButton PlayButton cmd:Title PlayButton" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-cycle-reset.log" 2>&1 \
             && grep -qF "clicks [PlayButton PlayButton cmd:Title PlayButton] -> PlayLabel" "$TMP_OUT/render-cycle-reset.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  click-through cycling failed:"
            cat "$TMP_OUT/render-cycle.log" "$TMP_OUT/render-cycle-wrap.log" "$TMP_OUT/render-cycle-reset.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # Hierarchy multi-selection (#16096994). The tree drives the SAME
          # selection set as the canvas, with the same modifier vocabulary —
          # except that Shift spans a contiguous range of visible rows rather
          # than adding one node, which is the one difference a row order
          # makes. Two runs:
          #   range + subtract — Shift spans PlayButton..ContinueButton, then
          #     Cmd and Alt each drop one row out of the middle of that range
          #   anchor stability — a SECOND Shift+click re-spans from the same
          #     anchor (PlayLabel) instead of ratcheting outward, so a range
          #     that overshot can be shrunk. That is why the anchor is its own
          #     field and is not moved by a Shift+click.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayButton shift:ContinueButton cmd:PlayLabel alt:PlayIcon" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-tree.log" 2>&1 \
             && grep -qF "tree clicks [PlayButton shift:ContinueButton cmd:PlayLabel alt:PlayIcon] -> PlayButton,SecondaryActions,ContinueButton active ContinueButton" "$TMP_OUT/render-tree.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
                RAE_UI_HEADLESS=1 RAE_UI_EDITOR_TREE=1 RAE_SDL_HEADLESS_MS=1200 \
                RAE_UI_EDITOR_TEST_TREE_CLICKS="PlayLabel shift:ContinueButton shift:PlayButton" \
                perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-tree-anchor.log" 2>&1 \
             && grep -qF "tree clicks [PlayLabel shift:ContinueButton shift:PlayButton] -> PlayButton,PlayIcon,PlayLabel active PlayButton" "$TMP_OUT/render-tree-anchor.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  hierarchy multi-selection failed:"
            cat "$TMP_OUT/render-tree.log" "$TMP_OUT/render-tree-anchor.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The Rect component widget (#54986153): the inspector reads and
          # writes a SELECTION SET, never one entity. Two nodes are selected,
          # so `h` starts MIXED ("—" — the buttons differ in height) while the
          # fields they agree on show a value plus what DRIVES it ("0 layout",
          # "0 fill"). One `h=64` edit then goes to BOTH of them, which is what
          # turns the mixed field into an agreeing one — the write-to-all
          # convention every later widget copies.
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_TEST_SELECT=PlayButton,ContinueButton \
             RAE_UI_EDITOR_TEST_SET_RECT=h=64 \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1200 \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-rect.log" 2>&1 \
             && grep -qF "[ui-editor] rect (2): x 0 layout  y 0 layout  w 0 fill  h —" "$TMP_OUT/render-rect.log" \
             && grep -qF "[ui-editor] rect set h=64 -> 2: x 0 layout  y 0 layout  w 0 fill  h 64" "$TMP_OUT/render-rect.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  rect widget failed:"
            grep -v '^\[present\]' "$TMP_OUT/render-rect.log" 2>/dev/null | sed 's/^/    /'
          fi
          # A .raepack as the PROJECT (projectSystem): 106's pack opens with
          # its directory as the scene root, all 23 scenes under it scanned
          # (sorted package paths from the pack's declared `scenes: { root:
          # "assets/scenes" }`, the first — Album — mounted), the chrome's
          # project panel listing them; RAE_UI_EDITOR_TEST_OPEN=10 then opens
          # the tenth row (Home) as a tap would — the reload must name the new
          # file. 106's scenes import Theme, so the only diagnostics left are
          # runtime-loaded textures (album covers, the avatar): the mount
          # lines assert the node counts, not 0 diagnostics.
          SCREENSHOT="$TMP_OUT/ui-editor-project.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/106_mobile_ui/106_mobile_ui.raepack" \
             RAE_UI_EDITOR_TEST_OPEN=10 \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-project.log" 2>&1 \
             && grep -qE '\[ui-editor\] project examples/106_mobile_ui/106_mobile_ui.raepack: 23 scenes under examples/106_mobile_ui/assets/scenes/' "$TMP_OUT/render-project.log" \
             && grep -qE '\[ui-editor\] mounted Album: [1-9][0-9]* nodes' "$TMP_OUT/render-project.log" \
             && grep -qE '\[ui-editor\] frame: iphone-15-pro 1179x2556 column 1080 safe 0,177,0,102' "$TMP_OUT/render-project.log" \
             && grep -qE '\[ui-editor\] open Home \(examples/106_mobile_ui/assets/scenes/Home.raescene\)' "$TMP_OUT/render-project.log" \
             && grep -qE '\[ui-editor\] reloaded examples/106_mobile_ui/assets/scenes/Home.raescene \(\+[1-9][0-9]* -[1-9][0-9]* nodes' "$TMP_OUT/render-project.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot-project.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  project (.raepack) open failed:"
            cat "$TMP_OUT/render-project.log" "$TMP_OUT/screenshot-project.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The file picker (#51700883): RAE_UI_EDITOR_TEST_OPEN_FILE=1 requests
          # the OS dialog after the first frame as O / the Open pill would, and
          # RAE_SDL_FILE_DIALOG_RESULT answers it headlessly. The choice goes
          # through the SAME opener as the boot argument: a 106 pack picked from
          # a bare-scene boot becomes the project (its OWN root — the boot
          # RAE_UI_EDITOR_ROOT does not pin it) and the reload names Album; a
          # broken scene keeps the last good page with the parse error.
          SCREENSHOT="$TMP_OUT/ui-editor-picker.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" RAE_UI_EDITOR_TEST_OPEN_FILE=1 \
             RAE_SDL_FILE_DIALOG_RESULT="examples/106_mobile_ui/106_mobile_ui.raepack" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-picker.log" 2>&1 \
             && grep -qE '\[ui-editor\] mounted MainMenu: [1-9][0-9]* nodes, 0 diagnostics' "$TMP_OUT/render-picker.log" \
             && grep -qE '\[ui-editor\] project examples/106_mobile_ui/106_mobile_ui.raepack: 23 scenes under examples/106_mobile_ui/assets/scenes/' "$TMP_OUT/render-picker.log" \
             && grep -qE '\[ui-editor\] reloaded examples/106_mobile_ui/assets/scenes/Album.raescene \(\+[1-9][0-9]* -[1-9][0-9]* nodes' "$TMP_OUT/render-picker.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot-picker.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  file picker (pack) failed:"
            cat "$TMP_OUT/render-picker.log" "$TMP_OUT/screenshot-picker.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          printf '{ "type": "Scene", "version": 2, "root": "X", "nodes": { broken' > "$TMP_OUT/Broken.raescene"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" RAE_UI_EDITOR_TEST_OPEN_FILE=1 \
             RAE_SDL_FILE_DIALOG_RESULT="$TMP_OUT/Broken.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-picker-broken.log" 2>&1 \
             && grep -qE '\[ui-editor\] reload of .*Broken.raescene kept the last good page: parse error' "$TMP_OUT/render-picker-broken.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  file picker (broken scene) failed:"
            cat "$TMP_OUT/render-picker-broken.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # A SMALL document must not shrink the chrome: the chrome's design
          # space is the window in points and the document is fitted into the
          # edit area by the camera, so after picking a 993x130 row scene the
          # frame is still the full window with the chrome drawn.
          SCREENSHOT="$TMP_OUT/ui-editor-small.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" RAE_UI_EDITOR_TEST_OPEN_FILE=1 \
             RAE_SDL_FILE_DIALOG_RESULT="examples/106_mobile_ui/assets/scenes/TrackRow.raescene" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-small.log" 2>&1 \
             && grep -qE '\[ui-editor\] reloaded examples/106_mobile_ui/assets/scenes/TrackRow.raescene' "$TMP_OUT/render-small.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=20 \
                > "$TMP_OUT/screenshot-small.log" 2>&1 \
             && grep -qE 'non-blank BMP' "$TMP_OUT/screenshot-small.log"; then
            :
          else
            UI_EDITOR_OK=0
            echo "  small document shrank the chrome:"
            cat "$TMP_OUT/render-small.log" "$TMP_OUT/screenshot-small.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The camera (cameraSystem): RAE_UI_EDITOR_ZOOM=2 boots the document at
          # 200% centred in the edit area; the log names the zoom and the
          # frame is the whole window (only the document scaled) — and it must
          # DIFFER from the fitted reference by more than 20% of its pixels, so
          # a zoom that is logged but not rendered (a spurious first-frame
          # resize once re-fitted it) fails.
          SCREENSHOT="$TMP_OUT/ui-editor-zoom.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" RAE_UI_EDITOR_ZOOM=2 \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-zoom.log" 2>&1 \
             && grep -qE '\[ui-editor\] zoom 200% offset' "$TMP_OUT/render-zoom.log" \
             && grep -qE '\[ui-editor\] mounted MainMenu: [1-9][0-9]* nodes, 0 diagnostics' "$TMP_OUT/render-zoom.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot-zoom.log" 2>&1 \
             && grep -qE 'non-blank BMP' "$TMP_OUT/screenshot-zoom.log" \
             && ! python3 tools/assert_bmp_diff.py "$SCREENSHOT" "../examples/121_ui_editor/references/MainMenu.png" \
                  --max-diff-pct 20 >> "$TMP_OUT/screenshot-zoom.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  camera zoom failed:"
            cat "$TMP_OUT/render-zoom.log" "$TMP_OUT/screenshot-zoom.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The outline pools (outlineSystem): RAE_UI_EDITOR_OUTLINES=both boots
          # with the "every node" and "every button" pools on — one authored
          # outline instance per matched document entity, positioned over it —
          # so the frame must differ clearly from the plain reference.
          SCREENSHOT="$TMP_OUT/ui-editor-outlines.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" RAE_UI_EDITOR_OUTLINES=both \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-outlines.log" 2>&1 \
             && grep -qE '\[ui-editor\] mounted MainMenu: [1-9][0-9]* nodes, 0 diagnostics' "$TMP_OUT/render-outlines.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot-outlines.log" 2>&1 \
             && ! python3 tools/assert_bmp_diff.py "$SCREENSHOT" "../examples/121_ui_editor/references/MainMenu.png" \
                  --max-diff-pct 0.3 >> "$TMP_OUT/screenshot-outlines.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  outline pools failed:"
            cat "$TMP_OUT/render-outlines.log" "$TMP_OUT/screenshot-outlines.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          # The save path (documentSystem/DocumentSave, lib/ui/SceneWriter):
          # RAE_UI_EDITOR_TEST_SAVE writes the document after the first frame
          # as Cmd+S would; the saved file must open in the editor with the
          # same node count and render pixel-identical to the reference (the
          # unit fixture 884 proves every sample and 106 scene round-trips).
          SAVED="$TMP_OUT/saved-MainMenu.raescene"
          SCREENSHOT="$TMP_OUT/ui-editor-saved.bmp"
          if (cd .. && RAE_UI_EDITOR_SCENE="examples/121_ui_editor/assets/samples/MainMenu.raescene" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" RAE_UI_EDITOR_TEST_SAVE="$SAVED" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-save.log" 2>&1 \
             && grep -qE '\[ui-editor\] saved .*saved-MainMenu.raescene \(40 nodes\)' "$TMP_OUT/render-save.log" \
             && (cd .. && RAE_UI_EDITOR_SCENE="$SAVED" \
             RAE_UI_EDITOR_ROOT="examples/121_ui_editor/assets/samples" \
             RAE_UI_HEADLESS=1 RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 30 "$TMP_OUT/app") > "$TMP_OUT/render-saved.log" 2>&1 \
             && grep -qE '\[ui-editor\] mounted saved-MainMenu: 40 nodes, 0 diagnostics' "$TMP_OUT/render-saved.log" \
             && python3 tools/assert_bmp_diff.py "$SCREENSHOT" "../examples/121_ui_editor/references/MainMenu.png" \
                  --max-diff-pct 0.3 > "$TMP_OUT/screenshot-saved.log" 2>&1; then
            :
          else
            UI_EDITOR_OK=0
            echo "  save path failed:"
            cat "$TMP_OUT/render-save.log" "$TMP_OUT/render-saved.log" "$TMP_OUT/screenshot-saved.log" 2>/dev/null | grep -v '^\[present\]' | sed 's/^/    /'
          fi
          if [ "$UI_EDITOR_OK" = "1" ]; then
            echo "PASS: $EXAMPLE_NAME (7 samples incl. nested sub-scenes and Coverage with 0 diagnostics + reference diffs, effect motion, watch reload, inspector select, composite multi-selection inspector, generated component widgets, value provenance, component add/remove/filter, select parent + identity, hierarchy collapse/expand, selected-row plate, row labels + tree filter, canvas click + marquee selection gestures, click-through cycling, hierarchy multi-selection, Rect widget multi-select read+write, Rect text inputs + lock, wheel routing, hover glow, hover leak, .raepack project open + switch, file picker, camera zoom, outline pools, save path)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (ui editor sample gate)"
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "106_mobile_ui" ]; then
          # The mobile UI (#1023): a Home screenshot proves the scene-driven
          # pages render and the action audit line proves every id the world
          # can fire has an action-table row (actionSystem/ActionAudit); a
          # paused headless run's last `[loop]` line must report
          # `appSystems=0` — an idle frame runs no app binding system
          # (observeSystem/AppObservation); a scripted drag (RAE_UI_DRAG, inputSystem/ScrollInput)
          # on the Library page pulls past the laid-out floor of its authored
          # `ScrollRoot`, rubber-bands, and the spring settles EXACTLY at that
          # floor — the `[scroll-drag] settled` line's scrollY must equal its
          # floor, which verifies drag -> release -> spring without a pointer.
          # A fake Spotify snapshot (RAE_UI_SPOTIFY_FAKE) proves the
          # now-playing text is a binding: spotifyBindingSystem writes the
          # TextBinding nodes from SpotifyState (spotifySystem/SpotifyView).
          # Finally a Library -> album tap runs the hero transition as an
          # ECS entity (ui/animationSystem/HeroTransitionSystem): the log
          # must show it start (t=0, both rects) and land (done), and the
          # resting frame after it is non-blank.
          SCREENSHOT="$TMP_OUT/mobile-ui.bmp"
          if (cd .. && RAE_UI_SCREEN=home RAE_UI_NO_SPOTIFY=1 RAE_AUTO_EXIT_SEC=1 \
             RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot.log" 2>&1 \
             && grep -aEq '\[action-audit\] ids=[1-9][0-9]* unknown=0$' "$TMP_OUT/render.log" \
             && (cd .. && RAE_UI_SCREEN=home RAE_UI_NO_SPOTIFY=1 RAE_UI_PAUSED=1 RAE_UI_DEBUG_LOOP=1 \
             RAE_AUTO_EXIT_SEC=2 RAE_SDL_HEADLESS_MS=2000 \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/idle.log" 2>&1 \
             && [ "$(tr '\r' '\n' < "$TMP_OUT/idle.log" | grep -a 'appSystems=' | tail -1 | grep -c 'appSystems=0$')" -eq 1 ] \
             && (cd .. && RAE_UI_SCREEN=library RAE_UI_NO_SPOTIFY=1 RAE_UI_DRAG=-1500 \
             RAE_AUTO_EXIT_SEC=4 RAE_SDL_HEADLESS_MS=4000 \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/drag.log" 2>&1 \
             && grep -q '\[scroll-drag\] released scrollY=' "$TMP_OUT/drag.log" \
             && grep -Eq '\[scroll-drag\] settled scrollY=(-[0-9.]+) floor=\1$' "$TMP_OUT/drag.log" \
             && (cd .. && RAE_UI_SCREEN=player RAE_UI_SPOTIFY_FAKE='Bound Title|Bound Artist|Bound Album' \
             RAE_UI_PAUSED=1 RAE_AUTO_EXIT_SEC=2 RAE_SDL_HEADLESS_MS=2000 \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/spotify.log" 2>&1 \
             && grep -aq '\[spotify-bind\] title=Bound Title artist=Bound Artist' "$TMP_OUT/spotify.log" \
             && (cd .. && RAE_UI_SCREEN=library RAE_UI_SCREEN_SEQ='album.open.bjork_post' RAE_UI_NO_SPOTIFY=1 \
             RAE_UI_PAUSED=1 RAE_AUTO_EXIT_SEC=3 RAE_SDL_HEADLESS_MS=3000 RAE_GPU2D_SCREENSHOT="$TMP_OUT/hero.bmp" \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/hero.log" 2>&1 \
             && grep -aq '\[hero\] start key=bjork_post from=' "$TMP_OUT/hero.log" \
             && grep -aq '\[hero\] done key=bjork_post' "$TMP_OUT/hero.log" \
             && python3 tools/assert_nonblank_bmp.py "$TMP_OUT/hero.bmp" --min-colors=50 > "$TMP_OUT/hero-shot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (Home screenshot, action audit clean, idle frame runs 0 app systems, scripted drag springs back to the ScrollRoot floor + Spotify text bound + hero transition flies and lands: $(grep '\[scroll-drag\] settled' "$TMP_OUT/drag.log"))"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (mobile UI screenshot / scripted drag gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" "$TMP_OUT/idle.log" "$TMP_OUT/drag.log" "$TMP_OUT/spotify.log" "$TMP_OUT/hero.log" "$TMP_OUT/hero-shot.log" 2>/dev/null | grep -av '^\[present\]' | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "119_ocean_fft" ]; then
          # The realistic water tier on its own (#851): the log line proves the
          # FFT baked and the ocean drew. Then cycle all size/count combinations
          # on hardware, checking exact cadence, wave times and FFT readbacks.
          # Whether the waves look right remains a visual check.
          SCREENSHOT="$TMP_OUT/ocean.bmp"
          if (cd .. && RAE_OCEAN_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=1500 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 45 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && grep -qE '\[ocean\] deterministic deferred frame rendered: hour [0-9.]+, 3 cascades of 256, [1-9][0-9]* ocean frames' "$TMP_OUT/render.log" \
             && grep -q '\[water fft\] spectrum baked' "$TMP_OUT/render.log" \
             && grep -qF '[water comparison] 1 brown island, 3 boxes' "$TMP_OUT/render.log" \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot.log" 2>&1 \
             && (cd .. && RAE_WATER_TIER_CHECK=1 RAE_OCEAN_TEST_FRAME=1 RAE_SDL_HEADLESS_MS=15000 \
                 perl -e 'alarm shift; exec @ARGV' 75 "$TMP_OUT/app") > "$TMP_OUT/tiers.log" 2>&1 \
             && grep -qF '[water tiers] 14 stages ok' "$TMP_OUT/tiers.log" \
             && [ "$(grep -c '\[water fft\] ifft ok' "$TMP_OUT/tiers.log")" -eq 14 ]; then
            echo "PASS: $EXAMPLE_NAME (FFT ocean screenshot + 9 tiers, cadence, invalidation, shutdown)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (ocean render gate)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" "$TMP_OUT/tiers.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "120_resource_lifetimes" ]; then
          # Destructors by name (#881): the compiled binary must run and every
          # voice must release its slot through `drop`, including the field
          # and List-element paths — the log proves the count and the order.
          if (cd .. && perl -e 'alarm shift; exec @ARGV' 20 "$TMP_OUT/app") > "$TMP_OUT/run.log" 2>&1 \
             && [ "$(grep -c '\[voice\] release slot' "$TMP_OUT/run.log")" -eq 7 ] \
             && grep -q '^\[voice\] double note 74 on slot 1$' "$TMP_OUT/run.log" \
             && ! grep -q 'BAD release' "$TMP_OUT/run.log" \
             && grep -q '^\[chord\] C major ends$' "$TMP_OUT/run.log" \
             && [ "$(tail -1 "$TMP_OUT/run.log")" = "[lifetimes] live voices: 0" ]; then
            echo "PASS: $EXAMPLE_NAME (destructors ran for every voice: scope, field, list element, own, explicit; copy took a new slot)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (destructor gate)"
            cat "$TMP_OUT/run.log" | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "118_water_lake" ]; then
          # The toon water on its own (#853): the log line proves the lake was
          # drawn on every rendered frame under the deferred graph; the shot
          # proves a frame came out. Runs from the repo root (lib/*.wgsl).
          SCREENSHOT="$TMP_OUT/water-lake.bmp"
          if (cd .. && RAE_WATER_TEST_FRAME=1 RAE_WATER_FFT_DIAG=1 RAE_SDL_HEADLESS_MS=1200 \
             RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 40 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && grep -qE '\[water example\] deterministic deferred frame rendered: hour [0-9.]+, 1 island, 3 boxes, [1-9][0-9]* water frames' "$TMP_OUT/render.log" \
             && grep -qF '[water comparison] 1 brown island, 3 boxes' "$TMP_OUT/render.log" \
             && grep -q '\[water fft\] h0 ok' "$TMP_OUT/render.log" \
             && grep -q '\[water fft\] ifft ok' "$TMP_OUT/render.log" \
             && [ "$(grep -c "\[water\] toon lake" "$TMP_OUT/render.log")" -eq 1 ] \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=50 \
                > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (toon water lake, free camera, deferred)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (water lake render gate)"
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
          # decode that feeds per-vertex colour). The region check proves the
          # UI is DRAWN, not only created: the corner menu button is a dark
          # square in the top-left. The "panel buttons" log line kept passing
          # while the frame loop rendered an empty UiSystems and no UI showed.
          SHOT="$TMP_OUT/walker.bmp"
          if (cd .. && RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SHOT" \
                perl -e 'alarm shift; exec @ARGV' 45 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SHOT" --min-colors=20 > "$TMP_OUT/shot.log" 2>&1 \
             && python3 tools/assert_bmp_region_dark.py "$SHOT" 70 70 150 150 0.5 >> "$TMP_OUT/shot.log" 2>&1 \
             && [ "$(grep -c "walker: 6717 verts, 3465 triangles" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "skeleton: 65 joints, 76 nodes" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "walk 195 ch" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "palette atlas 512x512" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "skinned parts: 12" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "panel buttons: 4 clip, 0 transport" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "\[water\] toon lake: 2 bodies" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "\[shadow\] casters 130, 3 cascades" "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c "\[seamtest\] .* netOk=true physicsOk=true" "$TMP_OUT/render.log")" -eq 1 ]; then
            echo "PASS: $EXAMPLE_NAME (12 skinned parts + ground casting shadows, clip retargeted, atlas decoded)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (walker character)"
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
            if ! ( (cd .. && RAE_GBUFFER_VIEW="$GB_VIEW" RAE_GBUFFER_DEBUG=1 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$GB_SHOT" \
                  perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/gb-$GB_VIEW.log" 2>&1 \
                  && python3 tools/assert_nonblank_bmp.py "$GB_SHOT" $GB_ARGS >> "$TMP_OUT/gb-$GB_VIEW.log" 2>&1); then
              GB_OK=0
            fi
          done
          # Order is DERIVED from the graph's resource edges, never written,
          # and the pyramid must actually have been built — a silently
          # skipped pass would still leave a correct-looking lit image,
          # since nothing reads the pyramid yet. Every declared pass now
          # executes (renderer_deferred.rae), so the pyramid's presence in
          # the derived pass order (`4: depthPyramid`; waterFft leads since #831) is the built signal —
          # the old per-build `depth pyramid: N mips` debug log was removed.
          if [ "$GB_OK" = "1" ] \
             && [ "$(grep -c '  0: waterFft' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  1: shadow' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  2: gbuffer' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  3: ssao' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  4: depthPyramid' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  6: transparentForward' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  7: underwater' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  8: taa' "$TMP_OUT/gb-lit.log")" -ge 1 ] \
             && [ "$(grep -c '  10: present' "$TMP_OUT/gb-lit.log")" -ge 1 ]; then
            echo "PASS: $EXAMPLE_NAME (lit frame + 4 G-buffer channels, pyramid built, derived pass order)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (deferred G-buffer)"
            cat "$TMP_OUT"/gb-*.log 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "111_metaballs_forward" ]; then
          # 111 is now ALSO the forward-path host of the runtime draw-limit guard
          # (rae_g3d_push_draw_record): a second run under RAE_GPU3D_DRAW_LIMIT=1
          # must log "[gpu3d] ERROR: draw limit exceeded". This moved here from the
          # retired 113_gltf_character (its glTF loading coverage lives in 114).
          # The forward twin: one screenshot is enough. 112 owns the deep
          # coverage (scene switching, pause, settings, free camera); repeating
          # all of it here would double the slowest example in the suite to
          # re-test the UI rather than the renderer, which is the only thing
          # that differs.
          SCREENSHOT="$TMP_OUT/gpu3d-ui-forward.bmp"
          if (cd .. && RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --gpu3d-ui > "$TMP_OUT/screenshot.log" 2>&1 \
             && [ "$(grep -c '\[shadow\] casters 17, 3 cascades' "$TMP_OUT/render.log")" -eq 1 ] \
             && (cd .. && RAE_GPU3D_DRAW_LIMIT=1 RAE_SDL_HEADLESS_MS=800 \
                   perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/drawlimit.log" 2>&1 \
             && [ "$(grep -c "\[gpu3d\] ERROR: draw limit exceeded" "$TMP_OUT/drawlimit.log")" -ge 1 ]; then
            echo "PASS: $EXAMPLE_NAME (forward path, lit frame + shadow cascades, draw-limit guard fires)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (forward 3D/UI screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "112_metaballs_deferred" ]; then
          # The free-camera shot must also be a SHADED SCENE (thousands of
          # colours): a black viewport under an intact UI passed the UI-only
          # check for the #920..#921 slices (the UI drew on a canvas the scene
          # was never composited into).
          SCREENSHOT="$TMP_OUT/gpu3d-ui.bmp"
          FREE_SCREENSHOT="$TMP_OUT/gpu3d-ui-free.bmp"
          PAUSE_SCREENSHOT="$TMP_OUT/gpu3d-ui-pause.bmp"
          SETTINGS_SCREENSHOT="$TMP_OUT/gpu3d-ui-settings.bmp"
          SCENE2_SCREENSHOT="$TMP_OUT/gpu3d-ui-scene2.bmp"
          SCENE3_SCREENSHOT="$TMP_OUT/gpu3d-ui-scene3.bmp"
          if (cd .. && RAE_GPU3D_SDF_TEST_LOG=1 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --gpu3d-ui > "$TMP_OUT/screenshot.log" 2>&1 \
             && [ "$(grep -c '\[gpu3d\] SDF metaballs: count=5' "$TMP_OUT/render.log")" -eq 1 ] \
             && [ "$(grep -c '\[shadow\] casters 17, 3 cascades' "$TMP_OUT/render.log")" -eq 1 ] \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=free RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$FREE_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/free-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$FREE_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/free-screenshot.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$FREE_SCREENSHOT" --min-colors=2000 >> "$TMP_OUT/free-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=pause RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$PAUSE_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/pause-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$PAUSE_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/pause-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=debug RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SETTINGS_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/settings-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SETTINGS_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/settings-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=scene2 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCENE2_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/scene2-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCENE2_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/scene2-screenshot.log" 2>&1 \
             && (cd .. && RAE_GPU3D_UI_TEST_STATE=scene3 RAE_SDL_HEADLESS_MS=1000 RAE_GPU2D_SCREENSHOT="$SCENE3_SCREENSHOT" \
                perl -e 'alarm shift; exec @ARGV' 35 "$TMP_OUT/app") > "$TMP_OUT/scene3-render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCENE3_SCREENSHOT" --gpu3d-ui > "$TMP_OUT/scene3-screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (3D scenes + camera, settings and pause UI screenshots)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (3D/UI screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" "$TMP_OUT/free-render.log" "$TMP_OUT/free-screenshot.log" "$TMP_OUT/pause-render.log" "$TMP_OUT/pause-screenshot.log" "$TMP_OUT/settings-render.log" "$TMP_OUT/settings-screenshot.log" "$TMP_OUT/scene2-render.log" "$TMP_OUT/scene2-screenshot.log" "$TMP_OUT/scene3-render.log" "$TMP_OUT/scene3-screenshot.log" 2>/dev/null | sed 's/^/  /'
            ((FAILED++))
          fi
        elif [ "$EXAMPLE_NAME" = "115_procgen_showcase" ]; then
          # The procgen scene (trees, rocks, the texture swatches over it) must
          # come out SHADED: thousands of colours, not a swatch strip over black.
          SCREENSHOT="$TMP_OUT/procgen.bmp"
          if (cd .. && RAE_SDL_HEADLESS_MS=1500 RAE_GPU2D_SCREENSHOT="$SCREENSHOT" \
             perl -e 'alarm shift; exec @ARGV' 45 "$TMP_OUT/app") > "$TMP_OUT/render.log" 2>&1 \
             && python3 tools/assert_nonblank_bmp.py "$SCREENSHOT" --min-colors=2000 > "$TMP_OUT/screenshot.log" 2>&1; then
            echo "PASS: $EXAMPLE_NAME (shaded procgen scene + texture swatches)"
            ((PASSED++))
          else
            echo "FAIL: $EXAMPLE_NAME (procgen screenshot)"
            cat "$TMP_OUT/render.log" "$TMP_OUT/screenshot.log" 2>/dev/null | sed 's/^/  /'
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

if [ $FAILED -gt 0 ] || [ $PASSED -eq 0 ]; then
  exit 1
fi
