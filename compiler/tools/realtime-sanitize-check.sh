#!/usr/bin/env bash
# The C side of `realtime` / `noAlloc` (docs/realtime-language-design.md §5.1,
# §5.2): a pre-suite case of every full run, like the TSan gate.
#
# 1. Function effects. The backend writes `realtime` as clang's `nonblocking`
#    and `noAlloc` as `nonallocating` (RAE_NONBLOCKING / RAE_NONALLOCATING,
#    rae_runtime.h). The real-time fixtures' generated C must compile with
#    -Werror=function-effects: clang checks the same promises again, through
#    the runtime's own C. Any clang (Apple's has it).
# 2. RealtimeSanitizer. The same fixtures run under `rae run --sanitize
#    realtime` (LLVM's clang, -fsanitize=realtime) and must print no report and
#    their expected output; and a planted liar, C that promises `realtime` and
#    calls malloc, must be caught, so the gate cannot pass by doing nothing.
#    Apple's clang has no RealtimeSanitizer: this part SKIPs with a message
#    when LLVM's clang is missing (`make setup` installs it with Homebrew;
#    RAE_RTSAN_CC names another).
#
#   bash tools/realtime-sanitize-check.sh          # from compiler/
#   RAE_REALTIME_FILTER="1405_realtime_keyword_accepts" bash tools/realtime-sanitize-check.sh
set -u
cd "$(dirname "$0")/.." || exit 1
RAE="${RAE_BIN:-bin/rae}"
[ -x "$RAE" ] || { echo "error: $RAE not built (make -C compiler build)" >&2; exit 1; }
export RAE_FORMAT=check RAE_PROGRESS=off RAE_TOOLCHAIN_CHECK=off
TIMEOUT_S="${RAE_REALTIME_TIMEOUT:-120}"
TMP="$(mktemp -d -t rae_realtime_XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# Every fixture with `realtime` / `noAlloc` functions that keep their promises
CASES="1395_atomic_signal_ring 1396_spsc_ring_threads 1399_realtime_check_clean 1400_realtime_report \
1405_realtime_keyword_accepts 1408_c_export_calls"
if [ -n "${RAE_REALTIME_FILTER:-}" ]; then CASES="$RAE_REALTIME_FILTER"; fi

FAILED=0
CHECKED=0

# ---- 1. function effects ---------------------------------------------------
for name in $CASES; do
  dir="tests/cases/$name"
  work="$TMP/$name"
  mkdir -p "$work"
  if ! perl -e 'alarm shift; exec @ARGV' 300 "$RAE" build --target compiled --emit-c --project "$dir" \
      --out "$work/out.c" "$dir/Main.rae" > "$work/emit.log" 2>&1; then
    echo "FAIL: realtime effects $name (emit failed)"; tail -5 "$work/emit.log" | sed 's/^/    /'
    FAILED=$((FAILED + 1)); continue
  fi
  if ! clang -fsyntax-only -std=c11 -Wno-everything -Wfunction-effects -Werror=function-effects \
      -I"$work" -Iruntime -I/opt/homebrew/include "$work/out.c" > "$work/effects.log" 2>&1; then
    echo "FAIL: realtime effects $name (clang's -Wfunction-effects)"
    grep -E "error:|note:" "$work/effects.log" | head -8 | sed 's/^/    /'
    FAILED=$((FAILED + 1)); continue
  fi
  CHECKED=$((CHECKED + 1))
done

# ---- 2. RealtimeSanitizer --------------------------------------------------
RTSAN_CC="${RAE_RTSAN_CC:-}"
if [ -z "$RTSAN_CC" ]; then
  for candidate in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang; do
    [ -x "$candidate" ] && { RTSAN_CC="$candidate"; break; }
  done
fi
printf 'int main(void){return 0;}\n' > "$TMP/probe.c"
SANITIZED=0
if [ -z "$RTSAN_CC" ] || ! "$RTSAN_CC" -fsanitize=realtime "$TMP/probe.c" -o "$TMP/probe" >/dev/null 2>&1; then
  echo "SKIP: RealtimeSanitizer (no LLVM clang with -fsanitize=realtime; brew install llvm, or set RAE_RTSAN_CC)"
else
  export RAE_RTSAN_CC="$RTSAN_CC"
  trim() { grep -v '^@@RAE_' "$1" | perl -0pe 's/\n+\z/\n/'; }
  for name in $CASES; do
    dir="tests/cases/$name"
    work="$TMP/$name"
    case_env=()
    if [ -f "$dir/config.env" ]; then
      while IFS= read -r env_line || [ -n "$env_line" ]; do
        case "$env_line" in ''|'#'*) ;; *) case_env+=("$env_line") ;; esac
      done < "$dir/config.env"
    fi
    env ${case_env[@]+"${case_env[@]}"} perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" \
      "$RAE" run --sanitize realtime --project "$dir" "$dir/Main.rae" > "$work/out" 2>&1
    rc=$?
    if grep -q "RealtimeSanitizer" "$work/out"; then
      echo "FAIL: rtsan $name (a RealtimeSanitizer report)"
      grep -A6 "ERROR: RealtimeSanitizer" "$work/out" | head -12 | sed 's/^/    /'
      FAILED=$((FAILED + 1)); continue
    fi
    if [ "$rc" != "0" ]; then
      echo "FAIL: rtsan $name (exit $rc)"; grep -v '^@@RAE_' "$work/out" | tail -5 | sed 's/^/    /'
      FAILED=$((FAILED + 1)); continue
    fi
    if [ -f "$dir/expected.txt" ] && ! diff -q <(trim "$work/out") <(trim "$dir/expected.txt") >/dev/null; then
      echo "FAIL: rtsan $name (output differs from expected.txt)"
      diff <(trim "$work/out") <(trim "$dir/expected.txt") | head -8 | sed 's/^/    /'
      FAILED=$((FAILED + 1)); continue
    fi
    SANITIZED=$((SANITIZED + 1))
  done
  # The planted liar: C that promises `realtime` and allocates must be caught
  liar="$TMP/liar"
  mkdir -p "$liar"
  cat > "$liar/lying.c" <<'C'
#include <stdint.h>
#include <stdlib.h>
/* Promised realtime in Main.rae; it allocates, which only RealtimeSanitizer sees */
void* rae_ext_lyingAlloc(int64_t size) { return malloc((size_t)size); }
C
  cat > "$liar/Main.rae" <<'RAE'
func lyingAlloc(size: Int) realtime unsafe extern ret Ptr

func render(size: view Int) realtime ret Int {
  unsafe {
    let block: Ptr = lyingAlloc(size: size)
  }
  ret size
}

func main() {
  log("rendered {render(size: 64)}")
}
RAE
  RAE_FORMAT=off perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" "$RAE" run --sanitize realtime "$liar/Main.rae" \
    > "$liar/out" 2>&1
  if grep -q "Intercepted call to real-time unsafe function \`malloc\`" "$liar/out"; then
    SANITIZED=$((SANITIZED + 1))
  else
    echo "FAIL: rtsan planted liar (RealtimeSanitizer did not report its malloc)"
    grep -v '^@@RAE_' "$liar/out" | tail -5 | sed 's/^/    /'
    FAILED=$((FAILED + 1))
  fi
fi

if [ "$FAILED" -gt 0 ]; then
  echo "FAIL: realtime-sanitize-check ($FAILED failure(s))"
  exit 1
fi
echo "PASS: realtime-sanitize-check ($CHECKED programs pass -Wfunction-effects, $SANITIZED runs under RealtimeSanitizer as expected)"
