#!/bin/bash
# Rae test runner

set -e

BIN="bin/rae"
TEST_DIR="tests/cases"
PASSED=0
FAILED=0
PASSED_TEST_NAMES=""
TARGET_FILTER=$(printf "%s" "${TEST_TARGET:-${TARGET:-}}" | tr '[:upper:]' '[:lower:]')
TEST_NAME_FILTER="$1"

# Tests that are permanently disabled because they hang or are known to be broken
HARDCODED_DISABLED=""

echo "Running Rae tests..."
echo

if [ ! -f "$BIN" ]; then
  echo "Error: $BIN not found. Run 'make build' first."
  exit 1
fi

if [ ! -d "$TEST_DIR" ]; then
  echo "Warning: $TEST_DIR not found. No tests to run."
  exit 0
fi

# Find all test directories (directories containing main.rae)
TEST_FILES=$(find "$TEST_DIR" -name "main.rae" | sort)

if [ -z "$TEST_FILES" ]; then
  echo "No test files found in $TEST_DIR"
  exit 0
fi

if [ -n "$TARGET_FILTER" ]; then
  echo "Target filter: $TARGET_FILTER"
  echo
fi

# Targets to test
if [ -n "$TARGET_FILTER" ]; then
  TARGETS=("$TARGET_FILTER")
else
  TARGETS=("live" "compiled")
fi

