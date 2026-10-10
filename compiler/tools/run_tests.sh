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

# Find all test directories (directories containing Main.rae)
TEST_FILES=$(find "$TEST_DIR" \( -name "Main.rae" -o -name "main.rae" \) | sort)

if [ -z "$TEST_FILES" ]; then
  echo "No test files found in $TEST_DIR"
  exit 0
fi

if [ -n "$TARGET_FILTER" ]; then
  echo "Target filter: $TARGET_FILTER"
  echo
fi

# Targets to test. Compiled is the sole supported target.
#
# The Live/Hybrid (bytecode VM) target was REMOVED (#957). Only the Compiled
# (C backend) target exists; a live/hybrid target filter is rejected.
if [ "$TARGET_FILTER" = "live" ] || [ "$TARGET_FILTER" = "hybrid" ]; then
  echo "The Live/Hybrid (bytecode VM) target was removed (#957); only 'compiled' exists."
  exit 1
fi
TARGETS=("compiled")

# #919: on a full run, `rae format --check` over the whole active tree comes
# BEFORE the compiler suite — an unformatted lib/example/fixture file fails
# the run outright (tools/format-check-tree.sh lists the exclusions).
TREE_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/format-check-tree.sh" ]; then
  if ! bash tools/format-check-tree.sh; then TREE_CHECK_FAILED=1; fi
  echo
fi
# #1016: the banned-terms gate, same place (skipped where no term list exists).
TERMS_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/banned-terms-gate.sh" ]; then
  if ! bash tools/banned-terms-gate.sh; then TERMS_CHECK_FAILED=1; fi
  echo
fi
# The stress cases (docs/stress-tests.md), same place: a "footgun" case that
# stops reproducing FAILS on purpose (the ratchet).
STRESS_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "../stress/run.sh" ]; then
  if ! bash ../stress/run.sh; then STRESS_CHECK_FAILED=1; fi
  echo
fi
# The threaded fixtures under ThreadSanitizer (tools/tsan-check.sh), same place.
TSAN_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/tsan-check.sh" ]; then
  if ! bash tools/tsan-check.sh; then TSAN_CHECK_FAILED=1; fi
  echo
fi
# Every benchmark program still compiles (tools/benchmarks-compile-check.sh).
BENCH_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/benchmarks-compile-check.sh" ]; then
  if ! bash tools/benchmarks-compile-check.sh; then BENCH_CHECK_FAILED=1; fi
  echo
fi
# lib/Float8's x86-64 and wasm lowerings (tools/float8-lowerings-check.sh).
FLOAT8_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/float8-lowerings-check.sh" ]; then
  if ! bash tools/float8-lowerings-check.sh; then FLOAT8_CHECK_FAILED=1; fi
  echo
fi
# The memory-boundary fixtures under AddressSanitizer (tools/asan-check.sh).
ASAN_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/asan-check.sh" ]; then
  if ! bash tools/asan-check.sh; then ASAN_CHECK_FAILED=1; fi
  echo
fi
# lib/'s `when` branches type-checked for every real target
# (tools/check-targets.sh), same place.
TARGETS_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/check-targets.sh" ]; then
  if ! bash tools/check-targets.sh; then TARGETS_CHECK_FAILED=1; fi
  echo
fi
# The generated C compiles without warnings (tools/c-warnings-gate.sh).
WARNINGS_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/c-warnings-gate.sh" ]; then
  if ! bash tools/c-warnings-gate.sh; then WARNINGS_CHECK_FAILED=1; fi
  echo
