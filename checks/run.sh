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
  if perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" \
      "$RAE" run --target compiled --project "$dir" "$dir/Main.rae" > "$log" 2>&1; then
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