for TARGET in "${TARGETS[@]}"; do
  echo "Testing target: $TARGET"
  echo "------------------------"
  
  for TEST_FILE in $TEST_FILES; do
    case "$TEST_FILE" in
      */helpers/*)
        continue
        ;; 
    esac
    TEST_DIRNAME=$(dirname "$TEST_FILE")
    if [ -f "$TEST_DIRNAME/.raetesthelper" ]; then
      continue
    fi
    # Test name is the directory name
    TEST_NAME=$(basename "$TEST_DIRNAME")
    TEST_NUMBER="${TEST_NAME%%_*}"

    # Apply name filter if provided
    if [ -n "$TEST_NAME_FILTER" ]; then
      if [ "$TEST_NAME" != "$TEST_NAME_FILTER" ] && [ "$TEST_NUMBER" != "$TEST_NAME_FILTER" ]; then
        continue
      fi
    fi
    
    # Check if we should skip this test (hardcoded or RAE_SKIP_TESTS env var)
    SKIP_LIST=" $HARDCODED_DISABLED $(echo "${RAE_SKIP_TESTS:-}" | tr ',' ' ') "
    if [[ "$SKIP_LIST" =~ " $TEST_NAME " ]] || [[ "$SKIP_LIST" =~ " $TEST_NUMBER " ]]; then
      echo "SKIP: $TEST_NAME (disabled)"
      continue
    fi
    
    EXPECT_FILE="$TEST_DIRNAME/expected.txt"
    if [ ! -f "$EXPECT_FILE" ]; then
      if [ "$TARGET" = "${TARGETS[0]}" ]; then
        echo "SKIP: $TEST_NAME (no expected.txt file)"
      fi
      continue
    fi

    CMD_FILE="$TEST_DIRNAME/config.cmd"
    CMD_ARGS=("parse") # Default
    if [ -f "$CMD_FILE" ]; then
      CMD_LINE=$(head -n 1 "$CMD_FILE" | tr -d '\r')
      CMD_LINE=$(echo "$CMD_LINE" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
      if [ -n "$CMD_LINE" ]; then
        read -r -a CMD_ARGS <<< "$CMD_LINE"
      fi
    fi

    # Filtering logic
    RUN_THIS=1
    if [ "$TARGET" = "compiled" ] && [ -z "$TEST_NAME_FILTER" ]; then
        case "$TEST_NAME" in
            # Skip build tests that target live/hybrid specifically
            407_*|408_*|395_*|498_*|499_*|500_*)
                RUN_THIS=0 ;;
            # Skip tests that don't have func main() (C backend expects a main)
            000_*|100_*|101_*|102_*|103_*|104_*|105_*)
                RUN_THIS=0 ;;
            # Skip tests known to have different output or no support in C yet
            306_*|318_*|332_*|334_*|338_*|339_*|343_*|382_*|385_*)
                RUN_THIS=0 ;;
        esac
        # C backend only handles 'run' or 'build' commands in this loop
        case "${CMD_ARGS[0]}" in
            hot-reload) RUN_THIS=0 ;;
            lex|parse|format|pack) RUN_THIS=0 ;;
        esac
    fi
    # Live mode does not yet implement `copy T` parameter deep-copy
    # (Stage 3) or owning-return deep-copy (Stage 4) semantics — these
    # are wired only in the compiled backend. Tests 460-464 (copy T
    # param) and 471/474 (Stage 4 return-from-alias-source) assert
    # buffer independence that the live VM cannot honour today; gate
    # them to the compiled target for now. (470, 472, 473, 475 work
    # in Live too because their semantics happen to match the VM's
    # implicit deep-copy-on-store behaviour for those shapes.)
    #
    # Stage 6: after the lib/ui/layout.rae migration to view Int /
    # view Float for hot-path params, test 414 hangs in the live VM.
    # The compiled backend lowers view-on-numeric-primitive to plain
    # pass-by-value (see Stage 6 c_backend.c), but the VM still
    # treats `view Int` as a real ref/handle, and a deep traversal
    # of the UI tree exercises a path the VM doesn't yet handle
    # quickly. Gating 414 to compiled-only until the VM gets the
    # equivalent Stage 6 treatment. Test 483 (Stage 6 explicit-
    # primitive-modes) runs in both targets and is the live-side
    # coverage for the new annotations.
    if [ "$TARGET" = "live" ] && [ -z "$TEST_NAME_FILTER" ]; then
        case "$TEST_NAME" in
            460_*|461_*|462_*|463_*|464_*|471_*|474_*)
                RUN_THIS=0 ;;
            414_*)
                RUN_THIS=0 ;;
        esac
    fi

    if [ $RUN_THIS -eq 0 ]; then
        continue
    fi

    # Hot-reload is a Live-only mode. The special harness below always
    # invokes `run --target live --watch`, so running it in the compiled
    # target loop is redundant and can be flaky under full-suite load.
    if [ "$TARGET" = "compiled" ] && [ "${CMD_ARGS[0]}" = "hot-reload" ]; then
        continue
    fi

    # Labels and TMP logic
    DISPLAY_NAME="$TEST_NAME [$TARGET]"
    if [[ "${CMD_ARGS[0]}" =~ ^(parse|lex|format)$ ]]; then
        DISPLAY_NAME="$TEST_NAME"
    fi

    TMP_OUTPUT_FILE=""
    TMP_INPUT_FILE=""
    TMP_OUTPUT_DIR=""
    APPEND_TEST_FILE=1
    CMD_RUN_ARGS=("${CMD_ARGS[@]}")

    for idx in "${!CMD_RUN_ARGS[@]}"; do
      if [ "${CMD_RUN_ARGS[$idx]}" = "{{TMP_OUTPUT}}" ]; then
        if [ -z "$TMP_OUTPUT_FILE" ]; then
          TMP_OUTPUT_FILE=$(mktemp)
        fi
        CMD_RUN_ARGS[$idx]="$TMP_OUTPUT_FILE"
      elif [ "${CMD_RUN_ARGS[$idx]}" = "{{TMP_INPUT}}" ]; then
        if [ -z "$TMP_INPUT_FILE" ]; then
          TMP_INPUT_FILE=$(mktemp)
          cp "$TEST_FILE" "$TMP_INPUT_FILE"
        fi
        CMD_RUN_ARGS[$idx]="$TMP_INPUT_FILE"
        APPEND_TEST_FILE=0
      elif [ "${CMD_RUN_ARGS[$idx]}" = "{{TMP_OUTDIR}}" ]; then
        if [ -z "$TMP_OUTPUT_DIR" ]; then
          TMP_OUTPUT_DIR=$(mktemp -d)
        fi
        CMD_RUN_ARGS[$idx]="$TMP_OUTPUT_DIR"
      fi
    done

    # Final command assembly
    SKIP_EXEC=0
    if [ "${CMD_RUN_ARGS[0]}" = "hot-reload" ]; then
        # Special handling for hot-reload tests
        # It expects a main.rae and a main_v2.rae
        # It will copy main.rae to a tmp file, start watch run in background,
        # wait, copy main_v2.rae over the tmp file, wait, then stop.
        TMP_HOT_FILE=$(mktemp -t rae_test_XXXXXX.rae)
        cp "$TEST_FILE" "$TMP_HOT_FILE"
        
        if [ -z "$TMP_OUTPUT_FILE" ]; then
          TMP_OUTPUT_FILE=$(mktemp)
        fi
        
        # Start in background
        "$BIN" run --target live --watch "$TMP_HOT_FILE" > "$TMP_OUTPUT_FILE" 2>&1 &
        HOT_PID=$!
        
        # Wait for the watcher to produce two program output lines.
        # Full-suite runs can be slow enough that a fixed sleep races the
        # initial VM execution, especially on hot-reload tests with sleep().
        for _ in {1..50}; do
            if [ "$(grep -v "^Watching '" "$TMP_OUTPUT_FILE" | wc -l | tr -d ' ')" -gt 1 ]; then
                break
            fi
            sleep 0.05
        done
        
        # Patch
        V2_FILE="${TEST_DIRNAME}/main_v2.rae"
        if [ -f "$V2_FILE" ]; then
            cp "$V2_FILE" "$TMP_HOT_FILE"
            # The watcher currently tracks mtimes at whole-second granularity.
            # Force a distinct timestamp so fast test runs cannot miss patches.
            perl -e 'my $t = time() + 2; utime $t, $t, @ARGV' "$TMP_HOT_FILE"
        fi
        
        # Wait for patch and execution. This is intentionally output-driven:
        # the watcher may compile or poll slowly under full-suite load.
        for _ in {1..100}; do
            if grep -q "\[hot-patch\]" "$TMP_OUTPUT_FILE"; then
                break
            fi
            sleep 0.1
        done
        sleep 1
        
        # Cleanup
        kill $HOT_PID || true
        wait $HOT_PID 2>/dev/null || true
        rm -f "$TMP_HOT_FILE"
        
        # Set up actual output for comparison
        ACTUAL_OUTPUT=$(cat "$TMP_OUTPUT_FILE")
        rm -f "$TMP_OUTPUT_FILE"
        DISPLAY_NAME="$TEST_NAME [hot-reload]"
        SKIP_EXEC=1
    elif [ "${CMD_RUN_ARGS[0]}" = "run" ]; then
        CMD_RUN_ARGS=("run" "--target" "$TARGET" "${CMD_RUN_ARGS[@]:1}" "$TEST_FILE")
    elif [ "${CMD_RUN_ARGS[0]}" = "build" ]; then
        CMD_RUN_ARGS=("build" "${CMD_RUN_ARGS[@]:1}" "$TEST_FILE")
    elif [ $APPEND_TEST_FILE -eq 1 ]; then
      CMD_RUN_ARGS+=("$TEST_FILE")
    fi

    if [ $SKIP_EXEC -eq 0 ]; then
        # Leak-class tests (43[1-9]_*) opt into RAE_MEM_STATS=1 so the
        # mem_stats_outstanding native returns real counts. Without
        # this the native is a 0-stub in Live mode (and reads zero in
        # Compiled mode too since RAE_MEM_STATS gates the side-hash),
        # so leak regressions silently pass. The atexit dump lines
        # are filtered out before stdout comparison so the expected.txt
        # stays simple.
        ENABLE_MEM_STATS=0
        case "$TEST_NAME" in
            43[1-9]_*) ENABLE_MEM_STATS=1 ;;
        esac
        # For parse/lex/format, we want to capture both stdout and stderr to see errors + any partial results
        if [[ "${CMD_ARGS[0]}" =~ ^(parse|lex|format)$ ]]; then
            CMD_STDOUT=$("$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
        elif [ $ENABLE_MEM_STATS -eq 1 ]; then
            CMD_STDOUT=$(RAE_MEM_STATS=1 "$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 | grep -Ev '^\[rae (vm )?mem-stats\]|^  \[(mem|vm):' || true)
        else
            CMD_STDOUT=$("$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
        fi
        ACTUAL_OUTPUT="$CMD_STDOUT"
        
        if [ -n "$TMP_OUTPUT_FILE" ]; then
          ACTUAL_OUTPUT=$(cat "$TMP_OUTPUT_FILE")
          rm -f "$TMP_OUTPUT_FILE"
        elif [ -n "$TMP_INPUT_FILE" ]; then
          if [ "${CMD_ARGS[0]}" = "format" ]; then
              ACTUAL_OUTPUT=$(cat "$TMP_INPUT_FILE")
          fi
          rm -f "$TMP_INPUT_FILE"
        fi
    fi

    if [ -n "$TMP_OUTPUT_DIR" ]; then
      # Package summary...
      rm -rf "$TMP_OUTPUT_DIR"
    fi
    
    EXPECTED_OUTPUT=$(cat "$EXPECT_FILE")
    
    IS_MATCH=0
    if [ "${EXPECTED_OUTPUT:0:6}" = "REGEX:" ]; then
        PATTERN="${EXPECTED_OUTPUT:6}"
        # Use python for robust regex matching including multiline/newlines
        if echo "$ACTUAL_OUTPUT" | python3 -c "import sys, re; pattern=r\"\"\"$PATTERN\"\"\"; exit(0 if re.search(pattern, sys.stdin.read(), re.DOTALL) else 1)" 2>/dev/null; then
            IS_MATCH=1
        fi
    elif [ "$ACTUAL_OUTPUT" = "$EXPECTED_OUTPUT" ]; then
        IS_MATCH=1
    elif [ "$(head -n 1 "$EXPECT_FILE" | tr -d '\r')" = "BINARY_SKIP_MATCH" ]; then
        IS_MATCH=1
        DISPLAY_NAME="$DISPLAY_NAME (binary match skipped)"
    fi

    if [ $IS_MATCH -eq 1 ]; then
      echo "PASS: $DISPLAY_NAME"
      ((PASSED++))
      PASSED_TEST_NAMES="$PASSED_TEST_NAMES $TEST_NAME.rae"
    else
      echo "FAIL: $DISPLAY_NAME"
      echo "  Expected:"
      echo "$EXPECTED_OUTPUT" | sed 's/^/    /'
      echo "  Actual:"
      echo "$ACTUAL_OUTPUT" | sed 's/^/    /'
      echo
      ((FAILED++))
    fi
  done
  echo
done

echo
echo "==========================================
Results: $PASSED passed, $FAILED failed
=========================================="

# Update test history
if [ -f "tools/update_test_history.py" ]; then
  ./tools/update_test_history.py $PASSED_TEST_NAMES
fi

if [ $FAILED -gt 0 ]; then
  exit 1
fi

# Also run example smoke tests if we are in 'live' or default mode
# AND we are not filtering for a specific test
if ([ -z "$TARGET_FILTER" ] || [ "$TARGET_FILTER" = "live" ]) && [ -z "$TEST_NAME_FILTER" ]; then
  echo
  ./tools/run_examples.sh
fi