fi
# Both Rae HTTP benchmark servers against every spec/Http.md rule
# (benchmarks/servers/check.sh --rae, decision F7), same place.
SERVER_CHECK_FAILED=0
if [ -z "$TEST_NAME_FILTER" ] && [ -f "../benchmarks/servers/check.sh" ]; then
  if ! sh ../benchmarks/servers/check.sh --rae; then SERVER_CHECK_FAILED=1; fi
  echo
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
    # #919: the build's format preflight runs in CHECK mode under the suite —
    # a fixture whose source is not canonical fails instead of being rewritten
    # under the runner's feet. The CRLF / missing-final-newline fixtures test
    # the strict lexer on their exact bytes, so the preflight is OFF for them.
    case "$TEST_NAME" in
      345_*|346_*|348_*) export RAE_FORMAT=off ;;
      *) export RAE_FORMAT=check ;;
    esac
    # A declared shader (`shader(files: [...])`) is validated with naga at
    # build time; under the suite a missing naga is an ERROR, never a silent
    # skip (docs/shaders-and-the-compiler.md). `cargo install naga-cli`.
    export RAE_SHADER_VALIDATE="${RAE_SHADER_VALIDATE:-require}"

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
    # A case's own environment: `config.env`, one NAME=value per line, set
    # only for that case's command (passed with `env`, so it never leaks
    # into the next case of a serial run)
    CASE_ENV=()
    if [ -f "$TEST_DIRNAME/config.env" ]; then
      while IFS= read -r env_line || [ -n "$env_line" ]; do
        case "$env_line" in ''|'#'*) ;; *) CASE_ENV+=("$env_line") ;; esac
      done < "$TEST_DIRNAME/config.env"
    fi
    CMD_ARGS=("parse") # Default
    if [ -f "$CMD_FILE" ]; then
      CMD_LINE=$(head -n 1 "$CMD_FILE" | tr -d '\r')
      CMD_LINE=$(echo "$CMD_LINE" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
      if [ -n "$CMD_LINE" ]; then
        read -r -a CMD_ARGS <<< "$CMD_LINE"
        # #995: a `--` on the line means program arguments follow. Re-parse
        # with shell quoting so `run -- a "b c"` yields the two words `a` and
        # `b c`; lines without `--` keep the plain whitespace split above.
        case " $CMD_LINE " in
          *" -- "*|*" --") eval "CMD_ARGS=($CMD_LINE)" ;;
        esac
      fi
    fi

    # Hot-reload is a Live-VM-only mode — its harness always invokes
    # `run --target live --watch`. Live is deprecated/frozen
    # (docs/live-vm-status.md), so hot-reload tests never run, on any
    # target and even when named directly with a filter.
    if [ "${CMD_ARGS[0]}" = "hot-reload" ]; then
      continue
    fi

    # Filtering logic
    RUN_THIS=1
    # The compiled-target skip list below is bypassed by a name filter so a
    # developer can force-run one skipped case. RAE_TEST_APPLY_SKIPS=1 re-applies
    # it under a filter: the parallel runner (tools/run_tests_parallel.sh) drives
    # this script one case at a time and must skip exactly what the full loop
    # skips, or it "runs" the deprecated Live-only cases the loop never does.
    if [ "$TARGET" = "compiled" ] && { [ -z "$TEST_NAME_FILTER" ] || [ "${RAE_TEST_APPLY_SKIPS:-0}" = "1" ]; }; then
        case "$TEST_NAME" in
            # Skip build tests that target live/hybrid specifically
            407_*|408_*|395_*|498_*|499_*|500_*|501_*|502_*|503_*)
                RUN_THIS=0 ;;
            # Skip tests that don't have func main() (C backend expects a main)
            000_*|100_*|101_*|102_*|103_*|104_*|105_*)
                RUN_THIS=0 ;;
            # Skip tests known to have different output or no support in C yet
            306_*|318_*|332_*|334_*|338_*|339_*|343_*|382_*|385_*)
                RUN_THIS=0 ;;
        esac
        # C backend only handles 'run' or 'build' commands in this loop.
        #
        # lex/parse/format/pack are NOT backend commands — they exercise the
        # frontend and are target-independent. They used to run under the live
        # target; when live was frozen they stopped running anywhere, silently
        # dropping 30+ cases. With `compiled` the only target, running them
        # here runs them exactly once. (A crash in `rae format` on any global
        # binding sat undetected behind this gap.)
        case "${CMD_ARGS[0]}" in
            hot-reload) RUN_THIS=0 ;;
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
            # 526 PNG codec: decodePng returns dims via `mod Int` out-params,
            # which the Live VM can't write back (mod-struct fields work, scalar
            # mod doesn't). The codec's target is Compiled anyway.
            526_*)
                RUN_THIS=0 ;;
            # 527 exercises a lodepng-backed oracle extern — compiled-only.
            527_*)
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
        # It expects a Main.rae and a MainV2.rae
        # It will copy Main.rae to a tmp file, start watch run in background,
        # wait, copy MainV2.rae over the tmp file, wait, then stop.
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
        V2_FILE="${TEST_DIRNAME}/MainV2.rae"
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
        # #995: the entry file goes BEFORE a `--`; everything after it is the
        # program's own argv and must stay after the file.
        RUN_OPTS=()
        APP_ARGS=()
        SEEN_DASHDASH=0
        for a in "${CMD_RUN_ARGS[@]:1}"; do
          if [ $SEEN_DASHDASH -eq 1 ]; then APP_ARGS+=("$a")
          elif [ "$a" = "--" ]; then SEEN_DASHDASH=1
          else RUN_OPTS+=("$a"); fi
        done
        CMD_RUN_ARGS=("run" "--target" "$TARGET" "${RUN_OPTS[@]}" "$TEST_FILE")
        if [ $SEEN_DASHDASH -eq 1 ]; then CMD_RUN_ARGS+=("--" "${APP_ARGS[@]}"); fi
    elif [ "${CMD_RUN_ARGS[0]}" = "build" ]; then
        CMD_RUN_ARGS=("build" "${CMD_RUN_ARGS[@]:1}" "$TEST_FILE")
    elif [ $APPEND_TEST_FILE -eq 1 ]; then
      CMD_RUN_ARGS+=("$TEST_FILE")
    fi

    if [ $SKIP_EXEC -eq 0 ]; then
        # Leak-class tests that CALL the mem_stats native opt into
        # RAE_MEM_STATS=1 so `rae_ext_rae_mem_stats_outstanding()` returns real
        # counts. Without this the native is a 0-stub in Live mode (and reads
        # zero in Compiled mode too since RAE_MEM_STATS gates the side-hash), so
        # those leak regressions silently pass. The atexit dump lines are
        # filtered out before stdout comparison so the expected.txt stays simple.
        #
        # #775: the mem-stats side-hash is per-allocation bookkeeping that itself
        # grows RSS. The RSS-based leak tests (431/432/433 measure processRssKb,
        # they do NOT call the native) must therefore run WITHOUT it — with
        # RAE_MEM_STATS=1 their RSS reading is dominated by the tracker's own
        # growth (432 flaked: ~0 KB growth clean vs up to ~2.9 MB against its
        # 3 MB bound under the tracker). Excluding them measures the real program
        # and is deterministic; they keep their own RSS-threshold leak check.
        ENABLE_MEM_STATS=0
        case "$TEST_NAME" in
            43[4-9]_*|449_*|542_*|543_*|545_*|666_*|712_*|757_*|847_*|848_*|866_*) ENABLE_MEM_STATS=1 ;;
        esac
        # A case that READS the counters gets them, found from its source: the
        # name list above missed 918-931 (and 429), whose outstanding count
        # was therefore always 0 — they passed while leaking.
        if grep -q 'rae_ext_rae_mem_stats_' "$TEST_FILE" 2>/dev/null; then
            ENABLE_MEM_STATS=1
        fi
        # Leak check at exit, ON by default (RAE_TEST_MEMCHECK=0 turns it
        # off): every compiled `run` case runs under RAE_MEM_STATS=1, and one
        # that exits with Strings or buffers still allocated FAILS even when
        # its output matches. The exit dump is read before the mem-stats
        # filter drops it. The RSS cases (431-433) keep running without the
        # tracker (see #775 above).
        MEMCHECK=0
        if [ "${RAE_TEST_MEMCHECK:-1}" != "0" ] && [ "${CMD_RUN_ARGS[0]}" = "run" ]; then
            case "$TEST_NAME" in
                43[1-3]_*) ;;
                *) MEMCHECK=1; ENABLE_MEM_STATS=1 ;;
            esac
        fi
        LEAK_MSG=""
        CMD_RAW=""
        APP_EXIT_CODE=""
        # For parse/lex/format, we want to capture both stdout and stderr to see errors + any partial results
        if [[ "${CMD_ARGS[0]}" =~ ^(parse|lex|format)$ ]]; then
            CMD_STDOUT=$(env ${CASE_ENV[@]+"${CASE_ENV[@]}"} "$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
        elif [ $ENABLE_MEM_STATS -eq 1 ]; then
            CMD_RAW=$(env ${CASE_ENV[@]+"${CASE_ENV[@]}"} RAE_MEM_STATS=1 "$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
            # Leak-check only a program that ran to its normal end: one that
            # stops itself on purpose (a fatal report, exit code != 0) still
            # holds its live World, which is not a leak. The driver's
            # `@@RAE_APP_EXIT@@ ... code=N` line carries the program's code.
            APP_EXIT_CODE=$(printf '%s\n' "$CMD_RAW" | sed -n 's/^@@RAE_APP_EXIT@@ .* code=\([0-9-]*\).*/\1/p' | tail -1)
            if [ $MEMCHECK -eq 1 ] && [ "${APP_EXIT_CODE:-0}" = "0" ]; then
                # The program's exit dump is the LAST block: the `rae` driver
                # that ran it prints none (rae_mem_stats_silence_exit_report).
                LEAK_STRINGS=$(printf '%s\n' "$CMD_RAW" | sed -n 's/^  \[mem:string:TOTAL *\].* outstanding=\([0-9-]*\).*/\1/p' | tail -1)
                LEAK_BUFFERS=$(printf '%s\n' "$CMD_RAW" | sed -n 's/^  \[mem:buf *\].* outstanding=\([0-9-]*\).*/\1/p' | tail -1)
                if [ "${LEAK_STRINGS:-0}" != "0" ] || [ "${LEAK_BUFFERS:-0}" != "0" ]; then
                    LEAK_MSG="leak at exit: ${LEAK_STRINGS:-0} strings, ${LEAK_BUFFERS:-0} buffers outstanding"
                fi
            fi
            CMD_STDOUT=$(printf '%s\n' "$CMD_RAW" | grep -Ev '^\[rae (vm )?mem-stats\]|^  \[(mem|vm):' || true)
        else
            CMD_STDOUT=$(env ${CASE_ENV[@]+"${CASE_ENV[@]}"} "$BIN" "${CMD_RUN_ARGS[@]}" 2>&1 || true)
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

        # #763 no-globals WARNING phase: these transitional warnings are emitted
        # to stderr for every module-level `var`/heap-`let` in the imported libs,
        # so a `run` test that pulls in a global-holding lib module would otherwise
        # see them merged into its captured output. Filter them from the
        # comparison (same idea as the mem-stats filter) — EXCEPT the test that
        # asserts them. Removed by #766 when the warning becomes a hard error.
        case "$TEST_NAME" in
            708_no_globals_error) ;;
            *) ACTUAL_OUTPUT=$(printf '%s' "$ACTUAL_OUTPUT" | grep -Ev 'warning: module-level `(var|let)`' || true) ;;
        esac
        # Build/run timing sentinels (`@@RAE_BUILD_TIME@@`, `@@RAE_APP_START@@`,
        # `@@RAE_APP_EXIT@@`): the compiler prints them to stderr on EVERY
        # compiled build/run for the devtools and other log parsers. They carry
        # wall-clock numbers, so they can never be part of an expected output.
        ACTUAL_OUTPUT=$(printf '%s' "$ACTUAL_OUTPUT" | grep -Ev '^@@RAE_(BUILD_TIME|BUILD_PROGRESS|APP_START|APP_EXIT)@@' || true)
    fi

    if [ -n "$TMP_OUTPUT_DIR" ]; then
      # Package summary...
      rm -rf "$TMP_OUTPUT_DIR"
    fi
    
    EXPECTED_OUTPUT=$(cat "$EXPECT_FILE")

    # #916: every `format` case is ALSO checked for idempotence (formatting
    # the output again changes nothing) and AST equivalence (`rae parse` of
    # the input and of the output dump identically — the formatter moved
    # layout, never meaning). A failure of either is reported as the case's
    # failure, so a fixture's expected.txt cannot bless a broken layout.
    FORMAT_CHECK_MSG=""
    # Only the format-to-Rae commands (default / --stdout / --write / --output)
    # produce Rae to re-check; --check/--json/--rules and an over-cap/parse
    # error output are not Rae, so skip the idempotence + AST re-check there.
    FORMAT_EMITS_RAE=1
    for a in "${CMD_ARGS[@]}"; do
      case "$a" in --check|--json|--rules|--stdin) FORMAT_EMITS_RAE=0 ;; esac
    done
    case "$ACTUAL_OUTPUT" in error:*) FORMAT_EMITS_RAE=0 ;; esac
    if [ "${CMD_ARGS[0]}" = "format" ] && [ "$FORMAT_EMITS_RAE" = "1" ] && [ "${EXPECTED_OUTPUT:0:6}" != "REGEX:" ]; then
        FMT_TMP_DIR=$(mktemp -d)
        printf '%s\n' "$ACTUAL_OUTPUT" > "$FMT_TMP_DIR/Main.rae"
        FMT_SECOND=$("$BIN" format --stdout "$FMT_TMP_DIR/Main.rae" 2>&1 || true)
        if [ "$FMT_SECOND" != "$ACTUAL_OUTPUT" ]; then
            FORMAT_CHECK_MSG="format is not idempotent: a second pass changed the output"
        fi
        # The input may lack the final newline the strict lexer demands (the
        # formatter is lenient about it); compare the dumps of two temp copies.
        cp "$TEST_FILE" "$FMT_TMP_DIR/Input.rae"
        [ -n "$(tail -c 1 "$FMT_TMP_DIR/Input.rae")" ] && printf '\n' >> "$FMT_TMP_DIR/Input.rae"
        FMT_AST_IN=$("$BIN" parse "$FMT_TMP_DIR/Input.rae" 2>&1 | sed "s#$FMT_TMP_DIR/Input.rae#FILE#g" | grep -v '^warning' || true)
        FMT_AST_OUT=$("$BIN" parse "$FMT_TMP_DIR/Main.rae" 2>&1 | sed "s#$FMT_TMP_DIR/Main.rae#FILE#g" | grep -v '^warning' || true)
        if [ "$FMT_AST_IN" != "$FMT_AST_OUT" ]; then
            FORMAT_CHECK_MSG="${FORMAT_CHECK_MSG:+$FORMAT_CHECK_MSG; }format changed the AST (rae parse of input and output differ)"
        fi
        rm -rf "$FMT_TMP_DIR"
    fi

    IS_MATCH=0
    [ "${SKIP_EXEC:-0}" -eq 0 ] || LEAK_MSG=""
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

    if [ $IS_MATCH -eq 1 ] && [ -n "$FORMAT_CHECK_MSG" ]; then
      IS_MATCH=0
      echo "FAIL: $DISPLAY_NAME ($FORMAT_CHECK_MSG)"
      ((FAILED++))
    elif [ $IS_MATCH -eq 1 ] && [ -n "${LEAK_MSG:-}" ]; then
      IS_MATCH=0
      echo "FAIL: $DISPLAY_NAME ($LEAK_MSG)"
      ((FAILED++))
    elif [ $IS_MATCH -eq 1 ]; then
      echo "PASS: $DISPLAY_NAME"
      ((PASSED++))
      PASSED_TEST_NAMES="$PASSED_TEST_NAMES $TEST_NAME.rae"
    else
      echo "FAIL: $DISPLAY_NAME"
      echo "  Expected:"
      echo "$EXPECTED_OUTPUT" | sed 's/^/    /'
      echo "  Actual:"
      echo "$ACTUAL_OUTPUT" | sed 's/^/    /'
      # A compiled run's exit, which the comparison filters out with the
      # driver's @@ lines: a crash, or a program that never started (the build
      # failed) - otherwise a run that printed nothing fails with a blank
      # "Actual" and no clue (999_box3d_ecs_determinism, 2026-10-06).
      if [ -n "$CMD_RAW" ]; then
        if [ -n "$APP_EXIT_CODE" ]; then
          echo "  Program exit code: $APP_EXIT_CODE"
        else
          echo "  Program exit: none - the driver stopped before running it (build failed?)"
        fi
        if [ -z "$ACTUAL_OUTPUT" ]; then
          echo "  Raw output (last lines, unfiltered):"
          printf '%s\n' "$CMD_RAW" | tail -8 | sed 's/^/    /'
        fi
      fi
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

