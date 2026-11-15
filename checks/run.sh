#!/usr/bin/env bash
# checks/run.sh — run every hardware check (checks/README.md) and report one
# line per check. A check is a program that exits 0 when every test in it
# passed, so the verdict is its exit code. Exit 1 if any check fails.
#
#   RAE_CHECK_FILTER="gpuArgs webTask"  runs only the named checks
#   RAE_CHECK_TIMEOUT=120               seconds per check (build + run)
set -u
cd "$(dirname "$0")/.." || exit 1     # -> repo root
RAE="${RAE_BIN:-compiler/bin/rae}"
if [ ! -x "$RAE" ]; then
  echo "error: $RAE not built (make -C compiler build)" >&2
  exit 1
fi
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
TIMEOUT_S="${RAE_CHECK_TIMEOUT:-120}"
TMP="$(mktemp -d -t rae_checks_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

PASSED=0
FAILED=0
for dir in checks/*/; do
  name=$(basename "$dir")
  [ -f "$dir/Main.rae" ] || continue
  if [ -n "${RAE_CHECK_FILTER:-}" ]; then
    case " $RAE_CHECK_FILTER " in
      *" $name "*) ;;
      *) continue ;;
    esac
  fi
  log="$TMP/$name.log"
  # A check with a reference.png is a picture: it runs for RAE_HEADLESS_FRAMES=10 with a
  # screenshot, and the screenshot must match the reference (compiler/tools/assert_bmp_diff.py:
  # tolerant, and skipped at another display scale than the reference's). checks/references.sh
  # writes the references.
  picture=()
  shot="$TMP/$name.bmp"
  if [ -f "$dir/reference.png" ]; then
    picture=(env RAE_HEADLESS_FRAMES=10 RAE_GPU2D_SCREENSHOT="$shot")
  fi
  if perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" \
      ${picture[@]+"${picture[@]}"} "$RAE" run --target compiled --project "$dir" "$dir/Main.rae" > "$log" 2>&1 \
     && { [ ! -f "$dir/reference.png" ] || python3 compiler/tools/assert_bmp_diff.py "$shot" "$dir/reference.png" \
            --skip-size-mismatch >> "$log" 2>&1; }; then
    echo "PASS: $name"
    PASSED=$((PASSED + 1))
  else
    echo "FAIL: $name (exit $?)"
    grep -v '^@@RAE_' "$log" | tail -20 | sed 's/^/  /'
    FAILED=$((FAILED + 1))
  fi
done

echo "checks: $PASSED passed, $FAILED failed"
if [ "$FAILED" -gt 0 ] || [ "$PASSED" -eq 0 ]; then
  exit 1
fi