# Update test history. RAE_TEST_NO_HISTORY=1 skips it: the parallel runner
# (tools/run_tests_parallel.sh) invokes this script once per case, and 400
# concurrent writers of stats/test_history.json (plus a `git log` per passed
# test) would race and dominate the wall clock — it updates history once, at
# the end, itself.
if [ -f "tools/update_test_history.py" ] && [ "${RAE_TEST_NO_HISTORY:-0}" != "1" ]; then
  ./tools/update_test_history.py $PASSED_TEST_NAMES
fi

# #917: format-CLI behavior checks, on a full (unfiltered) run only.
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/test-format-cli.sh" ]; then
  echo
  if ! bash tools/test-format-cli.sh; then FAILED=$((FAILED+1)); fi
fi

# #934: package-CLI behavior checks (add/fetch/update/tree), full run only.
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/test-packages-cli.sh" ]; then
  echo
  if ! bash tools/test-packages-cli.sh; then FAILED=$((FAILED+1)); fi
fi

# File, asset and console policy (docs/runtime-c-audit.md row 7): stdin,
# $RAE_STDLIB and a scratch directory, which a single-file fixture cannot set.
# Full run only.
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/test-files-io.sh" ]; then
  echo
  if ! bash tools/test-files-io.sh; then FAILED=$((FAILED+1)); fi
fi
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/test-build-out.sh" ]; then
  echo
  if ! bash tools/test-build-out.sh; then FAILED=$((FAILED+1)); fi
fi

# The browser build (tools/wasm_webgpu_smoke.sh: 109/110 through Emscripten +
# EmdawnWebGPU, ~50 s; SKIP without emcc). Full run only.
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/wasm_webgpu_smoke.sh" ]; then
  echo
  if ! bash tools/wasm_webgpu_smoke.sh; then FAILED=$((FAILED+1)); fi
fi

# #44411932: native file-open dialog binding on its env-driven headless path
# (RAE_SDL_FILE_DIALOG_RESULT / RAE_SDL_HEADLESS_MS), which a single-file
# fixture cannot set per case. Full run only.
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/test-sdl-file-dialog.sh" ]; then
  echo
  if ! bash tools/test-sdl-file-dialog.sh; then FAILED=$((FAILED+1)); fi
fi

# `rae watch` restarts on a `.raescene` change too. Full run only.
if [ -z "$TEST_NAME_FILTER" ] && [ -f "tools/test-watch-scene-data.sh" ]; then
  echo
  if ! bash tools/test-watch-scene-data.sh; then FAILED=$((FAILED+1)); fi
fi

if [ "$TREE_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$TERMS_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$STRESS_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$TSAN_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$ASAN_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$FLOAT8_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$BENCH_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$SERVER_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$TARGETS_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ "$WARNINGS_CHECK_FAILED" = "1" ]; then FAILED=$((FAILED+1)); fi
if [ $FAILED -gt 0 ]; then
  exit 1
fi

# The example smoke tests (run_examples.sh) are the VISUAL gate: they build +
# render every example in a real SDL/GPU window and take screenshots, and take
# minutes. They are NOT part of the default `make test` — the default run is
# the non-visual unit suite only. Opt in with RAE_RUN_EXAMPLES=1 (which is what
# `make test-examples` does); RAE_SKIP_EXAMPLES=1 is still honoured and wins.
# A single named test never runs them.
if [ -z "$TEST_NAME_FILTER" ]; then
  if [ "${RAE_RUN_EXAMPLES:-0}" = "1" ] && [ "${RAE_SKIP_EXAMPLES:-0}" != "1" ]; then
    echo
    ./tools/run_examples.sh
  else
    echo
    echo "Example smoke tests (visual) not run — RAE_RUN_EXAMPLES=1 or 'make test-examples' runs them."
  fi
fi
